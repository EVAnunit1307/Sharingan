import copy
import unittest

from GroundStation.scene_transport import SCHEMA, chunk_scene
from Mapping.scene_relay_trial import PROFILES, simulate


class RelaySimulationTests(unittest.TestCase):
    def source(self):
        identity = dict(session_id='sim', map_id='map', frame_id='seed', clock_id='clock')
        scenes = []
        for i in range(3):
            scenes.append(dict(schema=SCHEMA, **identity, revision=i+1, units='arbitrary',
                live=False, mapping_eligible=False, observed_ns=i * 3_000_000_000,
                poses=[dict(position=[j, 0, 0], observed_ns=j * 3_000_000_000) for j in range(i+1)],
                points=[[j/100, 0, 1, 180, 130, 90] for j in range(100*(i+1))]))
        return dict(identity=identity, scenes=scenes, warm_model_seconds=[3., 3., 3.])

    def test_batches_are_causal_and_old_estimates_never_become_current(self):
        result = simulate(self.source(), PROFILES[0], duration=16.)
        self.assertEqual(result['summary']['model_ready_seconds'], [3., 6., 9.])
        for state in result['timeline']:
            self.assertIsNone(state['current_camera'])
            if state['revision']:
                self.assertGreater(state['seconds'], state['revision'] * 3.)
        for install in result['summary']['installations']:
            self.assertGreater(install['observation_age_seconds'], 3.)
        self.assertEqual(result['summary']['final_revision'], 3)

    def test_outage_preserves_prior_map_then_recovers_latest_without_backfill(self):
        profile = dict(PROFILES[2], outage=[5., 8.], drop_probability=.05)
        result = simulate(self.source(), profile, duration=20.)
        middle = [s for s in result['timeline'] if 5.5 <= s['seconds'] <= 8.]
        self.assertTrue(middle)
        self.assertTrue(all(s['revision'] == 1 for s in middle))
        self.assertFalse([s for s in result['timeline'] if s['seconds'] == 7.5][0]['link_recent'])
        revisions = [i['revision'] for i in result['summary']['installations']]
        self.assertEqual(revisions, sorted(set(revisions)))
        self.assertEqual(result['summary']['final_revision'], 3)
        self.assertGreater(result['summary']['counters']['dropped_packets'], 0)

    def test_slow_link_coalesces_unsent_maps_and_queue_is_bounded(self):
        source = self.source(); profile = dict(PROFILES[0], bits_per_second=16000)
        result = simulate(source, profile, duration=60.)
        self.assertGreater(result['summary']['counters']['superseded_unsent_chunks'], 0)
        limit = max(len(chunk_scene(s)) for s in source['scenes'])
        self.assertLessEqual(result['summary']['max_queued_map_chunks'], limit)
        self.assertLessEqual(result['summary']['max_queued_priority_packets'], 3)
        self.assertEqual(result['summary']['final_revision'], 3)

    def test_same_seed_same_results_and_input_is_not_mutated(self):
        source = self.source(); original = copy.deepcopy(source)
        profile = dict(PROFILES[2], outage=[5., 8.])
        one = simulate(source, profile, duration=20.)
        two = simulate(source, profile, duration=20.)
        self.assertEqual(one, two); self.assertEqual(source, original)
        self.assertLessEqual(one['summary']['wire_bytes_delivered'], one['summary']['wire_bytes_sent'])


if __name__ == '__main__': unittest.main()
