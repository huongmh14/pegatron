"""USB Serial -> MQTT bridge streaming live Pico 2 W logs to the dashboard."""
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import json
import re
import signal
import sys
from threading import Thread
import time
from typing import Any

import paho.mqtt.client as mqtt

from protocol import topic_prefix
from windows_wifi import scan_windows_networks

STATUS_RE = re.compile(
    r"TRAM1:\s*(?P<s1>(?:d=\s*(?:----|\d+)mm\s*\[[^\]]+\]|\S+\s*!))\s+"
    r"TRAM2:\s*(?P<s2>(?:d=\s*(?:----|\d+)mm\s*\[[^\]]+\]|\S+\s*!))"
)
DIST_RE = re.compile(r"d=\s*(?P<dist>----|\d+)mm\s*\[\s*(?P<state>[A-Z0-9_?]+)\s*\|\s*#\s*(?P<count>\d+)\]")
PASS_S_RE = re.compile(r"\[TRAM2\]\s+QUA TRAM:.*size=S\s*->\s*PASS")
CALIB_C1_RE = re.compile(r"\[TRAM(?P<st>[12])\]\s+CALIB C1\b.*H0=(?P<h0>[0-9.]+)mm")
TEST_ACT_RE = re.compile(r"\[TRAM(?P<st>[12])\]\s+TEST: servo day\b")


def _perm(allowed: bool, reason: str = "OK") -> dict[str, Any]:
    return {"allowed": allowed, "reason": "OK" if allowed else reason}


def format_config_command(cfg: dict[str, Any]) -> bytes | None:
    try:
        l_min = float(cfg["l_min_mm"])
        l_max = float(cfg["l_max_mm"])
        m_min = float(cfg["m_min_mm"])
        m_max = float(cfg["m_max_mm"])
        s1_on = int(cfg["station_1_on_ms"])
        s1_td = int(cfg["station_1_delay_ms"])
        s2_on = int(cfg["station_2_on_ms"])
        s2_td = int(cfg["station_2_delay_ms"])
        min_off = int(cfg["min_off_ms"])
    except (KeyError, TypeError, ValueError):
        return None
    if not (
        0.0 <= l_min < l_max <= 300.0
        and 0.0 <= m_min < m_max <= 300.0
        and 50 <= s1_on <= 600
        and s1_td == 0
        and 50 <= s2_on <= 600
        and s2_td == 0
        and 40 <= min_off <= 500
    ):
        return None
    return (
        f"k {l_min:.1f} {l_max:.1f} {m_min:.1f} {m_max:.1f} "
        f"{s1_on} {s1_td} {s2_on} {s2_td} {min_off}\n"
    ).encode("ascii")


