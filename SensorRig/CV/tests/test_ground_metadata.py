from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from camera_dashboard import CameraPipeline, camera_position
from quest_bridge import make_packet
from radar_service import DEFAULT_CONFIG, RadarService, RadarTracker
from test_handoff import frame
from GroundStation.fusion import FusionConfig, FusionEngine


class FakeDetector:
    cfg = {"model": "test"}


class GroundMetadataTests(unittest.TestCase):
    def test_bridge_preserves_camera_capture_clock_and_reconnect_generation(self):
        p = CameraPipeline(FakeDetector(), hfov=60)
        captured = time.monotonic()
        p.result = dict(frame_id=12, generation=4, captured_at=captured,
                        timestamp=time.time(), frame_width=800, frame_height=600, people=[])
        output = make_packet(p.snapshot(), None, "awaiting rig pose")
        self.assertEqual(output["source_session_id"], p.source_session_id)
        self.assertEqual(output["camera_generation"], 4)
        self.assertEqual(output["camera_capture_mono_ms"], captured * 1000)
        self.assertEqual(output["camera_frame_width"], 800)
        self.assertEqual(output["camera_frame_height"], 600)
        self.assertEqual(output["camera_hfov_deg"], 60)
        self.assertFalse(output["rig"]["tracking_ok"])
        self.assertEqual(output["detections"], [])

    def test_new_sensor_process_identity_is_distinct(self):
        self.assertNotEqual(CameraPipeline(FakeDetector()).source_session_id,
                            CameraPipeline(FakeDetector()).source_session_id)

    def test_radar_timestamp_uses_same_monotonic_clock_domain(self):
        radar = RadarService()
        observed = time.monotonic()
        radar.last_frame = observed
        radar.frame_id = 9
        radar.generation = 3
        result = radar.snapshot()
        self.assertEqual(result["capture_mono_ms"], observed * 1000)
        self.assertEqual(result["generation"], 3)
        self.assertEqual(result["frame_id"], 9)
        self.assertGreaterEqual(result["age_ms"], 0)

    def test_real_pi_packet_matches_without_a_world_pose_and_falls_back_on_radar_loss(self):
        with tempfile.TemporaryDirectory() as directory:
            radar = RadarService(config_path=Path(directory) / "mount.json")
            radar.set_config(dict(DEFAULT_CONFIG, mount_level=True, mounting_confirmed=True))
            tracker = RadarTracker()
            camera = CameraPipeline(FakeDetector())
            camera.radar = radar
            engine = FusionEngine(FusionConfig(alignment_confirmed=True))
            at = time.monotonic()
            for offset in (-.2, -.1):
                radar.ingest(frame(x=0, y=3000), tracker, at + offset)
            box = [280, 40, 360, 440]
            observation = dict(id=1, observed=True, score=.9, box=box,
                               **camera_position(box, 640, 480))
            for i in range(2):
                captured = at + i * .1
                radar.ingest(frame(x=0, y=3000), tracker, captured)
                camera.result = dict(frame_id=i + 1, generation=0, captured_at=captured,
                    timestamp=time.time(), frame_width=640, frame_height=480, people=[observation])
                with patch("time.monotonic", return_value=captured):
                    packet = make_packet(camera.snapshot(), None, "no external pose")
                self.assertEqual(packet["detections"], [])
                self.assertFalse(packet["rig"]["tracking_ok"])
                result = engine.ingest(packet, now=10 + i * .1)
            self.assertEqual(result["people"][0]["position_source"], "radar_matched")
            self.assertEqual(result["people"][0]["forward_m"], 3)
            self.assertEqual(result["radar_targets"], [])
            radar.error = "serial disconnected"
            with patch("time.monotonic", return_value=at + .2):
                failed = make_packet(camera.snapshot(), None, "no external pose")
            fallback = engine.ingest(failed, now=10.2)["people"][0]
            self.assertEqual(fallback["camera_id"], 1)
            self.assertEqual(fallback["position_source"], "camera_estimate")
            self.assertAlmostEqual(fallback["forward_m"], observation["y_m"])


if __name__ == "__main__":
    unittest.main()
