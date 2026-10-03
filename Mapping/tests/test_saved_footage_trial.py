import unittest

import numpy as np

from Mapping.saved_footage_trial import choose_frames, image_quality


class SavedFootageTests(unittest.TestCase):
    def test_control_only_replaces_known_bad_frame(self):
        metrics = {i: dict(decoded=True, sharpness=1, corner_coverage=.5) for i in range(50)}
        selected, decisions = choose_frames([10, 25, 40], metrics, {24, 25, 26}, 'streak_only')
        self.assertEqual(selected, [10, 23, 40])
        self.assertTrue(decisions[1]['anchor_excluded'])

    def test_quality_keeps_time_bins_and_shared_windows(self):
        metrics = {i: dict(decoded=True, sharpness=float(i), corner_coverage=.5) for i in range(70)}
        selected, _ = choose_frames([10, 25, 40, 55], metrics, set(), 'quality_coverage')
        self.assertEqual(selected, [16, 31, 46, 61])
        self.assertEqual(selected[:3][1:], selected[1:][:2])

    def test_artifacts_and_decode_failures_cannot_win_on_sharpness(self):
        metrics = {i: dict(decoded=True, sharpness=1, corner_coverage=.5) for i in range(10)}
        metrics[4]['sharpness'] = 10000
        metrics[6] = dict(decoded=False)
        selected, _ = choose_frames([5], metrics, {4}, 'quality_coverage')
        self.assertEqual(selected, [5])

    def test_no_eligible_frame_fails_instead_of_bridging_long_gap(self):
        with self.assertRaises(ValueError):
            choose_frames([20], {0: dict(decoded=True)}, set(), 'streak_only')

    def test_blank_image_is_finite_without_features(self):
        q = image_quality(np.zeros((480, 640, 3), dtype=np.uint8))
        self.assertEqual(q['corners'], 0)
        self.assertEqual(q['corner_coverage'], 0)
        self.assertEqual(q['sharpness'], 0)
        with self.assertRaises(ValueError): image_quality(None)


if __name__ == '__main__': unittest.main()