class SerialState:
    """Tracks conveyor telemetry and produces strict protocol v1 payloads."""

    def __init__(self, device_id: str = "pico-01", default_h0: float = 82.0) -> None:
        self.device_id = device_id
        self.prefix = topic_prefix(device_id)
        now_token = str(int(time.time()))
        self.boot_id = f"usb-{now_token}"
        self.link_id = f"serial-{now_token}"
        self.started_monotonic = time.monotonic()
        self.seq = 0
        self.control_epoch = 1
        self.mode = "sorting"
        self.trays = {"S": 0, "M": 0, "L": 0}
        self.faults: list[str] = []
        self.stations: dict[int, dict[str, Any]] = {
            1: {
                "distance_mm": None,
                "height_mm": None,
                "sample_valid": False,
                "home": None,
                "push_count": 0,
                "reference_mm": round(float(default_h0), 1),
                "sensor_ok": True,
            },
            2: {
                "distance_mm": None,
                "height_mm": None,
                "sample_valid": False,
                "home": None,
                "push_count": 0,
                "reference_mm": round(float(default_h0), 1),
                "sensor_ok": True,
            },
        }

    def _refresh_faults(self) -> None:
        faults: list[str] = []
        if not self.stations[1]["sensor_ok"]:
            faults.append("TOF_UNAVAILABLE_1")
        if not self.stations[2]["sensor_ok"]:
            faults.append("TOF_UNAVAILABLE_2")
        if self.mode == "fault":
            faults.append("FAULT_LOCKED")
        self.faults = faults

    def _update_station_segment(self, station_id: int, segment: str) -> None:
        st = self.stations[station_id]
        match = DIST_RE.search(segment)
        if not match:
            st["sensor_ok"] = False
            st["sample_valid"] = False
            st["distance_mm"] = None
            st["height_mm"] = None
            return

        st["sensor_ok"] = True
        cnt = int(match.group("count"))
        if cnt > st["push_count"]:
            st["push_count"] = cnt
        if station_id == 1 and cnt > self.trays["L"]:
            self.trays["L"] = cnt
        if station_id == 2 and cnt > self.trays["M"]:
            self.trays["M"] = cnt

        raw = match.group("dist")
        if raw == "----":
            st["sample_valid"] = False
            st["distance_mm"] = None
            st["height_mm"] = None
        else:
            dist = int(raw)
            h0 = float(st["reference_mm"])
            height = max(-2000.0, min(2000.0, round(h0 - dist, 1)))
            st["sample_valid"] = True
            st["distance_mm"] = dist
            st["height_mm"] = height

    def parse_line(self, line: str) -> bool:
        """Parses one line from Pico USB serial. Returns True if telemetry should publish."""
        text = line.strip()
        if not text:
            return False

        if "[CLI] -> PAUSE" in text:
            self.mode = "idle"
            self.control_epoch += 1
            self._refresh_faults()
            return True
        if "[CLI] -> START: RUNNING" in text:
            self.mode = "sorting"
            self.control_epoch += 1
            self._refresh_faults()
            return True
        if "LOI QUA NHIET" in text or "-> FAULT!" in text:
            self.mode = "fault"
            self.control_epoch += 1
            self._refresh_faults()
            return True

        calib_m = CALIB_C1_RE.search(text)
        if calib_m:
            sid = int(calib_m.group("st"))
            h0 = max(-2000.0, min(2000.0, round(float(calib_m.group("h0")), 1)))
            self.stations[sid]["reference_mm"] = h0
            if self.stations[sid]["distance_mm"] is not None:
                d = float(self.stations[sid]["distance_mm"])
                self.stations[sid]["height_mm"] = max(-2000.0, min(2000.0, round(h0 - d, 1)))
            return True

        if PASS_S_RE.search(text):
            self.trays["S"] += 1
            return True

        act_m = TEST_ACT_RE.search(text)
        if act_m and self.mode == "idle":
            sid = int(act_m.group("st"))
            self.stations[sid]["push_count"] += 1
            return True

        status_m = STATUS_RE.search(text)
        if status_m:
            self._update_station_segment(1, status_m.group("s1"))
            self._update_station_segment(2, status_m.group("s2"))
            self._refresh_faults()
            return True

        return False

    def build_availability(self, status: str = "online") -> dict[str, Any]:
        return {
            "schema_version": 1,
            "link_id": self.link_id,
            "status": status,
        }

    def build_telemetry(self) -> dict[str, Any]:
        self.seq += 1
        uptime_ms = max(1, int((time.monotonic() - self.started_monotonic) * 1000))

        def st_payload(sid: int) -> dict[str, Any]:
            s = self.stations[sid]
            return {
                "height_mm": s["height_mm"],
                "distance_mm": s["distance_mm"],
                "sample_valid": bool(s["sample_valid"]),
                "home": s["home"],
                "push_count": int(s["push_count"]),
                "reference_mm": s["reference_mm"],
            }

        def test_or_calib_perm(sid: int) -> dict[str, Any]:
            if self.mode == "fault":
                return _perm(False, "FAULT_LOCKED")
            if self.mode != "idle":
                return _perm(False, "IDLE_AND_HOME_REQUIRED")
            if not self.stations[sid]["sensor_ok"]:
                return _perm(False, f"TOF_UNAVAILABLE_{sid}")
            return _perm(True)

        enable_perm = (
            _perm(False, "FAULT_LOCKED")
            if self.mode == "fault"
            else _perm(self.mode == "idle", "ALREADY_SORTING")
        )
        pause_perm = _perm(self.mode == "sorting", "ALREADY_PAUSED")

        return {
            "schema_version": 1,
            "boot_id": self.boot_id,
            "link_id": self.link_id,
            "seq": int(self.seq),
            "uptime_ms": int(uptime_ms),
            "control_epoch": int(self.control_epoch),
            "mode": self.mode,
            "estop": None,
            "faults": list(self.faults),
            "station_1": st_payload(1),
            "station_2": st_payload(2),
            "trays": {
                "S": int(self.trays["S"]),
                "M": int(self.trays["M"]),
                "L": int(self.trays["L"]),
            },
            "permissions": {
                "ENABLE_SORTING": enable_perm,
                "PAUSE_SORTING": pause_perm,
                "TEST_PUSHER": {
                    "station_1": test_or_calib_perm(1),
                    "station_2": test_or_calib_perm(2),
                },
                "CALIBRATE_TOF": {
                    "station_1": test_or_calib_perm(1),
                    "station_2": test_or_calib_perm(2),
                },
            },
        }

    def command_to_serial(self, action: str, station: int | None) -> tuple[bytes | None, str]:
        if action == "ENABLE_SORTING":
            if self.mode == "fault":
                return None, "FAULT_LOCKED"
            if self.mode == "sorting":
                return None, "ALREADY_SORTING"
            self.mode = "sorting"
            self.control_epoch += 1
            return b"p\n", "OK"
        if action == "PAUSE_SORTING":
            if self.mode != "sorting":
                return None, "ALREADY_PAUSED"
            self.mode = "idle"
            self.control_epoch += 1
            return b"p\n", "OK"
        if action == "TEST_PUSHER":
            sid = station or 1
            if self.mode != "idle":
                return None, "IDLE_AND_HOME_REQUIRED"
            if not self.stations[sid]["sensor_ok"]:
                return None, f"TOF_UNAVAILABLE_{sid}"
            return (b"1\n" if sid == 1 else b"2\n"), "OK"
        if action == "CALIBRATE_TOF":
            sid = station or 1
            if self.mode != "idle":
                return None, "IDLE_AND_HOME_REQUIRED"
            if not self.stations[sid]["sensor_ok"]:
                return None, f"TOF_UNAVAILABLE_{sid}"
            return b"c\n", "CALIBRATED"
        return None, "NOT_SUPPORTED"


