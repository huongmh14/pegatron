"""PEGATRON localhost dashboard and single-device MQTT bridge."""
import hmac
import json
import logging
import os
import socket
import subprocess
import time
from contextlib import asynccontextmanager
from pathlib import Path
from threading import Event as ThreadEvent, RLock, Thread
from typing import Annotated, Literal
from uuid import uuid4

import paho.mqtt.client as mqtt
from fastapi import FastAPI, HTTPException, Query, Request
from fastapi.responses import FileResponse, StreamingResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, ConfigDict, Field, ValidationError, model_validator
from starlette.middleware.trustedhost import TrustedHostMiddleware

from protocol import (ACTIONS, FRESH_SECONDS, WIFI_SECONDS, Ack, Availability, Command, CommandInput,
                      Event, StrictModel, Telemetry, WifiConnectInput, WifiResult, topic_prefix)
from storage import Store
from windows_wifi import get_current_windows_ssid, get_current_windows_wifi, scan_windows_networks

LOG = logging.getLogger("pegatron")
STATIC = Path(__file__).with_name("static")
MESSAGES = {"telemetry": Telemetry, "availability": Availability, "events": Event, "ack": Ack}
WALL_START = time.time()
COUNTER_START = time.perf_counter()
DEFAULT_STATION_CONFIG = {
    "l_min_mm": 22.0,
    "l_max_mm": 100.0,
    "m_min_mm": 7.0,
    "m_max_mm": 20.0,
    "station_1_on_ms": 220,
    "station_1_delay_ms": 0,
    "station_2_on_ms": 220,
    "station_2_delay_ms": 0,
    "min_off_ms": 120,
}


class StationConfigInput(BaseModel):
    model_config = ConfigDict(extra="forbid")
    l_min_mm: float = Field(ge=0.0, le=299.0)
    l_max_mm: float = Field(ge=1.0, le=300.0)
    m_min_mm: float = Field(ge=0.0, le=299.0)
    m_max_mm: float = Field(ge=1.0, le=300.0)
    station_1_on_ms: int = Field(ge=50, le=600)
    station_1_delay_ms: int = Field(ge=0, le=0)
    station_2_on_ms: int = Field(ge=50, le=600)
    station_2_delay_ms: int = Field(ge=0, le=0)
    min_off_ms: int = Field(ge=40, le=500)

    @model_validator(mode="after")
    def validate_ranges(self):
        if self.l_min_mm >= self.l_max_mm:
            raise ValueError("l_max_mm must exceed l_min_mm")
        if self.m_min_mm >= self.m_max_mm:
            raise ValueError("m_max_mm must exceed m_min_mm")
        return self


def wall_time():
    # On Windows, successive time.time() calls can be identical even across DB writes.
    return WALL_START + (time.perf_counter() - COUNTER_START)


