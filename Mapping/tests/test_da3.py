import unittest
import numpy as np
from Mapping.da3_inspect import centers, compare_poses, similarity


class GeometryTests(unittest.TestCase):
    def test_camera_centres_and_similarity_follow_world_to_camera_convention(self):
        points=np.array([[0,0,0],[1,0,0],[0,2,0],[0,0,3]],dtype=float)
        rotation=np.array([[0,-1,0],[1,0,0],[0,0,1]],dtype=float)
        translation=np.array([4.,-3.,2.])
        target=2*points@rotation.T+translation
        scale,r,t=similarity(points,target)
        np.testing.assert_allclose(scale,2)
        np.testing.assert_allclose(r,rotation,atol=1e-8)
        np.testing.assert_allclose(t,translation)
        ex=np.concatenate([np.repeat(rotation[None],4,axis=0),(-target@rotation.T)[:,:,None]],axis=2)
        np.testing.assert_allclose(centers(ex),target)

    def test_pose_and_depth_comparison_is_invariant_to_coordinate_frame_and_scale(self):
        ca=np.array([[0,0,0],[1,0,0],[0,2,0],[0,0,3]],dtype=float)
        r=np.array([[0,-1,0],[1,0,0],[0,0,1]],dtype=float); t=np.array([4.,-3.,2.])
        cb=(ca-t)@r/2
        ea=np.concatenate([np.repeat(np.eye(3)[None],4,axis=0),(-ca)[:,:,None]],axis=2)
        # A camera-to-world = r @ B camera-to-world, hence B w2c = r.
        eb=np.concatenate([np.repeat(r[None],4,axis=0),(-cb@r.T)[:,:,None]],axis=2)
        a={'report':{'indices':list(range(4))},'arrays':{'extrinsics':ea,'depth':np.ones((4,2,2))*6}}
        b={'report':{'indices':list(range(4))},'arrays':{'extrinsics':eb,'depth':np.ones((4,2,2))*3}}
        result=compare_poses(a,b)
        self.assertLess(result['path_rmse_fraction_of_spread'],1e-8)
        self.assertLess(result['orientation_p90_degrees'],1e-5)
        self.assertLess(result['aligned_depth_relative_change_median'],1e-8)

    def test_stationary_inputs_do_not_claim_repeatable_scaled_geometry(self):
        ex=np.tile(np.eye(4)[:3],(4,1,1))
        trial={'report':{'indices':list(range(4))},'arrays':{'extrinsics':ex,'depth':np.ones((4,2,2))}}
        with self.assertRaises(ValueError):compare_poses(trial,trial)
