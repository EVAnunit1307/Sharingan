from dataclasses import asdict
import io
import json
from pathlib import Path
import tempfile
import threading
import unittest
from unittest.mock import patch

from GroundStation.fusion import FusionConfig
from GroundStation.server import RelayState, create_app, run_recording, run_upstream, start_websocket
from GroundStation.tests.test_fusion import packet


class RelayTests(unittest.TestCase):
    def setUp(self):
        self.now = 0
        self.state = RelayState(FusionConfig(alignment_confirmed=True), clock=lambda: self.now)

    def test_live_snapshot_expires_and_preserves_legacy_fields(self):
        first = packet()
        first.update(rig=dict(x=5, y=6, heading_deg=7, tracking_ok=True), detections=[dict(id=1)])
        self.state.ingest(first)
        out = self.state.snapshot()
        self.assertEqual(out["rig"], first["rig"])
        self.assertEqual(out["detections"], first["detections"])
        self.now = 2
        expired = self.state.snapshot()
        self.assertEqual(expired["spatial_people"]["people"], [])
        self.assertEqual(expired["detections"], [])
        self.assertFalse(expired["rig"]["tracking_ok"])
        self.assertGreater(expired["relay_sequence"], out["relay_sequence"])

    def test_sensor_disconnect_clears_output_without_closing_quest_connection(self):
        self.state.ingest(packet())
        self.state.disconnect("Pi unplugged")
        output = self.state.snapshot()
        self.assertFalse(output["camera_connected"])
        self.assertEqual(output["spatial_people"]["people"], [])

    def test_recording_preserves_monotonic_receipt_time(self):
        recorded = io.StringIO()
        self.state.recording = recorded
        self.now = 123.5
        self.state.ingest(packet())
        row = json.loads(recorded.getvalue())
        self.assertEqual(row["received_at"], 123.5)
        self.assertEqual(row["packet"], packet())

    def test_recorded_input_runs_through_relay_and_expires_after_playback(self):
        with tempfile.TemporaryDirectory() as directory:
            recording = Path(directory) / "input.jsonl"
            recording.write_text("\n".join(json.dumps(dict(received_at=0, packet=packet(i))) for i in (1, 2)))
            self.state.is_replay = True
            run_recording(self.state, recording, threading.Event())
            result = self.state.snapshot()
            self.assertTrue(result["is_replay"])
            self.assertEqual(result["spatial_people"]["people"][0]["position_source"], "radar_matched")
            self.assertEqual(self.state.error, "Replay finished")
            self.now = .8
            self.assertEqual(self.state.snapshot()["spatial_people"]["people"], [])
            recording.write_text("[]\n")
            run_recording(self.state, recording, threading.Event())
            self.assertIn("Replay failed", self.state.error)
            self.assertEqual(self.state.snapshot()["spatial_people"]["people"], [])

    def test_dashboard_config_and_read_only_proxy(self):
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "settings.json"
            client = create_app(self.state, "http://sensor:8766", config_path=destination).test_client()
            page = client.get("/")
            self.assertEqual(page.status_code, 200)
            self.assertIn(b"Native Quest people", page.data)
            with client.get("/assets/ground.js") as asset:
                self.assertEqual(asset.status_code, 200)
            self.assertEqual(client.post("/radar/config", json={}).status_code, 404)
            self.assertEqual(client.post("/radar/trials", json={}).status_code, 405)
            settings = asdict(FusionConfig(camera_offset_right_m=.25, alignment_confirmed=True))
            self.assertEqual(client.post("/fusion/config", json=settings).status_code, 200)
            self.assertEqual(json.loads(destination.read_text()), settings)
            wrong = dict(settings, camera_offset_right_m=999)
            self.assertEqual(client.post("/fusion/config", json=wrong).status_code, 400)
            self.assertEqual(json.loads(destination.read_text()), settings)
            self.assertEqual(client.post("/fusion/config", json=settings, headers={"Origin":"http://elsewhere"}).status_code, 403)
            manifest = client.get("/handoff.json").json
            self.assertEqual(manifest["websocket_url"], "ws://localhost:8765/")
            self.assertEqual(client.get("/fusion.json").json["config"], settings)

    def test_proxy_failure_does_not_serve_stale_sensor_positions(self):
        client = create_app(self.state, "http://sensor:8766").test_client()
        with patch("GroundStation.server.urlopen", side_effect=OSError("offline")):
            self.assertEqual(client.get("/detections").status_code, 502)

    def test_real_websocket_reports_matches_and_clears_disconnect(self):
        from websockets.sync.client import connect
        stop = threading.Event()
        server, thread = start_websocket(self.state, "127.0.0.1", 0, stop)
        port = server.socket.getsockname()[1]
        try:
            self.state.ingest(packet())
            self.now = .1
            self.state.ingest(packet(2))
            with connect(f"ws://127.0.0.1:{port}") as client:
                data = json.loads(client.recv(timeout=2))
                self.assertEqual(data["spatial_people"]["people"][0]["position_source"], "radar_matched")
                self.state.disconnect("test outage")
                for _ in range(10):
                    data = json.loads(client.recv(timeout=2))
                    if not data["spatial_people"]["people"]:
                        break
                self.assertEqual(data["spatial_people"]["people"], [])
        finally:
            stop.set(); server.shutdown(); thread.join(timeout=2)

    def test_real_upstream_to_relay_flow(self):
        from websockets.sync.server import serve
        from websockets.sync.client import connect
        from websockets.exceptions import ConnectionClosed
        stop = threading.Event()
        def upstream(socket):
            try:
                for frame in range(1, 25):
                    socket.send(json.dumps(packet(frame)))
                    if stop.wait(.02): return
            except ConnectionClosed:
                pass
        pi = serve(upstream, "127.0.0.1", 0)
        pi_thread = threading.Thread(target=pi.serve_forever, daemon=True)
        pi_thread.start()
        state = RelayState(FusionConfig(alignment_confirmed=True))
        receiver = threading.Thread(target=run_upstream,
            args=(state, f"ws://127.0.0.1:{pi.socket.getsockname()[1]}", stop), daemon=True)
        downstream, downstream_thread = start_websocket(state, "127.0.0.1", 0, stop)
        receiver.start()
        try:
            with connect(f"ws://127.0.0.1:{downstream.socket.getsockname()[1]}") as quest:
                matched = False
                for _ in range(20):
                    output = json.loads(quest.recv(timeout=2))
                    people = output["spatial_people"]["people"]
                    if people and people[0]["position_source"] == "radar_matched":
                        matched = True; break
                self.assertTrue(matched)
        finally:
            stop.set(); pi.shutdown(); downstream.shutdown()
            receiver.join(timeout=3); pi_thread.join(timeout=2); downstream_thread.join(timeout=2)


if __name__ == "__main__":
    unittest.main()