class _DCB(ctypes.Structure):
    _fields_ = [
        ("DCBlength", wintypes.DWORD),
        ("BaudRate", wintypes.DWORD),
        ("flags", wintypes.DWORD),
        ("wReserved", wintypes.WORD),
        ("XonLim", wintypes.WORD),
        ("XoffLim", wintypes.WORD),
        ("ByteSize", ctypes.c_ubyte),
        ("Parity", ctypes.c_ubyte),
        ("StopBits", ctypes.c_ubyte),
        ("XonChar", ctypes.c_char),
        ("XoffChar", ctypes.c_char),
        ("ErrorChar", ctypes.c_char),
        ("EofChar", ctypes.c_char),
        ("EvtChar", ctypes.c_char),
        ("wReserved1", wintypes.WORD),
    ]


class _COMMTIMEOUTS(ctypes.Structure):
    _fields_ = [
        ("ReadIntervalTimeout", wintypes.DWORD),
        ("ReadTotalTimeoutMultiplier", wintypes.DWORD),
        ("ReadTotalTimeoutConstant", wintypes.DWORD),
        ("WriteTotalTimeoutMultiplier", wintypes.DWORD),
        ("WriteTotalTimeoutConstant", wintypes.DWORD),
    ]


class WinSerialPort:
    """Minimal Win32 serial port wrapper using kernel32 (no pyserial needed)."""

    _INVALID_HANDLE = wintypes.HANDLE(-1).value
    _GENERIC_RW = 0x80000000 | 0x40000000
    _OPEN_EXISTING = 3

    def __init__(self, port: str, baud: int = 115200) -> None:
        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        k32.CreateFileW.argtypes = [
            wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
            wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE,
        ]
        k32.CreateFileW.restype = wintypes.HANDLE
        k32.GetCommState.argtypes = [wintypes.HANDLE, ctypes.POINTER(_DCB)]
        k32.GetCommState.restype = wintypes.BOOL
        k32.SetCommState.argtypes = [wintypes.HANDLE, ctypes.POINTER(_DCB)]
        k32.SetCommState.restype = wintypes.BOOL
        k32.SetCommTimeouts.argtypes = [wintypes.HANDLE, ctypes.POINTER(_COMMTIMEOUTS)]
        k32.SetCommTimeouts.restype = wintypes.BOOL
        k32.ReadFile.argtypes = [
            wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID,
        ]
        k32.ReadFile.restype = wintypes.BOOL
        k32.WriteFile.argtypes = [
            wintypes.HANDLE, wintypes.LPCVOID, wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID,
        ]
        k32.WriteFile.restype = wintypes.BOOL
        k32.CloseHandle.argtypes = [wintypes.HANDLE]
        k32.CloseHandle.restype = wintypes.BOOL
        self._k32 = k32

        path = f"\\\\.\\{port}" if not port.startswith("\\\\.\\") else port
        handle = self._k32.CreateFileW(path, self._GENERIC_RW, 0, None, self._OPEN_EXISTING, 0, None)
        if handle == self._INVALID_HANDLE or handle == 0:
            err = ctypes.get_last_error()
            raise ctypes.WinError(err)
        self._handle = handle
        self._buf = bytearray()

        dcb = _DCB()
        dcb.DCBlength = ctypes.sizeof(_DCB)
        if not self._k32.GetCommState(self._handle, ctypes.byref(dcb)):
            err = ctypes.get_last_error()
            self.close()
            raise ctypes.WinError(err)
        dcb.BaudRate = baud
        dcb.ByteSize = 8
        dcb.Parity = 0
        dcb.StopBits = 0
        # Enable fDtrControl (bit 4) so Pico USB-CDC streams stdio cleanly.
        dcb.flags = (dcb.flags & ~(0x03 << 4)) | (0x01 << 4) | 0x01
        if not self._k32.SetCommState(self._handle, ctypes.byref(dcb)):
            err = ctypes.get_last_error()
            self.close()
            raise ctypes.WinError(err)

        timeouts = _COMMTIMEOUTS(20, 0, 150, 0, 500)
        self._k32.SetCommTimeouts(self._handle, ctypes.byref(timeouts))

    def readline(self) -> str | None:
        if self._handle is None:
            return None
        while b"\n" not in self._buf:
            chunk = ctypes.create_string_buffer(256)
            read_count = wintypes.DWORD(0)
            ok = self._k32.ReadFile(self._handle, chunk, 256, ctypes.byref(read_count), None)
            if not ok:
                raise ctypes.WinError(ctypes.get_last_error())
            if read_count.value == 0:
                return None
            self._buf.extend(chunk.raw[: read_count.value])
        idx = self._buf.index(b"\n")
        raw = bytes(self._buf[:idx])
        del self._buf[: idx + 1]
        return raw.decode("utf-8", errors="replace").rstrip("\r")

    def write(self, data: bytes) -> None:
        if self._handle is None:
            return
        written = wintypes.DWORD(0)
        ok = self._k32.WriteFile(self._handle, data, len(data), ctypes.byref(written), None)
        if not ok:
            raise ctypes.WinError(ctypes.get_last_error())

    def close(self) -> None:
        if self._handle is not None:
            self._k32.CloseHandle(self._handle)
            self._handle = None


