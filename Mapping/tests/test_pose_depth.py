import unittest

import cv2
import numpy as np

from Mapping.pose_depth import world_to_camera, normalize_extrinsics, anchor_depth, tracked_extrinsics, rectification_maps
from Mapping.da3_inspect import centers
from Mapping.pose_depth_compare import errors


class PoseDepthTests(unittest.TestCase):
    def test_orb_pose_inversion_projects_a_known_camera_point(self):
        # 90 degree z rotation: camera x maps to world y.
        ex = world_to_camera([3, 4, 5], [0, 0, 2**-.5, 2**-.5])
        np.testing.assert_allclose(ex @ [3, 6, 8, 1], [2, 0, 3, 1], atol=1e-7)
        np.testing.assert_allclose(centers(ex[None, :3]), [[3, 4, 5]])

    def test_normalization_preserves_relative_rotation_and_uses_lower_median(self):
        ex = np.array([world_to_camera([5+x, 0, 0], [0, 0, 0, 1]) for x in (0, 2, 4, 6)])
        normalized, divisor = normalize_extrinsics(ex)
        self.assertEqual(divisor, 2)
        np.testing.assert_allclose(normalized[0], np.eye(4))
        np.testing.assert_allclose(centers(normalized[:, :3])[:, 0], [0, 1, 2, 3])

    def test_anchor_depth_divides_by_input_to_model_scale(self):
        points = np.array([[0,0,0], [1,0,0], [0,2,0], [0,0,3]], float)
        ex = np.array([world_to_camera(p, [0,0,0,1]) for p in points])
        model = ex[:, :3].copy(); model[:, :, 3] *= 3
        k = np.repeat(np.eye(3)[None], 4, axis=0)
        arrays = dict(depth=np.ones((4,2,2))*6, extrinsics=model, intrinsics=k*2)
        result, metadata = anchor_depth(arrays, ex, k)
        np.testing.assert_allclose(result['depth'], 2)
        np.testing.assert_allclose(result['extrinsics'], ex[:, :3])
        np.testing.assert_allclose(result['intrinsics'], k)
        self.assertAlmostEqual(metadata['model_units_per_orb_unit'], 3)
        np.testing.assert_allclose(arrays['depth'], 6)  # preserve raw predictions

    def test_mismatched_tracking_or_lost_frames_cannot_condition(self):
        rows = [dict(image=f'images/{i}.jpg', sensor_timestamp_ns=i*1000000000) for i in range(2)]
        frames = [dict(image=r['image'], seconds=i, state=2, map_id=0,
                       position=[i,0,0], quaternion=[0,0,0,1]) for i,r in enumerate(rows)]
        tracking = dict(kind='orb_tracking_replay', session_id='s', calibration_sha256='cal', frames=frames)
        tracked_extrinsics(tracking,'s',rows,[0,1],'cal')
        for field, value in [('state',3),('map_id',1),('seconds',1.1),('image','wrong.jpg')]:
            previous=frames[1][field]; frames[1][field]=value
            with self.assertRaises(ValueError): tracked_extrinsics(tracking,'s',rows,[0,1],'cal')
            frames[1][field]=previous
        with self.assertRaises(ValueError): tracked_extrinsics(tracking,'s',rows,[0,1],'wrong calibration')

    def test_undistortion_map_matches_distorted_projection(self):
        k=np.array([[250,0,160],[0,250,120],[0,0,1]],float)
        distortion=np.array([-.25,.05,.001,-.002])
        mx,my=cv2.initUndistortRectifyMap(k,distortion,None,k,(320,240),cv2.CV_32FC1)
        pixels=np.array([[20,30],[160,120],[270,200]],float)
        rays=np.c_[pixels,np.ones(3)]@np.linalg.inv(k).T
        distorted,_=cv2.projectPoints(rays,np.zeros(3),np.zeros(3),k,distortion)
        np.testing.assert_allclose([[mx[int(y),int(x)],my[int(y),int(x)]] for x,y in pixels],
                                   distorted[:,0],atol=2e-5)

    def test_paired_reprojection_detects_wrong_depth_under_known_translation(self):
        k=np.repeat(np.array([[[50.,0,10],[0,50,10],[0,0,1]]]),2,axis=0)
        ex=np.array([world_to_camera([x,0,0],[0,0,0,1])[:3] for x in (0,.1)])
        data=dict(intrinsics=k,extrinsics=ex,depth=np.full((2,24,24),2.,np.float32))
        source=np.array([[12.,12.],[15.,14.]])
        target=source-np.array([2.5,0.])  # fx * baseline / plane distance
        pixel,depth,valid=errors(data,0,1,source,target)
        self.assertTrue(valid.all())
        np.testing.assert_allclose(pixel,0,atol=1e-6)
        np.testing.assert_allclose(depth,0,atol=1e-6)
        data['depth']*=2
        pixel,_,_=errors(data,0,1,source,target)
        np.testing.assert_allclose(pixel,1.25,atol=1e-6)

    def test_centered_rectification_matches_da3_and_projects_same_optical_axis(self):
        k=np.array([[793.38,0,299.11],[0,793.33,349.71],[0,0,1]],np.float32)
        distortion=np.array([-.36263,.18997,-.000815,.002367])
        output,mx,my,valid=rectification_maps(k,distortion,(640,480),True)
        self.assertTrue(valid.all())
        np.testing.assert_allclose(output[:2,2],[320,240])
        np.testing.assert_allclose([mx[240,320],my[240,320]],k[:2,2],atol=1e-4)


if __name__ == '__main__': unittest.main()
