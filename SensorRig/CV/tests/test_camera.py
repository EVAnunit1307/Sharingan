import sys
from pathlib import Path
import threading
import time
import unittest
import base64

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from camera_dashboard import CameraPipeline, camera_position, create_app
from detector.person_detector import PersonDetector


def observation(score=.65, x=10):
    return {"box": [x, 20, x + 50, 160], "score": score}


class InferenceFrameTests(unittest.TestCase):
    def test_exact_frame_endpoint_cannot_mix_metadata_or_serve_stale_image(self):
        pipeline=CameraPipeline(type('UnusedDetector',(),{'cfg':{'model':'fixture'}})())
        pipeline.result=dict(generation=3,frame_id=42,captured_at=time.monotonic(),frame_width=640,
            frame_height=480,people=[dict(id=7,observed=True,box=[100,10,200,470])],inference_jpeg=b'exact-image')
        client=create_app(pipeline).test_client()
        frame=client.get('/pose/frame')
        self.assertEqual(frame.status_code,200)
        self.assertEqual(frame.json['frame_id'],42)
        self.assertEqual(frame.json['generation'],3)
        self.assertEqual(base64.b64decode(frame.json['jpeg_base64']),b'exact-image')
        pipeline.result['captured_at']-=1
        self.assertEqual(client.get('/pose/frame').status_code,503)


class TrackingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.det = PersonDetector(threads=1)

    def setUp(self):
        self.det.reset()
        self.det.k = 2

    def test_weak_candidate_needs_two_distinct_observations(self):
        self.assertEqual(self.det.update([observation()], 1), [])
        people = self.det.update([observation()], 1.1)
        self.assertEqual(len(people), 1)
        self.assertTrue(people[0]['observed'])

    def test_high_confidence_is_immediate(self):
        self.assertEqual(len(self.det.update([observation(.9)], 1)), 1)

    def test_k_one_confirms_first_observation(self):
        self.det.k = 1
        self.assertEqual(len(self.det.update([observation()], 1)), 1)

    def test_low_confidence_can_sustain_but_not_create(self):
        self.assertEqual(self.det.update([observation(.45)], 1), [])
        person = self.det.update([observation(.9)], 1.1)[0]
        sustained = self.det.update([observation(.45)], 1.2)[0]
        self.assertEqual(person['id'], sustained['id'])
        self.assertTrue(sustained['observed'])

    def test_coasting_is_explicit_and_expires_in_seconds(self):
        self.det.update([observation(.9)], 1)
        coast = self.det.update([], 1.1)
        self.assertFalse(coast[0]['observed'])
        self.assertEqual(self.det.update([], 1.36), [])

    def test_duplicate_frame_cannot_confirm(self):
        self.det.update([observation()], 1)
        with self.assertRaises(ValueError):
            self.det.update([observation()], 1)
        self.assertFalse(self.det.tracks[0].confirmed)

    def test_two_people_do_not_share_observations(self):
        original = self.det.update([observation(.9, 10), observation(.9, 100)], 1)
        moved = self.det.update([observation(.8, 96), observation(.8, 14)], 1.1)
        self.assertEqual(len(moved), 2)
        self.assertEqual(original[0]['id'], moved[0]['id'])
        self.assertEqual(moved[0]['box'][0], 14)

    def test_nan_and_invalid_boxes_are_rejected(self):
        self.assertEqual(self.det.update([observation(float('nan')),
                          {'score': .9, 'box': [20, 10, 10, 30]}], 1), [])


class FakeDetector:
    cfg = {'model': 'test'}
    last_raw = []

    def __init__(self):
        self.calls = 0
        self.called = threading.Event()

    def reset(self):
        pass

    def detect(self, frame, timestamp=None):
        self.calls += 1
        self.called.set()
        return []


class PipelineTests(unittest.TestCase):
    def test_capture_does_not_hide_an_inference_error(self):
        pipeline = CameraPipeline(FakeDetector())
        pipeline.error = 'Inference failed'
        pipeline.publish_frame(np.zeros((480, 640, 3), dtype=np.uint8), time.monotonic())
        self.assertEqual(pipeline.snapshot()['error'], 'Inference failed')

    def test_latest_slot_is_consumed_once_and_skips_backlog(self):
        detector = FakeDetector()
        pipeline = CameraPipeline(detector, detect_fps=30)
        now = time.monotonic()
        frame = np.zeros((480, 640, 3), dtype=np.uint8)
        for i in range(8):
            pipeline.publish_frame(frame, now + i * .001)
        worker = threading.Thread(target=pipeline.detect_loop)
        worker.start()
        try:
            self.assertTrue(detector.called.wait(1))
            time.sleep(.15)
            self.assertEqual(detector.calls, 1)
            self.assertEqual(pipeline.snapshot()['frame_id'], 8)
        finally:
            pipeline.stop_event.set()
            with pipeline.condition:
                pipeline.condition.notify_all()
            worker.join(1)

    def test_stale_camera_clears_people_and_image(self):
        pipeline = CameraPipeline(FakeDetector())
        pipeline.result = dict(captured_at=time.monotonic()-2, people=[{'observed': True}],
                               raw=[observation()], frame_id=9, jpeg=b'old image')
        state = pipeline.snapshot()
        self.assertFalse(state['camera_connected'])
        self.assertEqual(state['people'], [])
        self.assertEqual(state['raw'], [])
        self.assertEqual(pipeline.jpeg()[0], 0)
        app = create_app(pipeline).test_client()
        self.assertEqual(app.get('/healthz').status_code, 503)
        self.assertEqual(app.get('/detections').json['person_count'], 0)

    def test_clipped_body_does_not_get_invented_range(self):
        position = camera_position([100, 20, 300, 480], 640, 480)
        self.assertIsNone(position['x_m'])
        self.assertIsNone(position['y_m'])
        self.assertLess(position['bearing_deg'], 0)

    def test_camera_depth_is_forward_not_radial(self):
        position = camera_position([400, 50, 500, 400], 640, 480)
        self.assertGreater(position['x_m'], 0)
        self.assertEqual(position['range_source'], 'monocular_estimate')
        self.assertAlmostEqual(position['x_m']/position['y_m'], 130/(640/(2*np.tan(np.deg2rad(31)))), places=3)


if __name__ == '__main__':
    unittest.main()