def run_bridge(port: str, baud: int, device_id: str, broker_host: str,
               broker_port: int, default_h0: float) -> None:
    state = SerialState(device_id=device_id, default_h0=default_h0)
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"serial-bridge-{device_id}")
    serial_holder: dict[str, WinSerialPort | None] = {"port": None}
    latest_config_cmd: list[bytes | None] = [None]
    stop = False

    def publish(leaf: str, payload: dict[str, Any], qos: int = 0, retain: bool = False) -> None:
        client.publish(f"{state.prefix}/{leaf}", json.dumps(payload), qos=qos, retain=retain)

    def on_connect(cli, userdata, flags, reason, properties):
        if not reason.is_failure:
            cli.subscribe([
                (f"{state.prefix}/command", 1),
                (f"{state.prefix}/wifi/request", 1),
                (f"{state.prefix}/config/set", 1),
            ])
            if serial_holder["port"] is not None:
                publish("availability", state.build_availability("online"), qos=1, retain=True)

    def handle_wifi_request(req: dict[str, Any]) -> None:
        from app import _detect_lan_ipv4, _provision_local_uf2

        req_id = req.get("request_id", "")
        action = req.get("action", "")
        if not req_id:
            return
        if action == "scan":
            result: dict[str, Any] = {
                "schema_version": 1,
                "request_id": req_id,
                "kind": "scan",
                "ok": True,
                "reason": "OK",
                "networks": [],
            }
            try:
                result["networks"] = scan_windows_networks()
            except OSError as error:
                result.update(
                    ok=False,
                    reason="WIFI_PERMISSION_DENIED"
                    if getattr(error, "winerror", None) == 5
                    else str(error)
                    if str(error) in ("WINDOWS_WIFI_ONLY", "NO_WIFI_ADAPTER")
                    else "HOST_SCAN_FAILED",
                )
            publish("wifi/result", result, qos=1)
        elif action == "connect":
            ssid = str(req.get("ssid", ""))
            password = str(req.get("password", ""))
            ok, reason = _provision_local_uf2(ssid, password, _detect_lan_ipv4())
            publish(
                "wifi/result",
                {
                    "schema_version": 1,
                    "request_id": req_id,
                    "kind": "connect",
                    "ok": ok,
                    "reason": reason,
                    "ssid": ssid,
                    "networks": [],
                },
                qos=1,
            )

    def on_message(cli, userdata, msg):
        try:
            cmd = json.loads(msg.payload.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            return
        if msg.topic == f"{state.prefix}/wifi/request":
            Thread(target=handle_wifi_request, args=(cmd,), daemon=True).start()
            return
        if msg.topic == f"{state.prefix}/config/set":
            data = format_config_command(cmd)
            if data is not None:
                latest_config_cmd[0] = data
                if serial_holder["port"] is not None:
                    try:
                        serial_holder["port"].write(data)
                        print("[BRIDGE] Applied config to Pico", flush=True)
                    except OSError:
                        pass
            return
        action = cmd.get("action", "")
        station = cmd.get("station")
        req_id = cmd.get("request_id", "")
        data, reason = state.command_to_serial(action, station)
        executed = data is not None and serial_holder["port"] is not None
        if executed:
            try:
                serial_holder["port"].write(data)
            except OSError:
                executed = False
                reason = "LINK_LOST"
        if req_id:
            ack = {
                "schema_version": 1,
                "request_id": req_id,
                "link_id": state.link_id,
                "result": "executed" if executed else "rejected",
                "reason": reason,
                "mode": state.mode,
            }
            publish("ack", ack, qos=1)
        if executed:
            publish("telemetry", state.build_telemetry())

    client.on_connect = on_connect
    client.on_message = on_message
    client.will_set(
        f"{state.prefix}/availability",
        json.dumps(state.build_availability("offline")),
        qos=1,
        retain=True,
    )
    client.connect(broker_host, broker_port, keepalive=15)
    client.loop_start()

    def handle_sig(*_):
        nonlocal stop
        stop = True

    signal.signal(signal.SIGINT, handle_sig)
    signal.signal(signal.SIGTERM, handle_sig)

    last_publish = 0.0
    last_warn = 0.0
    try:
        while not stop:
            if serial_holder["port"] is None:
                try:
                    serial_holder["port"] = WinSerialPort(port, baud=baud)
                    print(f"[BRIDGE] Connected {port} -> mqtt://{broker_host}:{broker_port} ({state.prefix})", flush=True)
                    publish("availability", state.build_availability("online"), qos=1, retain=True)
                    if latest_config_cmd[0] is not None:
                        time.sleep(0.15)
                        serial_holder["port"].write(latest_config_cmd[0])
                        print("[BRIDGE] Synced saved config to Pico", flush=True)
                    publish("telemetry", state.build_telemetry())
                    last_publish = time.monotonic()
                except OSError as exc:
                    now = time.monotonic()
                    if now - last_warn >= 5.0:
                        print(f"[BRIDGE] Waiting for {port} ({exc}). Close Serial Monitor if open.", flush=True)
                        last_warn = now
                    time.sleep(1.5)
                    continue

            try:
                line = serial_holder["port"].readline()
            except OSError as exc:
                print(f"[BRIDGE] Lost {port}: {exc}", flush=True)
                serial_holder["port"].close()
                serial_holder["port"] = None
                publish("availability", state.build_availability("offline"), qos=1, retain=True)
                continue

            if line is not None:
                print(f"[PICO] {line}", flush=True)
                if line:
                    publish("console", {"line": line[:4096]})
                if state.parse_line(line):
                    publish("telemetry", state.build_telemetry())
                    last_publish = time.monotonic()
            elif time.monotonic() - last_publish >= 1.0:
                publish("telemetry", state.build_telemetry())
                last_publish = time.monotonic()
    finally:
        if serial_holder["port"] is not None:
            serial_holder["port"].close()
        publish("availability", state.build_availability("offline"), qos=1, retain=True)
        client.loop_stop()
        client.disconnect()


def run_logs_only(port: str, baud: int, device_id: str, broker_host: str, broker_port: int) -> None:
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"console-{device_id}")
    client.connect_async(broker_host, broker_port, keepalive=15)
    client.loop_start()
    try:
        while True:
            try:
                serial = WinSerialPort(port, baud=baud)
                print(f"[CONSOLE] Connected {port}", flush=True)
                try:
                    while True:
                        line = serial.readline()
                        if line:
                            print(f"[PICO] {line}", flush=True)
                            client.publish(f"{topic_prefix(device_id)}/console", json.dumps({"line": line[:4096]}))
                finally:
                    serial.close()
            except OSError as exc:
                print(f"[CONSOLE] Waiting for {port}: {exc}", flush=True)
                time.sleep(2)
    except KeyboardInterrupt:
        pass
    finally:
        client.loop_stop()
        client.disconnect()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM6")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--device", default="pico-01")
    parser.add_argument("--broker", default="127.0.0.1")
    parser.add_argument("--port-mqtt", type=int, default=1883)
    parser.add_argument("--h0", type=float, default=82.0)
    parser.add_argument("--logs-only", action="store_true", help="Forward USB logs without controlling the Pico")
    args = parser.parse_args()
    if args.logs_only:
        run_logs_only(args.port, args.baud, args.device, args.broker, args.port_mqtt)
    else:
        run_bridge(args.port, args.baud, args.device, args.broker, args.port_mqtt, args.h0)