def _c_escape(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"')


def _detect_lan_ipv4() -> str:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.connect(("8.8.8.8", 80))
        return sock.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        sock.close()


def _provision_local_uf2(ssid: str, password: str, broker_ipv4: str) -> tuple[bool, str]:
    root = Path(__file__).resolve().parent.parent
    build_dir = root / "build"
    ninja = Path.home() / ".pico-sdk" / "ninja" / "v1.13.2" / "ninja.exe"
    if not build_dir.is_dir() or not ninja.is_file():
        return False, "BUILD_FAILED"
    cfg = (
        "#pragma once\n\n"
        f'#define PEGATRON_WIFI_SSID "{_c_escape(ssid)}"\n'
        f'#define PEGATRON_WIFI_PASSWORD "{_c_escape(password)}"\n'
        f'#define PEGATRON_BROKER_IPV4 "{_c_escape(broker_ipv4)}"\n'
        '#define PEGATRON_MQTT_PASSWORD ""\n'
        "#define PEGATRON_REFERENCE_1_MM 82\n"
        "#define PEGATRON_REFERENCE_2_MM 82\n"
    )
    (build_dir / "pegatron_local_config.h").write_text(cfg, encoding="utf-8")
    try:
        proc = subprocess.run(
            [str(ninja), "-C", str(build_dir)],
            capture_output=True,
            text=True,
            timeout=30,
        )
    except (OSError, subprocess.SubprocessError):
        return False, "BUILD_FAILED"
    if proc.returncode != 0:
        LOG.warning("UF2 build failed: %s", proc.stderr or proc.stdout)
        return False, "BUILD_FAILED"
    return True, f"PROVISIONED_UF2:{broker_ipv4}"


class Dashboard:
    def __init__(self, device_id, path, hardware_control=False, estop_absent=False):
        self.device_id = device_id
        self.prefix = topic_prefix(device_id)
        self.simulated = device_id == "sorter-sim"
        self.hardware_control = hardware_control
        self.estop_absent = estop_absent  # Machine has no E-STOP input: null is accepted.
        self.lock = RLock()
        self.wifi_pending = None  # {request_id, kind, at}. The password is never stored.
        self.wifi_last = {"scan": None, "connect": None}
        self.store = Store(path)
        self.received_at, saved = self.store.latest()
        self.latest = Telemetry.model_validate(saved) if saved else None
        self.fresh_at = None
        self.online_link = None
        self.broker_connected = False
        self.config_path = path.with_name(f"{device_id}_config.json")
        self.station_config = self._load_config()

    def _load_config(self):
        if self.config_path.is_file():
            try:
                raw = json.loads(self.config_path.read_text(encoding="utf-8"))
                return StationConfigInput.model_validate(raw).model_dump()
            except (OSError, json.JSONDecodeError, ValidationError):
                pass
        return dict(DEFAULT_STATION_CONFIG)

    def get_config(self):
        with self.lock:
            return dict(self.station_config)

    def save_config(self, cfg):
        validated = StationConfigInput.model_validate(cfg).model_dump()
        with self.lock:
            self.station_config = validated
            self.config_path.parent.mkdir(parents=True, exist_ok=True)
            self.config_path.write_text(json.dumps(validated, indent=2), encoding="utf-8")
            return dict(self.station_config)

    def broker_status(self, connected):
        with self.lock:
            self.broker_connected = connected
            self.online_link = None
            self.fresh_at = None

    def fresh(self):
        return (self.broker_connected and self.online_link is not None and self.latest is not None
                and self.latest.link_id == self.online_link and self.fresh_at is not None
                and time.monotonic() - self.fresh_at < FRESH_SECONDS)

    def handle_message(self, leaf, payload, retained=False):
        if leaf == "console" and not retained and len(payload) <= 8192:
            try:
                data = json.loads(payload)
                line = data.get("line") if isinstance(data, dict) else None
            except (ValueError, UnicodeDecodeError):
                return
            if isinstance(line, str) and line and len(line) <= 4096:
                with self.lock:
                    if self.broker_connected:
                        self.store.record_log(wall_time(), line)
            return
        if leaf not in MESSAGES or len(payload) > 8192 or (retained and leaf != "availability"):
            return
        try:
            message = MESSAGES[leaf].model_validate_json(payload)
        except ValidationError:
            LOG.warning("Invalid MQTT payload on %s", leaf)
            return

        with self.lock:
            if not self.broker_connected:
                return
            now = wall_time()
            if isinstance(message, Availability):
                if message.status == "online" and self.online_link != message.link_id:
                    self.online_link = message.link_id
                    self.fresh_at = None
                if message.status == "offline" and self.online_link == message.link_id:
                    self.online_link = None
                    self.fresh_at = None
            elif isinstance(message, Telemetry):
                if message.link_id != self.online_link:
                    return
                previous = self.latest
                if previous and message.boot_id == previous.boot_id and (
                    message.seq <= previous.seq or message.uptime_ms < previous.uptime_ms
                ):
                    return
                gap = not self.fresh() or previous.boot_id != message.boot_id
                if self.store.record_sample(now, message, previous, gap):
                    self.latest = message
                    self.received_at = now
                    self.fresh_at = time.monotonic()
            elif isinstance(message, Event):
                self.store.record_event(now, message)
            elif isinstance(message, Ack):
                self.store.record_ack(now, message)

    def wifi_result(self, payload):
        try:
            message = WifiResult.model_validate_json(payload)
        except ValidationError:
            LOG.warning("Invalid MQTT payload on wifi/result")
            return
        with self.lock:
            pending = self.wifi_pending
            if not pending or pending["request_id"] != message.request_id:
                return
            self.wifi_pending = None
            self.wifi_last[message.kind] = {
                "request_id": message.request_id, "ok": message.ok, "reason": message.reason,
                "ssid": message.ssid, "at": wall_time(),
                "networks": sorted((n.model_dump() for n in message.networks), key=lambda n: -n["rssi"])}

    def wifi_state(self):
        with self.lock:
            pending = self.wifi_pending
            if pending and wall_time() - pending["at"] > WIFI_SECONDS:
                self.wifi_last[pending["kind"]] = {"request_id": pending["request_id"], "ok": False,
                                                   "reason": "TIMEOUT", "ssid": None, "networks": [],
                                                   "at": wall_time()}
                pending = self.wifi_pending = None
            return {"online": self.fresh(), "pending": pending, "device_id": self.device_id,
                    "source": "windows" if self.simulated else ("device" if self.fresh() else "windows_provision"),
                    "host_ssid": None if self.simulated else get_current_windows_ssid(),
                    **self.wifi_last}

    def wifi_send(self, body, kind, publish, allow_offline=False):
        with self.lock:
            if self.wifi_state()["pending"]:
                raise HTTPException(409, "WIFI_BUSY")
            if not self.fresh() and not allow_offline and not (self.simulated and kind == "scan"):
                raise HTTPException(409, "OFFLINE")
            request_id = uuid4().hex
            payload = {"schema_version": 1, "request_id": request_id, "action": kind, **body}
            if not publish(json.dumps(payload, ensure_ascii=False), "wifi/request"):
                raise HTTPException(503, "MQTT_SEND_FAILED")
            self.wifi_pending = {"request_id": request_id, "kind": kind, "at": wall_time()}
            return {"request_id": request_id}

    def blocked_reason(self, action, station, now):
        if not self.fresh():
            return "OFFLINE"
        if not (self.simulated or self.hardware_control):
            return "MONITOR_ONLY"
        if self.latest.estop is not False and not (self.estop_absent and self.latest.estop is None):
            return "ESTOP_UNKNOWN" if self.latest.estop is None else "ESTOP"
        permission = getattr(self.latest.permissions, action)
        if station is not None:
            permission = getattr(permission, f"station_{station}")
        if not permission.allowed:
            return permission.reason
        if self.store.in_flight(now, action):
            return "COMMAND_PENDING"
        return None

    def status(self):
        with self.lock:
            now = wall_time()
            self.store.expire_commands(now)
            online = self.fresh()
            run = self.store.active_run()
            sample = self.latest
            trays = sample.trays.model_dump() if sample else dict.fromkeys(("S", "M", "L"))
            controls = {}
            for action in ACTIONS:
                if action in ("TEST_PUSHER", "CALIBRATE_TOF"):
                    controls[action] = {str(i): self.blocked_reason(action, i, now) for i in (1, 2)}
                else:
                    controls[action] = self.blocked_reason(action, None, now)
            return {"device_id": self.device_id, "simulated": self.simulated, "online": online,
                    "broker_connected": self.broker_connected, "received_at": self.received_at,
                    "snapshot": sample.model_dump() if sample else None, "controls": controls,
                    "confirmed_per_second": self.store.recent_rate(self.received_at, sample) if online else None,
                    "active_run": run, "run_counts": self.store.run_totals(run, now, trays)}

    def send(self, request, publish):
        with self.lock:
            now = wall_time()
            reason = self.blocked_reason(request.action, request.station, now)
            if reason:
                raise HTTPException(409, reason)
            command = Command(schema_version=1, request_id=uuid4().hex, link_id=self.latest.link_id,
                              control_epoch=self.latest.control_epoch, observed_uptime_ms=self.latest.uptime_ms,
                              **request.model_dump())
            self.store.record_command(now, command)
            if not publish(command.model_dump_json()):
                self.store.uncertain(command.request_id, "MQTT_SEND_UNCONFIRMED")
            return self.store.command(command.request_id)


class Bridge:
    def __init__(self, dashboard):
        self.dashboard = dashboard
        self.host = os.getenv("PEGATRON_MQTT_HOST", "127.0.0.1")
        self.port = int(os.getenv("PEGATRON_MQTT_PORT", "1883"))
        self.stop = ThreadEvent()
        self.client = None
        self.thread = Thread(target=self.run, name="mqtt-bridge", daemon=True)

    def run(self):
        while not self.stop.is_set():
            # A new client on every connection drops Paho's old QoS 1 outgoing queue.
            client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="pegatron-ui-" + uuid4().hex[:10],
                                 clean_session=True, protocol=mqtt.MQTTv311, reconnect_on_failure=False)
            client.connect_timeout = 2.0
            client.max_queued_messages_set(8)
            if username := os.getenv("PEGATRON_MQTT_USER"):
                client.username_pw_set(username, os.getenv("PEGATRON_MQTT_PASSWORD"))
            client.on_connect = self.on_connect
            client.on_message = self.on_message
            self.client = client
            try:
                client.connect(self.host, self.port, keepalive=10)
                while not self.stop.is_set() and client.loop(timeout=0.2) == mqtt.MQTT_ERR_SUCCESS:
                    pass
            except OSError as error:
                LOG.info("MQTT connection unavailable: %s", error)
            finally:
                self.dashboard.broker_status(False)
                self.client = None
                client.disconnect()
            self.stop.wait(1)

    def on_connect(self, client, userdata, flags, reason, properties):
        if reason.is_failure:
            client.disconnect()
            return
        client.subscribe([(f"{self.dashboard.prefix}/{leaf}", int(leaf != "telemetry")) for leaf in MESSAGES]
                         + [(f"{self.dashboard.prefix}/wifi/result", 1), (f"{self.dashboard.prefix}/console", 0)])
        self.dashboard.broker_status(True)
        self.publish(json.dumps(self.dashboard.get_config()), "config/set", retain=True)

    def on_message(self, client, userdata, message):
        prefix = self.dashboard.prefix + "/"
        if message.topic == prefix + "wifi/result":
            if not message.retain and len(message.payload) <= 4096:
                self.dashboard.wifi_result(message.payload)
        elif message.topic.startswith(prefix):
            self.dashboard.handle_message(message.topic[len(prefix):], message.payload, message.retain)

    def publish(self, payload, leaf="command", retain=False):
        client = self.client
        if client is None:
            return False
        try:
            return client.publish(f"{self.dashboard.prefix}/{leaf}", payload, qos=1, retain=retain).rc == mqtt.MQTT_ERR_SUCCESS
        except OSError:
            return False


