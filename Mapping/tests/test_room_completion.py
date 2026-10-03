import json
import unittest

import numpy as np

from Mapping.room_completion import cells_from_points, complete_cells, fit_walls, room_envelope


class CompletionTests(unittest.TestCase):
    def test_small_hole_filled_but_not_evidence_or_outside_hull(self):
        seen = {(x, y) for x in range(11) for y in range(11)} - {(5, 5)}
        self.assertEqual(complete_cells(seen), {(5, 5)})

    def test_known_projected_obstacle_blocks_fill(self):
        seen = {(x, y) for x in range(11) for y in range(11)} - {(5, 5)}
        self.assertEqual(complete_cells(seen, blocked={(5, 5)}), set())

    def test_wide_unseen_corridor_is_not_closed(self):
        seen = {(x, y) for x in range(20) for y in range(12) if x < 5 or x > 14}
        self.assertFalse(any(5 <= x <= 14 for x, y in complete_cells(seen)))

    def test_empty_and_unbounded_inputs(self):
        self.assertEqual(complete_cells(set()), set())
        with self.assertRaises(ValueError):
            complete_cells({(0, 0), (2000, 0), (0, 2000)})
        with self.assertRaises(ValueError):
            complete_cells({(0, 0)}, radius=10)

    def test_multiview_cells_have_portable_json_coordinates(self):
        points = np.array([[.2, .2, 0], [.3, .3, 0], [2.1, 2.1, 0]])
        cells = cells_from_points(points, np.array([0, 1, 0]), 1)
        self.assertEqual(json.loads(json.dumps([list(c) for c in cells])), [[0, 0]])

    def test_wall_holdout_does_not_move_fitted_plane(self):
        rng = np.random.default_rng(7)
        points, views = [], []
        for view in range(6):
            cloud = np.column_stack([rng.normal(0, .001, 250), rng.uniform(-1, 1, 250), rng.uniform(.1, 1, 250)])
            points.extend(cloud); views.extend([view] * len(cloud))
        points, views = np.array(points), np.array(views)
        first = fit_walls(points, views, 1, max_planes=1)[0]
        changed = points.copy(); changed[views % 2 == 1, 0] += .3
        second = fit_walls(changed, views, 1, max_planes=1)[0]
        np.testing.assert_array_equal(first['a'], second['a'])
        np.testing.assert_array_equal(first['b'], second['b'])
        self.assertGreater(first['heldout_points_within_tolerance'], 600)
        self.assertEqual(second['heldout_points_within_tolerance'], 0)
        self.assertLess(abs(first['a'][0]), .01)

    def test_envelope_is_explicitly_not_a_recovered_room(self):
        rng = np.random.default_rng(5)
        envelope = room_envelope(rng.uniform(0, 1, (500, 3)), 1)
        self.assertFalse(envelope['measured_boundary'])
        self.assertEqual(envelope['state'], 'unsupported_envelope')
        self.assertIsNone(room_envelope(np.empty((0, 3)), 1))


if __name__ == '__main__':
    unittest.main()
