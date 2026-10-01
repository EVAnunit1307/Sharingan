from dataclasses import replace
import unittest
import numpy as np

from Mapping.da3_trial import select_indices
from Mapping.operator_sketch import category, fit_floor, map_basis
from Mapping.pose_bridge import PoseSample, place_observation


class WindowTests(unittest.TestCase):
    def test_duration_uses_sensor_clock_and_never_uses_later_frames(self):
        rows=[dict(sensor_timestamp_ns=i*350_000_000,camera_generation=1) for i in range(40)]
        short=select_indices(rows,3,2,duration_seconds=2)
        long=select_indices(rows,3,2,duration_seconds=5)
        self.assertEqual(short,[3,5,7]);self.assertTrue(set(short)<=set(long))
        self.assertTrue(all(rows[i]['sensor_timestamp_ns']-rows[3]['sensor_timestamp_ns']<2e9 for i in short))
        rows[5]['camera_generation']=2
        with self.assertRaises(ValueError):select_indices(rows,3,2,duration_seconds=2)

    def test_invalid_or_nonmonotonic_windows_are_rejected(self):
        rows=[dict(sensor_timestamp_ns=i*100_000_000,camera_generation=1) for i in range(40)]
        for duration in (-1,float('nan'),.01,31):
            with self.assertRaises(ValueError):select_indices(rows,0,2,duration_seconds=duration)
        with self.assertRaises(ValueError):select_indices(rows,0,1,duration_seconds=3)
        rows[4]['sensor_timestamp_ns']=rows[3]['sensor_timestamp_ns']
        with self.assertRaises(ValueError):select_indices(rows,0,2,duration_seconds=2)


class FloorTests(unittest.TestCase):
    def test_multiple_views_support_a_floor_with_outliers_and_right_handed_basis(self):
        rng=np.random.default_rng(17)
        floor=np.column_stack([rng.uniform(-2,2,1500),rng.normal(0,.003,1500),rng.uniform(0,3,1500)])
        noisy=np.concatenate([floor,rng.uniform(-2,2,(150,3))])
        views=np.arange(len(noisy))%3
        cameras=np.array([[0,1,0],[.5,1,.5],[1,1,1]])
        plane,report=fit_floor(noisy,views,cameras,2)
        self.assertTrue(report['available']);self.assertGreater(report['inlier_fraction'],.85)
        self.assertGreater(plane['normal'][1],.99)
        rotation,origin=map_basis(plane,cameras[0],np.eye(3))
        np.testing.assert_allclose(rotation@rotation.T,np.eye(3),atol=1e-8)
        self.assertAlmostEqual(np.linalg.det(rotation),1)
        self.assertAlmostEqual(float((rotation@(cameras[0]-origin))[2]),1,delta=.02)

    def test_too_little_floor_withholds_top_down_geometry(self):
        plane,report=fit_floor(np.zeros((20,3)),np.arange(20)%2,np.ones((2,3)),1)
        self.assertIsNone(plane);self.assertFalse(report['available'])
        self.assertIsNone(category('person'))
        self.assertEqual(category('toilet'),'object') # Furniture identity is intentionally uncertain.


class PoseBridgeTests(unittest.TestCase):
    def setUp(self):
        rotation=np.array([[0,-1,0],[1,0,0],[0,0,1.]])
        self.pose=PoseSample('map-a','camera-clock',1_000_000_000,'arbitrary','da3_recorded_batch',
                             np.array([10.,20.,3.]),rotation)
        self.args=dict(pose=self.pose,timestamp_ns=1_000_000_000,map_id='map-a',clock_id='camera-clock',
                       units='arbitrary',camera_from_sensor=np.eye(4))

    def test_known_rotation_translation_and_sensor_offset(self):
        mount=np.eye(4);mount[:3,3]=[1,0,0]
        result=place_observation([2,0,0],**dict(self.args,camera_from_sensor=mount))
        np.testing.assert_allclose(result,[10,23,3])

    def test_lost_stale_different_map_clock_or_units_withhold_placement(self):
        variants=[{'pose':replace(self.pose,tracking=False)},{'pose':None},
                  {'timestamp_ns':1_100_000_001},{'map_id':'map-b'},
                  {'clock_id':'unsynchronized-imu-clock'},{'units':'metres'}]
        for variant in variants:
            with self.subTest(variant=variant):
                self.assertIsNone(place_observation([2,0,0],**dict(self.args,**variant)))

    def test_future_fused_pose_can_use_same_boundary_without_integrating_imu_here(self):
        fused=replace(self.pose,source='visual_inertial',imu_used=True,units='metres')
        result=place_observation([2,0,0],**dict(self.args,pose=fused,units='metres'))
        np.testing.assert_allclose(result,[10,22,3])
        with self.assertRaises(ValueError):place_observation([float('nan'),0,0],**self.args)
        with self.assertRaises(ValueError):
            place_observation([2,0,0],**dict(self.args,pose=replace(self.pose,rotation_map_from_camera=np.eye(3)*2)))

    def test_invalid_timing_cannot_bypass_freshness_check(self):
        for bad in (float('nan'),float('inf'),1_000_000_000.0,-1,True,np.bool_(False),'1000000000',None):
            variants=[{'timestamp_ns':bad},{'pose':replace(self.pose,timestamp_ns=bad)},{'max_age_ns':bad}]
            for variant in variants:
                with self.subTest(variant=variant):
                    self.assertIsNone(place_observation([2,0,0],**dict(self.args,**variant)))
        result=place_observation([2,0,0],**dict(self.args,timestamp_ns=np.int64(1_100_000_000)))
        np.testing.assert_allclose(result,[10,22,3])
        # A uint64 subtraction must not turn an old pose into a fresh one.
        ancient=replace(self.pose,timestamp_ns=np.uint64(2**64-1))
        self.assertIsNone(place_observation([2,0,0],**dict(self.args,pose=ancient,timestamp_ns=np.uint64(0))))