class RunInput(StrictModel):
    name: Annotated[str, Field(max_length=80)]


def local_write(request: Request):
    # Same-origin writes only. TrustedHostMiddleware also rejects DNS rebinding.
    origin = request.headers.get("origin")
    if origin and not hmac.compare_digest(origin, f"{request.url.scheme}://{request.url.netloc}"):
        raise HTTPException(403, "FOREIGN_ORIGIN")
    if request.headers.get("sec-fetch-site") == "cross-site":
        raise HTTPException(403, "FOREIGN_ORIGIN")


@asynccontextmanager
async def lifespan(app):
    device = os.getenv("PEGATRON_DEVICE_ID", "sorter-sim")
    topic_prefix(device)
    path = Path(os.getenv("PEGATRON_DB", f"data/{device}.sqlite3"))
    dashboard = Dashboard(device, path, os.getenv("PEGATRON_HARDWARE_CONTROL") == "1",
                          os.getenv("PEGATRON_ESTOP_ABSENT") == "1")
    bridge = Bridge(dashboard)
    app.state.dashboard, app.state.bridge = dashboard, bridge
    bridge.thread.start()
    try:
        yield
    finally:
        bridge.stop.set()
        bridge.thread.join(timeout=4)
        if not bridge.thread.is_alive():
            dashboard.store.db.close()


