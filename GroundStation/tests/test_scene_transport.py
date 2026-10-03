import base64
import copy
import hashlib
import unittest

from GroundStation.scene_transport import SCHEMA, SceneReceiver, chunk_scene, encode


IDENTITY = dict(session_id='test', map_id='room', frame_id='seed', clock_id='recording')


def scene(revision=1):
    return dict(schema=SCHEMA, **IDENTITY, revision=revision, live=False,
                mapping_eligible=False, units='arbitrary', observed_ns=1_000_000_000,
                points=[[i / 100, 0, 0, 120, 160, 200] for i in range(160)],
                poses=[dict(position=[0, 0, 0], observed_ns=1_000_000_000)])


def camera(sequence=1, **kwargs):
    return dict(schema=SCHEMA, kind='camera', **IDENTITY, sequence=sequence,
                **dict(dict(position=[0, 0, 0], observed_ns=1_000_000_000,
                            tracking=True, revision=1), **kwargs))


class SceneTransportTests(unittest.TestCase):
    def receiver(self, **kwargs):
        return SceneReceiver(**IDENTITY, **dict(dict(clock_offset_ns=0), **kwargs))

    def install(self, receiver, revision=1, now=1_000_000_000):
        for p in chunk_scene(scene(revision)): receiver.receive(p, now)

    def test_incomplete_revision_never_replaces_the_visible_map(self):
        r = self.receiver(); self.install(r)
        old = copy.deepcopy(r.scene)
        packets = chunk_scene(scene(2))
        for p in packets[:-1]: r.receive(p, 2_000_000_000)
        self.assertEqual(r.scene, old)
        self.assertEqual(r.missing(), [len(packets) - 1])
        self.assertEqual(r.receive(packets[-1], 2_000_000_001), 'installed')
        self.assertEqual(r.scene['revision'], 2)

    def test_reordered_chunks_and_duplicates_are_atomic(self):
        r = self.receiver(); packets = chunk_scene(scene())
        self.assertEqual(r.receive(packets[-1], 1), 'partial')
        self.assertEqual(r.receive(packets[-1], 2), 'duplicate_chunk')
        for i, p in enumerate(reversed(packets[:-1]), 3): r.receive(p, i)
        self.assertEqual(r.scene, scene())
        self.assertEqual(r.receive(packets[0], 100), 'old_revision')

    def test_late_previous_revision_cannot_overwrite_newer_assembly(self):
        r = self.receiver(); a = chunk_scene(scene(1)); b = chunk_scene(scene(2))
        r.receive(a[0], 0); r.receive(b[0], 1)
        self.assertEqual(r.receive(a[1], 2), 'old_revision')
        for p in b[1:]: r.receive(p, 3)
        self.assertEqual(r.scene['revision'], 2)

    def test_checksum_failure_preserves_map_and_can_retry(self):
        r = self.receiver(); self.install(r); packets = chunk_scene(scene(2))
        corrupt = copy.deepcopy(packets)
        raw = bytearray(base64.b64decode(corrupt[0]['payload'])); raw[10] ^= 1
        corrupt[0]['payload'] = base64.b64encode(raw).decode()
        for p in corrupt: result = r.receive(p, 2_000_000_000)
        self.assertEqual(result, 'checksum_rejected'); self.assertEqual(r.scene['revision'], 1)
        for p in packets: result = r.receive(p, 2_000_000_001)
        self.assertEqual(result, 'installed')

    def test_pose_expires_by_observation_time_not_arrival_or_map_refresh(self):
        r = self.receiver(); self.install(r)
        r.receive(camera(), 1_100_000_000)
        self.assertIsNotNone(r.state(2_999_999_999)['current_camera'])
        self.assertIsNone(r.state(3_000_000_001)['current_camera'])
        r.receive(camera(2), 8_000_000_000); self.install(r, 2, 8_000_000_000)
        self.assertIsNone(r.state(8_000_000_000)['current_camera'])
        self.assertEqual(r.state(8_000_000_000)['camera_age_seconds'], 7)

    def test_missing_map_unknown_clock_future_and_tracking_loss_withhold_pose(self):
        r = self.receiver(); r.receive(camera(), 1_000_000_000)
        self.assertIsNone(r.state(1_000_000_000)['current_camera'])
        self.install(r); self.assertIsNotNone(r.state(1_000_000_000)['current_camera'])
        self.assertEqual(r.receive(camera(2, observed_ns=2_000_000_000), 1_000_000_000), 'future_camera')
        r.receive(camera(2, tracking=False), 1_000_000_000)
        self.assertIsNone(r.state(1_000_000_000)['current_camera'])
        unknown = self.receiver(clock_offset_ns=None); self.install(unknown)
        unknown.receive(camera(), 1_000_000_000)
        self.assertIsNone(unknown.state(1_000_000_000)['current_camera'])

    def test_camera_sequence_and_observation_regressions_are_rejected(self):
        r = self.receiver(); r.receive(camera(4), 2_000_000_000)
        self.assertEqual(r.receive(camera(3), 2_000_000_000), 'old_camera')
        self.assertEqual(r.receive(camera(5, observed_ns=999_999_999), 2_000_000_000), 'old_camera')
        self.assertEqual(r.camera_sequence, 4)

    def test_session_frame_map_clock_mismatches_do_not_reset_receiver(self):
        r = self.receiver(); self.install(r)
        for key in IDENTITY:
            packet = chunk_scene(scene(2))[0]; packet[key] = 'different'
            self.assertEqual(r.receive(packet, 2_000_000_000), 'identity_rejected')
            self.assertEqual(r.scene['revision'], 1)

    def test_assembly_timeout_is_bounded_without_erasing_saved_map(self):
        r = self.receiver(); self.install(r)
        packets = chunk_scene(scene(2)); r.receive(packets[0], 2_000_000_000)
        r.state(22_000_000_001)
        self.assertIsNone(r.pending); self.assertEqual(r.scene['revision'], 1)
        for p in packets: r.receive(p, 23_000_000_000)
        self.assertEqual(r.scene['revision'], 2)

    def test_packet_limits_and_conflicting_chunks_are_rejected(self):
        r = self.receiver(); packet = chunk_scene(scene())[0]
        for key, value in [('count', 100000), ('total_bytes', 1000001), ('index', -1),
                           ('payload', 'A' * 20000), ('revision', True), ('sha256', 'z' * 64)]:
            self.assertEqual(r.receive(dict(packet, **{key: value}), 1), 'invalid_chunk')
        r.receive(packet, 2)
        self.assertEqual(r.receive(dict(packet, sha256='0' * 64), 3), 'conflicting_chunk')
        self.assertEqual(len(r.pending['parts']), 1)

    def test_verified_checksum_does_not_bypass_geometry_validation(self):
        r = self.receiver(); self.install(r)
        bad = scene(2); bad['points'][0][0] = 'bad'
        raw = encode(bad); packets = chunk_scene(scene(2))
        # Build bounded, internally consistent framing around invalid geometry.
        from GroundStation.scene_transport import CHUNK_BYTES
        count = (len(raw) + CHUNK_BYTES - 1) // CHUNK_BYTES
        for i in range(count):
            packet = dict(packets[0], index=i, count=count, total_bytes=len(raw),
                          sha256=hashlib.sha256(raw).hexdigest(),
                          payload=base64.b64encode(raw[i*CHUNK_BYTES:(i+1)*CHUNK_BYTES]).decode())
            outcome = r.receive(packet, 2_000_000_000)
        self.assertEqual(outcome, 'invalid_scene'); self.assertEqual(r.scene['revision'], 1)

    def test_delayed_heartbeat_does_not_masquerade_as_reconnection(self):
        r = self.receiver()
        p = dict(schema=SCHEMA, kind='heartbeat', **IDENTITY, sequence=1, observed_ns=0, revision=1)
        r.receive(p, 100_000_000)
        self.assertTrue(r.state(200_000_000)['link_recent'])
        self.assertFalse(r.state(3_000_000_000)['link_recent'])
        self.assertEqual(r.receive(dict(p, sequence=2), 4_000_000_000), 'stale_heartbeat')
        self.assertFalse(r.state(4_000_000_000)['link_recent'])
        with self.assertRaises(ValueError): r.state(3_000_000_000)


if __name__ == '__main__': unittest.main()
