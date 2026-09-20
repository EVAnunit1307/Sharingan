import io
import json
import unittest

from GroundStation.fusion import FusionConfig
from GroundStation.replay import replay
from GroundStation.tests.test_fusion import packet


def recording():
    return io.StringIO("\n".join(json.dumps(dict(received_at=t, packet=p))
                                  for t, p in ((0, packet()), (.1, packet(2)))))


class ReplayTests(unittest.TestCase):
    def test_recording_exercises_matching_and_preserves_original_payload(self):
        result = list(replay(recording(), FusionConfig(alignment_confirmed=True)))
        self.assertEqual(len(result), 2)
        self.assertTrue(all(p["is_replay"] for p in result))
        self.assertEqual(result[1]["camera_people"], packet(2)["camera_people"])
        self.assertEqual(result[0]["spatial_people"]["people"][0]["position_source"], "camera_estimate")
        self.assertEqual(result[1]["spatial_people"]["people"][0]["position_source"], "radar_matched")

    def test_alignment_is_not_assumed_when_replaying(self):
        result = list(replay(recording()))
        self.assertTrue(all(p["spatial_people"]["people"][0]["position_source"] == "camera_estimate" for p in result))

    def test_invalid_json_reports_line_number(self):
        with self.assertRaisesRegex(ValueError, "Record 2"):
            list(replay([json.dumps(dict(received_at=0, packet=packet())), "not json"]))

    def test_nonfinite_or_backwards_receipt_times_are_rejected(self):
        for value in (float("nan"), float("inf"), -1, True):
            records = [json.dumps(dict(received_at=0, packet=packet())),
                       json.dumps(dict(received_at=value, packet=packet(2)))]
            with self.assertRaisesRegex(ValueError, "Record 2"):
                list(replay(records))


if __name__ == "__main__":
    unittest.main()