app = FastAPI(title="PEGATRON IoT", lifespan=lifespan, docs_url=None, redoc_url=None)
app.add_middleware(TrustedHostMiddleware, allowed_hosts=["localhost", "127.0.0.1"])
app.mount("/static", StaticFiles(directory=STATIC), name="static")


@app.get("/")
def index():
    return FileResponse(STATIC / "index.html")


@app.get("/api/status")
def status(request: Request):
    return request.app.state.dashboard.status()


@app.get("/api/logs")
def logs(request: Request, limit: int = Query(default=200, ge=1, le=1000)):
    dashboard = request.app.state.dashboard
    with dashboard.lock:
        return dashboard.store.recent_logs(limit)


@app.get("/api/config")
def get_config(request: Request):
    return request.app.state.dashboard.get_config()


@app.post("/api/config")
def save_config(data: StationConfigInput, request: Request):
    local_write(request)
    saved = request.app.state.dashboard.save_config(data.model_dump())
    published = request.app.state.bridge.publish(json.dumps(saved), "config/set", retain=True)
    return {"ok": True, "config": saved, "published": published}


@app.post("/api/commands", status_code=202)
def send_command(data: CommandInput, request: Request):
    local_write(request)
    return request.app.state.dashboard.send(data, request.app.state.bridge.publish)


@app.get("/api/commands/{request_id}")
def command_status(request_id: str, request: Request):
    dashboard = request.app.state.dashboard
    with dashboard.lock:
        dashboard.store.expire_commands(wall_time())
        result = dashboard.store.command(request_id)
        if not result:
            raise HTTPException(404, "COMMAND_NOT_FOUND")
        return result


@app.get("/api/wifi")
def wifi(request: Request):
    return request.app.state.dashboard.wifi_state()


