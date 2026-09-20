import unittest
try:
    import cv2
    import numpy as np
except ImportError:
    cv2=None
from GroundStation.rig_motion import RigMotionMonitor


@unittest.skipIf(cv2 is None,'Optional image processing dependency unavailable')
class RigMotionTests(unittest.TestCase):
    def setUp(self):
        # Non-periodic background: a regular grid has ambiguous optical flow.
        rng=np.random.default_rng(19)
        self.image=cv2.GaussianBlur(rng.integers(0,256,(240,320),dtype=np.uint8),(5,5),0)

    def test_background_movement_latches_until_reference_reset(self):
        m=RigMotionMonitor();m.update(self.image,[],1)
        for i in (1,2):
            moved=cv2.warpAffine(self.image,np.float32([[1,0,i*4],[0,1,0]]),(320,240))
            alarm=m.update(moved,[],1)
        self.assertTrue(alarm['motion_alarm'])
        self.assertFalse(alarm['metric_pose_available'])
        self.assertTrue(m.update(moved,[],1)['motion_alarm'])
        self.assertFalse(m.update(moved,[],2)['motion_alarm'])

    def test_unobserved_or_stationary_image_does_not_claim_a_measured_pose(self):
        m=RigMotionMonitor()
        a=m.update(self.image,[],1);b=m.update(self.image,[],1)
        self.assertFalse(a['metric_pose_available'])
        self.assertFalse(b['motion_alarm'])
        self.assertFalse(b['metric_pose_available'])

    def test_person_box_masks_foreground_motion(self):
        m=RigMotionMonitor();m.update(self.image,[[0,0,320,240]],1)
        shifted=cv2.warpAffine(self.image,np.float32([[1,0,8],[0,1,0]]),(320,240))
        self.assertFalse(m.update(shifted,[[0,0,320,240]],1)['motion_alarm'])
