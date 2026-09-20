import unittest
from types import SimpleNamespace as N
from GroundStation.fusion import FusionConfig
from GroundStation.pose import extract_poses,floor_position,validate_frame


class PoseTests(unittest.TestCase):
    def frame(self):
        return dict(source_session_id='pi',generation=1,frame_id=2,capture_ms=1000,age_ms=0,
                    width=640,height=480,hfov=62,people=[dict(id=1,observed=True,box=[100,10,540,470])])

    def result(self):
        points=[N(x=.3+(i%2)*.4,y=.1+i/40,z=0,visibility=.9,presence=.9) for i in range(33)]
        world=[N(x=p.x-.5,y=p.y-.5,z=.01*i,visibility=.9,presence=.9) for i,p in enumerate(points)]
        return N(pose_landmarks=[points],pose_world_landmarks=[world])

    def test_pose_is_bound_to_same_frame_and_preserves_visibility(self):
        frame=self.frame();poses=extract_poses(self.result(),frame,FusionConfig())
        self.assertEqual(len(poses),1)
        self.assertEqual(poses[0]['camera_id'],1)
        self.assertEqual(poses[0]['capture_ms'],1000)
        self.assertEqual(len(poses[0]['joints']),33)
        self.assertIsNone(poses[0]['floor_position'])
        self.assertIsNone(poses[0]['height_m'])

    def test_coasting_box_cannot_receive_new_pose_evidence(self):
        frame=self.frame();frame['people'][0]['observed']=False
        self.assertEqual(extract_poses(self.result(),frame,FusionConfig()),[])

    def test_ambiguous_overlapping_boxes_are_not_forced_to_match(self):
        frame=self.frame();frame['people'].append(dict(frame['people'][0],id=2))
        self.assertEqual(extract_poses(self.result(),frame,FusionConfig()),[])

    def test_nonfinite_or_low_visibility_joints_reject_pose(self):
        r=self.result();r.pose_world_landmarks[0][10].x=float('nan')
        self.assertEqual(extract_poses(r,self.frame(),FusionConfig()),[])
        r=self.result()
        for p in r.pose_landmarks[0]:p.visibility=.1
        self.assertEqual(extract_poses(r,self.frame(),FusionConfig()),[])

    def test_floor_projection_requires_known_height_supported_feet_and_fixed_rig(self):
        points=[[320,400,.9] for _ in range(33)];frame=self.frame()
        self.assertIsNone(floor_position(points,frame,FusionConfig()))
        config=FusionConfig(camera_height_m=1,camera_fx_px=500,camera_fy_px=500)
        pos=floor_position(points,frame,config)
        self.assertAlmostEqual(pos['forward_m'],3.125)
        self.assertAlmostEqual(pos['right_m'],0)
        self.assertIsNone(floor_position(points,frame,FusionConfig(camera_height_m=1,rig_motion_mode='untracked')))
        points[31][2]=.1
        self.assertIsNone(floor_position(points,frame,config))

    def test_invalid_frame_metadata_is_rejected(self):
        for change in ({'width':99999},{'generation':True},{'age_ms':float('nan')},{'capture_ms':-1}):
            with self.subTest(change=change),self.assertRaises(ValueError):
                validate_frame(dict(self.frame(),**change))