@app.post("/api/wifi/scan", status_code=202)
def wifi_scan(request: Request):
    local_write(request)
    dashboard = request.app.state.dashboard
    if not dashboard.simulated and dashboard.fresh():
        return dashboard.wifi_send({}, "scan", request.app.state.bridge.publish)
    sent = dashboard.wifi_send({}, "scan", lambda *_: True, allow_offline=True)

    def scan_host():
        result = {"schema_version": 1, **sent, "kind": "scan", "ok": True,
                  "reason": "OK", "networks": []}
        try:
            result["networks"] = scan_windows_networks()
        except OSError as error:
            result.update(ok=False, reason="WIFI_PERMISSION_DENIED" if getattr(error, "winerror", None) == 5
                          else str(error) if str(error) in ("WINDOWS_WIFI_ONLY", "NO_WIFI_ADAPTER")
                          else "HOST_SCAN_FAILED")
        dashboard.wifi_result(json.dumps(result).encode())

    Thread(target=scan_host, daemon=True).start()
    return sent


@app.post("/api/wifi/connect", status_code=202)
def wifi_connect(data: WifiConnectInput, request: Request):
    local_write(request)
    dashboard = request.app.state.dashboard
    if dashboard.simulated:
        raise HTTPException(409, "SIMULATED_DEVICE")
    if dashboard.fresh():
        body = {"ssid": data.ssid, "password": data.password}
        return dashboard.wifi_send(body, "connect", request.app.state.bridge.publish)
    sent = dashboard.wifi_send({}, "connect", lambda *_: True, allow_offline=True)

    def provision_host():
        ok, reason = _provision_local_uf2(data.ssid, data.password, _detect_lan_ipv4())
        result = {"schema_version": 1, **sent, "kind": "connect", "ok": ok,
                  "reason": reason, "ssid": data.ssid, "networks": []}
        dashboard.wifi_result(json.dumps(result).encode())

    Thread(target=provision_host, daemon=True).start()
    return sent


@app.post("/api/wifi/connect-host", status_code=202)
def wifi_connect_host(request: Request):
    local_write(request)
    dashboard = request.app.state.dashboard
    if dashboard.simulated:
        raise HTTPException(409, "SIMULATED_DEVICE")
    ssid, password = get_current_windows_wifi()
    if not ssid:
        raise HTTPException(409, "HOST_WIFI_NOT_CONNECTED")
    try:
        validated = WifiConnectInput(ssid=ssid, password=password or "")
    except ValidationError as err:
        raise HTTPException(422, "BAD_CREDENTIALS") from err
    sent = dashboard.wifi_send({}, "connect", lambda *_: True, allow_offline=True)

    def provision_host():
        ok, reason = _provision_local_uf2(validated.ssid, validated.password, _detect_lan_ipv4())
        result = {"schema_version": 1, **sent, "kind": "connect", "ok": ok,
                  "reason": reason, "ssid": validated.ssid, "networks": []}
        dashboard.wifi_result(json.dumps(result).encode())

    Thread(target=provision_host, daemon=True).start()
    return {"request_id": sent["request_id"], "ssid": validated.ssid}


@app.get("/api/runs")
def runs(request: Request):
    dashboard = request.app.state.dashboard
    with dashboard.lock:
        return dashboard.store.runs()


@app.post("/api/runs", status_code=201)
def start_run(data: RunInput, request: Request):
    local_write(request)
    dashboard = request.app.state.dashboard
    with dashboard.lock:
        if not dashboard.fresh():
            raise HTTPException(409, "OFFLINE")
        if dashboard.store.active_run():
            raise HTTPException(409, "RUN_OPEN")
        return dashboard.store.start_run(data.name.strip() or time.strftime("Buổi chạy %d/%m %H:%M"), wall_time())


@app.post("/api/runs/{run_id}/end")
def end_run(run_id: int, request: Request):
    local_write(request)
    dashboard = request.app.state.dashboard
    with dashboard.lock:
        run = dashboard.store.end_run(run_id, wall_time())
        if not run:
            raise HTTPException(409, "RUN_NOT_OPEN")
        return run


@app.get("/api/history")
def history(request: Request, run_id: int | None = None, format: Literal["json", "csv"] = "json",
            limit: Annotated[int, Query(ge=1, le=3600)] = 300):
    dashboard = request.app.state.dashboard
    with dashboard.lock:
        now = wall_time()
        dashboard.store.expire_commands(now)
        run = dashboard.store.run(run_id) if run_id is not None else None
        if run_id is not None and not run:
            raise HTTPException(404, "RUN_NOT_FOUND")
        start, end = (run["started_at"], run["ended_at"] or now) if run else (now - 300, now)
        if format == "csv":
            filename = f"pegatron-run-{run_id}.csv" if run else "pegatron-recent.csv"
            return StreamingResponse(dashboard.store.export(start, end), media_type="text/csv",
                                     headers={"Content-Disposition": f'attachment; filename="{filename}"'})
        return {"run": run, "start": start, "end": end, **dashboard.store.history(start, end, limit)}

