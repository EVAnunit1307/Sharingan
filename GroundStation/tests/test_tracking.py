import unittest
from GroundStation.fusion import FusionConfig,FusionEngine
from GroundStation.server import RelayState
from SensorRig.CV.tracking_math import MotionFilter,assignment
from test_fusion import packet,person,target


class TrackingTests(unittest.TestCase):
    def setUp(self):
        self.engine=FusionEngine(FusionConfig(alignment_confirmed=True))

    def test_global_assignment_can_avoid_greedy_dead_end(self):
        self.assertEqual(assignment([[1,2],[2,100]],12),{0:1,1:0})
        self.assertEqual(assignment([[float('inf')]],12),{})

    def test_motion_filter_rejects_teleport_without_refresh_and_estimates_velocity(self):
        f=MotionFilter(0,2,0)
        for i in range(1,16):
            self.assertTrue(f.update(i*.05,2,i*.1))
        self.assertAlmostEqual(f.state()['velocity_right_mps'],.5,delta=.06)
        before=f.state()
        self.assertFalse(f.update(10,2,1.6))
        self.assertEqual(before,f.state())
        self.assertFalse(f.update(0,2,1.5))

    def test_camera_radar_loss_and_return_preserve_fused_id(self):
        first=self.engine.ingest(packet(),now=0)['tracks']
        second=self.engine.ingest(packet(2),now=.1)['tracks']
        cid=next(t['id'] for t in first if t['camera_id']==1)
        self.assertEqual(len(second),1)
        self.assertEqual(second[0]['id'],cid)
        third=self.engine.ingest(packet(3,people=[]),now=.2)['tracks']
        self.assertEqual(third[0]['id'],cid)
        self.assertEqual(third[0]['position_source'],'radar_only')
        self.assertEqual(third[0]['person_evidence'],'previously_seen')
        self.engine.ingest(packet(4),now=.3)
        fifth=self.engine.ingest(packet(5),now=.4)['tracks']
        self.assertEqual(len(fifth),1)
        self.assertEqual(fifth[0]['id'],cid)

    def test_empty_and_expired_samples_never_render_from_track_memory(self):
        self.engine.ingest(packet(),now=0)
        self.engine.ingest(packet(2),now=.1)
        self.assertEqual(self.engine.snapshot(now=.851)['tracks'],[])
        self.assertEqual(self.engine.ingest(packet(3,people=[],targets=[]),now=.9)['tracks'],[])

    def test_pose_expiry_does_not_get_renewed_by_fresh_radar(self):
        self.engine.ingest(packet(),now=0)
        self.engine.ingest(packet(2),now=.1)
        pose=dict(capture_ms=1200,age_ms=0,frame_id=2,generation=1,
            joints=[[0,0,0,1] for _ in range(33)],facing_deg=180,height_m=None)
        self.engine.tracker.attach_pose(('C',1,1),pose,.1)
        self.assertEqual(self.engine.snapshot(now=.1)['tracks'][0]['pose_source'],'camera')
        fresh=self.engine.ingest(packet(6,people=[]),now=.5)['tracks'][0]
        self.assertEqual(fresh['pose_source'],'estimated_motion')
        self.assertIsNone(fresh['pose'])

    def test_restart_drops_pose_and_old_identity(self):
        first=self.engine.ingest(packet(),now=0)['tracks'][0]['id']
        p=packet();p['source_session_id']='new-pi'
        second=self.engine.ingest(p,now=.1)['tracks'][0]
        self.assertNotEqual(first,second['id'])
        self.assertIsNone(second['pose'])

    def test_in_fov_camera_miss_keeps_independent_radar_unverified(self):
        row=self.engine.ingest(packet(people=[]),now=0)['tracks'][0]
        self.assertEqual(row['camera_visibility'],'in_fov_occlusion_unknown')
        self.assertEqual(row['person_evidence'],'unverified')

    def test_untracked_rig_does_not_claim_quest_world_placement(self):
        self.engine.set_config(FusionConfig(rig_motion_mode='untracked'))
        result=self.engine.ingest(packet(people=[]),now=0)
        self.assertFalse(result['rig_pose_valid'])
        self.assertEqual(len(result['tracks']),1) # Relative diagnostics still work.

    def test_controller_mode_preserves_relative_samples_without_inventing_walking(self):
        self.engine.set_config(FusionConfig(rig_motion_mode='left_controller'))
        first=self.engine.ingest(packet(people=[]),now=0)
        second=self.engine.ingest(packet(2,people=[]),now=.1)
        self.assertFalse(second['rig_pose_valid']) # Only the receiving Quest can validate its controller.
        self.assertEqual(second['rig_motion_mode'],'left_controller')
        self.assertEqual(first['tracks'][0]['id'],second['tracks'][0]['id'])
        self.assertEqual(second['tracks'][0]['velocity_right_mps'],0)
        self.assertEqual(second['tracks'][0]['velocity_forward_mps'],0)

    def test_controller_uses_accepted_radar_measurement_before_local_smoothing(self):
        self.engine.set_config(FusionConfig(rig_motion_mode='left_controller'))
        value=packet(people=[])
        value['radar']['targets'][0].update(raw_right_m=.37,raw_forward_m=2.8)
        track=self.engine.ingest(value,now=0)['tracks'][0]
        self.assertAlmostEqual(track['right_m'],.37)
        self.assertAlmostEqual(track['forward_m'],2.8)

    def test_duplicate_samples_do_not_shrink_motion_uncertainty(self):
        first=self.engine.ingest(packet(),now=0)['tracks'][0]
        duplicate=self.engine.ingest(packet(),now=.2)['tracks'][0]
        self.assertEqual(first['variance_right_m2'],duplicate['variance_right_m2'])
        self.assertAlmostEqual(duplicate['valid_for_ms'],first['valid_for_ms']-200)

    def test_late_pose_cannot_restore_missing_person_or_prior_session(self):
        state=RelayState(FusionConfig(),clock=lambda:.1)
        state.ingest(packet())
        frame=dict(source_session_id='previous',generation=1,frame_id=1,age_ms=0,capture_ms=1100)
        self.assertFalse(state.accept_poses(frame,[],1))
        frame['source_session_id']='pi-session';frame['age_ms']=351
        self.assertFalse(state.accept_poses(frame,[],1))
