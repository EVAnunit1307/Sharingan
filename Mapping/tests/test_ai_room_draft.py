import hashlib
import json
from pathlib import Path
import tempfile
import unittest

import numpy as np

from Mapping.ai_room_draft import build_trial, generate, project_points


def fixture(root):
    trial = root / 'input'
    trial.mkdir()
    n, h, w = 2, 12, 16
    ex = np.repeat(np.c_[np.eye(3), np.zeros(3)][None], n, axis=0)
    ex[1, 0, 3] = -1
    arrays = dict(depth=np.full((n, h, w), 2.),
        confidence=np.tile(np.arange(h * w).reshape(h, w), (n, 1, 1)),
        extrinsics=ex, intrinsics=np.repeat(np.eye(3)[None], n, axis=0),
        images=np.zeros((n, h, w, 3), np.uint8))
    np.savez(trial / 'prediction.npz', **arrays)
    report = dict(state='complete', session_id='fixture</script><script>bad()</script>',
        indices=[0, 1], frames=[dict(sensor_timestamp_ns=1_000_000_000 + i * 100_000_000) for i in range(n)],
        inference_seconds=.1)
    (trial / 'summary.json').write_text(json.dumps(report))
    return trial, arrays


class DraftTests(unittest.TestCase):
    def test_projection_inverts_world_to_camera_transform(self):
        r = np.array([[0., -1, 0], [1, 0, 0], [0, 0, 1]])
        t = np.array([1., 2., 3.])
        ex = np.c_[r, t]
        ys, xs = np.mgrid[0:2, 0:2]
        world = project_points(np.full((2, 2), 2.), np.eye(3), ex, ys, xs)
        expected_camera = np.stack([xs * 2, ys * 2, np.full_like(xs, 2)], axis=-1).reshape(-1, 3)
        np.testing.assert_allclose(world @ r.T + t, expected_camera)

    def test_weak_geometry_remains_visible_without_becoming_a_map(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); trial, _ = fixture(root)
            before = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in trial.iterdir()}
            draft = build_trial(trial)
            self.assertEqual(draft['display_points'], 2 * 12 * 16)
            self.assertGreater(draft['weak_display_points'], 0)
            self.assertFalse(draft['mapping_eligible'])
            self.assertFalse(draft['validated'])
            self.assertFalse(draft['live'])
            self.assertEqual(draft['units'], 'arbitrary')
            self.assertEqual({p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in trial.iterdir()}, before)

    def test_inline_payload_escapes_html_without_changing_provenance(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); trial, _ = fixture(root)
            output = root / 'draft.html'
            summary = generate([trial], output)
            html = output.read_text()
            self.assertNotIn('<script>bad()', html)
            encoded = html.split('const DATA=', 1)[1].split(';\nif(!DATA)', 1)[0]
            payload = json.loads(encoded)
            self.assertEqual(payload['trials'][0]['session_id'], summary['trials'][0]['session_id'])
            self.assertFalse(payload['mapping_eligible'])
            with self.assertRaises(ValueError):
                generate([trial], output)

    def test_nonfinite_or_mismatched_geometry_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); trial, arrays = fixture(root)
            arrays['depth'][0, 0, 0] = np.nan
            np.savez(trial / 'prediction.npz', **arrays)
            with self.assertRaises(ValueError):
                build_trial(trial)
            arrays['depth'][0, 0, 0] = 2
            arrays['intrinsics'] = np.eye(3)
            np.savez(trial / 'prediction.npz', **arrays)
            with self.assertRaises(ValueError):
                build_trial(trial)

    def test_misaligned_semantics_are_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); trial, _ = fixture(root)
            np.savez(trial / 'semantics.npz', labels=np.zeros((2, 4, 4)), scores=np.ones((2, 4, 4)))
            (trial / 'semantics.json').write_text(json.dumps(dict(id2label={'0': 'floor'})))
            with self.assertRaises(ValueError):
                build_trial(trial)

    def test_selected_input_retains_original_recording_row_numbers(self):
        with tempfile.TemporaryDirectory() as tmp:
            trial, _ = fixture(Path(tmp))
            path = trial / 'summary.json'
            report = json.loads(path.read_text())
            for frame, original_index in zip(report['frames'], [192, 216]):
                frame['source_row_index'] = original_index
            path.write_text(json.dumps(report))
            self.assertEqual(build_trial(trial)['indices'], [192, 216])


if __name__ == '__main__':
    unittest.main()
