import asyncio
import json
import socket
import tempfile
import time
import unittest
from pathlib import Path
from queue import SimpleQueue
from threading import Event, Thread
from unittest.mock import Mock, patch
from types import SimpleNamespace

import paho.mqtt.client as mqtt
from fastapi.testclient import TestClient

from app import Dashboard, app, wall_time
from broker.dev_broker import make_broker
from protocol import Ack, Command, CommandInput, WifiConnectInput
from simulator import SimulatedSorter, calibration, simulated_wifi


def wait_for(condition, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = condition()
        if value:
            return value
        time.sleep(0.025)
    raise AssertionError("Timed out waiting for condition")


class DashboardTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory(ignore_cleanup_errors=True)
        self.addCleanup(self.dir.cleanup)
        self.dashboard = Dashboard("sorter-sim", Path(self.dir.name) / "test.sqlite3")
        self.addCleanup(self.dashboard.store.db.close)
        self.dashboard.broker_status(True)
        self.sim = SimulatedSorter(now=10)
        self.sim.start_link("link1")
        self.availability("online")

    def availability(self, status, link_id="link1"):
        self.dashboard.handle_message("availability", json.dumps(
            {"schema_version": 1, "link_id": link_id, "status": status}), retained=True)

    def test_console_log_is_persisted_and_invalid_messages_are_ignored(self):
        self.dashboard.handle_message("console", json.dumps({"line": "[CONTROL] START"}))
        self.dashboard.handle_message("console", b"not json")
        self.dashboard.handle_message("console", json.dumps({"line": "retained"}), retained=True)
        rows = self.dashboard.store.recent_logs(10)
        self.assertEqual([row["message"] for row in rows], ["[CONTROL] START"])
        with patch.object(app.state, "dashboard", self.dashboard, create=True):
            self.assertEqual(TestClient(app, base_url="http://localhost").get("/api/logs?limit=1").json()[0]["message"], "[CONTROL] START")
        from storage import Store
        reopened = Store(self.dashboard.store.path)
        try:
            self.assertEqual(reopened.recent_logs(1)[0]["message"], "[CONTROL] START")
        finally:
            reopened.db.close()

    def sample(self, now=10):
        self.dashboard.handle_message("telemetry", json.dumps(self.sim.telemetry(now)))

    def test_link_and_stale(self):
        self.assertFalse(self.dashboard.status()["online"])
        self.sample()
        self.assertTrue(self.dashboard.status()["online"])
        self.availability("offline", "older-link")
        self.assertTrue(self.dashboard.status()["online"])
        self.dashboard.fresh_at -= 3.1
        self.assertEqual(self.dashboard.status()["controls"]["ENABLE_SORTING"], "OFFLINE")
        self.sample(14)
        self.availability("offline")
        self.assertFalse(self.dashboard.status()["online"])
        self.assertEqual(self.dashboard.status()["snapshot"]["seq"], 2)

    def test_restart_gap_and_unverified_tray(self):
        self.sim.counts = {"S": 12, "M": 8, "L": None}
        self.sample()
        run = self.dashboard.store.start_run("Test", wall_time())
        self.sim.counts["S"] += 3
        self.sample(11)
        self.assertEqual(self.dashboard.status()["run_counts"], {"S": 3, "M": 0, "L": None})

        self.availability("offline")
        self.sim.start_link("link2")
        self.availability("online", "link2")
        self.sim.counts["S"] += 7
        self.sample(15)
        self.assertEqual(self.dashboard.status()["run_counts"]["S"], 10)
        history = self.dashboard.store.history(run["started_at"], wall_time(), 20)
        self.assertEqual([p["gap"] for p in history["points"]], [False, True])

        self.sim = SimulatedSorter(now=16)
        self.sim.start_link("link3")
        self.availability("online", "link3")
        self.sim.counts["S"] = 2
        self.sample(17)
        self.assertEqual(self.dashboard.status()["run_counts"]["S"], 12)
        self.assertIsNone(self.dashboard.status()["confirmed_per_second"])

    def test_duplicate_and_retained_telemetry_do_not_refresh(self):
        sample = json.dumps(self.sim.telemetry(10))
        self.dashboard.handle_message("telemetry", sample, retained=True)
        self.assertFalse(self.dashboard.fresh())
        self.dashboard.handle_message("telemetry", sample)
        self.dashboard.fresh_at -= 3.1
        self.dashboard.handle_message("telemetry", sample)
        self.assertFalse(self.dashboard.fresh())

    def test_command_logged_before_publish_and_ack_scoped(self):
        self.sample()

        def publish(data):
            cmd = Command.model_validate_json(data)
            self.assertEqual(self.dashboard.store.command(cmd.request_id)["state"], "pending")
            return True

        command = self.dashboard.send(CommandInput(action="ENABLE_SORTING"), publish)
        req = command["request_id"]
        ack = Ack(schema_version=1, request_id=req, link_id="wrong", result="executed", reason="OK", mode="sorting")
        self.dashboard.handle_message("ack", ack.model_dump_json())
        self.assertEqual(self.dashboard.store.command(req)["state"], "pending")
        self.dashboard.store.expire_commands(wall_time() + 6)
        self.assertEqual(self.dashboard.store.command(req)["state"], "unknown")
        self.dashboard.handle_message("ack", ack.model_copy(update={"link_id": "link1"}).model_dump_json())
        self.assertEqual(self.dashboard.store.command(req)["state"], "executed")
        self.assertEqual(self.dashboard.status()["snapshot"]["mode"], "idle")

    def test_wifi_scan_connect_and_validation(self):
        self.sample()
        sent = []
        publish = lambda data, leaf: sent.append((leaf, json.loads(data))) or True
        scan = self.dashboard.wifi_send({}, "scan", publish)
        self.assertEqual(sent[-1][0], "wifi/request")
        with self.assertRaises(Exception):  # A second request while one is pending.
            self.dashboard.wifi_send({}, "scan", publish)
        self.dashboard.wifi_result(b'{"schema_version":1,"request_id":"other","kind":"scan","ok":true,"reason":"OK","networks":[]}')
        self.assertIsNotNone(self.dashboard.wifi_state()["pending"])  # Unknown id ignored.
        self.dashboard.wifi_result(simulated_wifi(json.dumps(sent[-1][1])))
        state = self.dashboard.wifi_state()
        self.assertEqual((state["source"], state["device_id"]), ("windows", "sorter-sim"))
        self.assertIsNone(state["pending"])
        self.assertEqual(state["scan"]["networks"][0]["ssid"], "Pegatron-Lab")
        self.assertEqual(state["scan"]["request_id"], scan["request_id"])

        body = WifiConnectInput(ssid="Pegatron-Lab", password=r'p"a\ss1234').model_dump()
        self.dashboard.wifi_send(body, "connect", publish)
        self.assertEqual(sent[-1][1]["password"], r'p"a\ss1234')  # Sent, but never stored.
        self.dashboard.wifi_result(simulated_wifi(json.dumps({**sent[-1][1], "password": "wrong-pass"})))
        state = self.dashboard.wifi_state()
        self.assertEqual((state["connect"]["ok"], state["connect"]["reason"]), (False, "WIFI_FAILED"))
        self.assertNotIn("password", json.dumps(state))
        for bad in ({"ssid": "x", "password": "short"}, {"ssid": "", "password": ""}, {"ssid": "é" * 17}):
            with self.assertRaises(Exception):
                WifiConnectInput(**bad)
        WifiConnectInput(ssid="Khach", password="")  # Open network is allowed.

    def test_physical_defaults_to_monitor_only(self):
        physical = Dashboard("pico-01", Path(self.dir.name) / "physical.sqlite3")
        self.addCleanup(physical.store.db.close)
        physical.broker_status(True)
        physical.handle_message("availability", '{"schema_version":1,"link_id":"link1","status":"online"}')
        physical.handle_message("telemetry", json.dumps(self.sim.telemetry(10)))
        self.assertEqual(physical.status()["controls"]["ENABLE_SORTING"], "MONITOR_ONLY")
        self.assertEqual((physical.wifi_state()["source"], physical.wifi_state()["device_id"]),
                         ("device", "pico-01"))

    def test_wifi_http_uses_windows_without_simulator_or_fake_connect(self):
        publish = Mock(return_value=True)
        self.dashboard.broker_status(False)
        with patch.object(app.state, "dashboard", self.dashboard, create=True), \
                patch.object(app.state, "bridge", SimpleNamespace(publish=publish), create=True), \
                patch("app.scan_windows_networks", return_value=[
                    {"ssid": "BM KTMT", "rssi": -51, "secure": True}]) as scan:
            http = TestClient(app, base_url="http://127.0.0.1")
            sent = http.post("/api/wifi/scan")
            self.assertEqual(sent.status_code, 202)
            wait_for(lambda: self.dashboard.wifi_state()["scan"])
            state = http.get("/api/wifi").json()
            self.assertEqual(state["scan"]["request_id"], sent.json()["request_id"])
            self.assertEqual(state["scan"]["networks"][0]["ssid"], "BM KTMT")
            self.assertIsNone(state["pending"])
            scan.assert_called_once()
            publish.assert_not_called()
            self.assertEqual(http.post("/api/wifi/connect", json={"ssid": "BM KTMT", "password": "12345678"})
                             .json()["detail"], "SIMULATED_DEVICE")
            denied = OSError("denied")
            denied.winerror = 5
            scan.side_effect = denied
            failed = http.post("/api/wifi/scan").json()["request_id"]
            wait_for(lambda: self.dashboard.wifi_state()["scan"]["request_id"] == failed)
            state = http.get("/api/wifi").json()
            self.assertFalse(state["scan"]["ok"])
            self.assertEqual(state["scan"]["reason"], "WIFI_PERMISSION_DENIED")
            self.assertEqual(state["scan"]["networks"], [])
            self.assertIsNone(state["pending"])
            physical = Dashboard("pico-01", Path(self.dir.name) / "wifi-physical.sqlite3")
            self.addCleanup(physical.store.db.close)
            physical.broker_status(True)
            physical.handle_message("availability", '{"schema_version":1,"link_id":"link1","status":"online"}')
            physical.handle_message("telemetry", json.dumps(self.sim.telemetry(10)))
            app.state.dashboard = physical
            self.assertEqual(http.post("/api/wifi/scan").status_code, 202)
            publish.assert_called_once()
            self.assertEqual(publish.call_args.args[1], "wifi/request")
            self.assertEqual(scan.call_count, 2)

    def test_host_wifi_scan_returns_before_windows_finishes(self):
        started, release = Event(), Event()

        def slow_scan():
            started.set()
            if not release.wait(3):
                raise OSError("HOST_SCAN_FAILED")
            return []

        with patch.object(app.state, "dashboard", self.dashboard, create=True), \
                patch("app.scan_windows_networks", side_effect=slow_scan):
            try:
                http = TestClient(app, base_url="http://127.0.0.1")
                sent = http.post("/api/wifi/scan")
                self.assertEqual(sent.status_code, 202)
                self.assertTrue(started.wait(1))
                self.assertIsNotNone(self.dashboard.wifi_state()["pending"])
                self.assertIsNone(self.dashboard.wifi_state()["scan"])
                self.assertEqual(http.post("/api/wifi/scan").status_code, 409)
            finally:
                release.set()
                wait_for(lambda: self.dashboard.wifi_state()["scan"])


class SimulatorTests(unittest.TestCase):
    def command(self, sim, action, station=None, **overrides):
        return Command.model_validate(dict(schema_version=1, request_id=action.lower(), link_id=sim.link_id,
                                           observed_uptime_ms=0, control_epoch=sim.epoch,
                                           action=action, station=station) | overrides)

    def test_duplicate_test_pulse_and_home_fault(self):
        sim = SimulatedSorter("home-fault", now=0)
        sim.start_link("link1")
        cmd = self.command(sim, "TEST_PUSHER", 1)
        sim.command(cmd, 0.1)
        sim.command(cmd, 0.12)
        sim.tick(0.31)
        sim.command(cmd, 0.4)
        self.assertEqual(sim.pushes, [1, 0])
        self.assertEqual(sim.mode, "fault")
        self.assertIn("HOME_TIMEOUT_1", sim.faults)

    def test_expired_and_locally_revoked(self):
        sim = SimulatedSorter("maintenance", now=0)
        sim.start_link("link1")
        cmd = self.command(sim, "TEST_PUSHER", 2)
        sim.command(cmd, 5)
        self.assertEqual(sim.pushes, [0, 0])
        sim.local("p")
        sim.command(cmd, 0.1)
        self.assertEqual(sim.pushes, [0, 0])

    def test_pause_preempts_calibration(self):
        sim = SimulatedSorter("maintenance", now=0)
        sim.start_link("link1")
        sim.command(self.command(sim, "CALIBRATE_TOF", 1), 0.1)
        sim.command(self.command(sim, "PAUSE_SORTING"), 0.2)
        sim.tick(0.7)
        self.assertEqual(sim.references, [80, 80])
        self.assertEqual(sim.mode, "idle")

    def test_calibration_limits(self):
        self.assertEqual(calibration([82.0] * 50, 80.0), (82.0, "OK"))
        self.assertEqual(calibration([80.0] * 49, 80.0)[1], "TOO_FEW_SAMPLES")
        self.assertEqual(calibration([80.0] * 49 + [84.0], 80.0)[1], "UNSTABLE_SURFACE")
        self.assertEqual(calibration([86.0] * 50, 80.0)[1], "REFERENCE_OUT_OF_RANGE")


class BrokerIntegrationTest(unittest.TestCase):
    def test_http_mqtt_simulator_command_history_csv_and_loss(self):
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        loop = asyncio.new_event_loop()
        ready = Event()

        async def broker_start():
            broker = make_broker(port)
            await broker.start()
            ready.set()
            return broker

        def broker_thread():
            asyncio.set_event_loop(loop)
            broker = loop.run_until_complete(broker_start())
            loop.run_forever()
            loop.run_until_complete(broker.shutdown())
            loop.close()

        thread = Thread(target=broker_thread, daemon=True)
        thread.start()
        self.assertTrue(ready.wait(5))
        sim = SimulatedSorter()
        sim.start_link("integration-link")
        prefix = "pegatron/v1/sorter-sim/"
        received = SimpleQueue()
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="integration-simulator")

        def publish_sample():
            client.publish(prefix + "telemetry", json.dumps(sim.telemetry(time.monotonic())), qos=0)

        def announce(client, userdata, mid, reasons, properties):
            client.publish(prefix + "availability", '{"schema_version":1,"link_id":"integration-link","status":"online"}',
                           retain=True, qos=1)
            ready.set()

        client.on_connect = lambda *args: client.subscribe(prefix + "command", qos=1)
        client.on_subscribe = announce
        client.on_message = lambda client, userdata, message: received.put(Command.model_validate_json(message.payload))
        ready.clear()
        try:
            client.connect("127.0.0.1", port)
            client.loop_start()
            self.assertTrue(ready.wait(5))
            with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as temp, patch.dict("os.environ", {
                "PEGATRON_MQTT_PORT": str(port), "PEGATRON_MQTT_HOST": "127.0.0.1",
                "PEGATRON_MQTT_USER": "", "PEGATRON_DB": str(Path(temp) / "test.sqlite3"),
                "PEGATRON_DEVICE_ID": "sorter-sim",
            }), TestClient(app, base_url="http://127.0.0.1") as http:
                def live():
                    publish_sample()
                    return http.get("/api/status").json()["online"]

                wait_for(live)
                run = http.post("/api/runs", json={"name": "Bài thử MQTT"}).json()
                sent = http.post("/api/commands", json={"action": "ENABLE_SORTING"})
                self.assertEqual(sent.status_code, 202)
                request_id = sent.json()["request_id"]
                sim.command(received.get(timeout=4), time.monotonic())
                for leaf, payload, _ in sim.drain(time.monotonic()):
                    client.publish(prefix + leaf, payload, qos=1)
                sim.counts.update(S=4, M=3, L=2)
                publish_sample()

                def acked():
                    return http.get(f"/api/commands/{request_id}").json()["state"] == "executed"

                wait_for(acked)
                wait_for(lambda: http.get("/api/status").json()["run_counts"]["S"] == 4)
                self.assertEqual(http.post(f'/api/runs/{run["id"]}/end', json={}).status_code, 200)
                history = http.get("/api/history", params={"run_id": run["id"]}).json()
                self.assertEqual(history["commands"][0]["state"], "executed")
                self.assertEqual(history["events"][0]["kind"], "mode_changed")
                csv = http.get("/api/history", params={"run_id": run["id"], "format": "csv"})
                self.assertIn("delta_S", csv.text)
                self.assertIn(request_id, csv.text)
                self.assertEqual(http.post("/api/commands", json={"action": "PAUSE_SORTING"},
                                           headers={"Origin": "https://example.org"}).status_code, 403)
                wait_for(lambda: not http.get("/api/status").json()["online"])
                self.assertEqual(http.post("/api/commands", json={"action": "PAUSE_SORTING"}).status_code, 409)
        finally:
            client.disconnect()
            client.loop_stop()
            loop.call_soon_threadsafe(loop.stop)
            thread.join(5)


if __name__ == "__main__":
    unittest.main()

