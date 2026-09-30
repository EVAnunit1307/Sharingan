import json
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import patch,Mock

import numpy as np

from Mapping.depth_geometry import robust_affine,inverse_to_depth,sample_image,voxel_average,relative_errors
from Mapping.depth_live import LiveDepth
from Mapping.pose_live import LivePose
from Mapping.server import create_app


class DepthGeometryTests(unittest.TestCase):
    def test_robust_alignment_recovers_known_geometry_with_outliers(self):
        rng=np.random.default_rng(4);prediction=np.linspace(1,20,300)
        inverse_depth=.08*prediction+.12+rng.normal(0,.002,len(prediction))
        inverse_depth[::5]+=rng.normal(0,2,len(prediction[::5]))
        slope,shift=robust_affine(prediction,inverse_depth)
        held=np.array([2.5,7.5,14.5])
        self.assertTrue(np.allclose(inverse_to_depth(held,slope,shift),1/(.08*held+.12),rtol=.01))

    def test_unobservable_alignment_and_negative_depth_are_rejected(self):
        with self.assertRaises(ValueError):robust_affine(np.ones(30),np.arange(30))
        depth=inverse_to_depth(np.array([0,1,2.]),1,-1)
        self.assertTrue(np.isnan(depth[:2]).all());self.assertEqual(depth[2],1.)

    def test_pixel_center_resize_preserves_coordinates_and_invalid_points(self):
        image=np.arange(16,dtype=np.float32).reshape(4,4)
        result=sample_image(image,np.array([[.5,.5],[1.5,2.5],[np.nan,1],[-10,1]]),4,4)
        self.assertEqual(result[0],0);self.assertEqual(result[1],9)
        self.assertTrue(np.isnan(result[2:]).all())
        resized=sample_image(image,np.array([[1.,1.],[3.,5.]]),8,8)
        np.testing.assert_allclose(resized,[0,9])

    def test_voxel_fusion_preserves_average_and_separates_distant_surfaces(self):
        xyz=np.array([[.1,0,0],[.2,0,0],[2,0,0]])
        rgb=np.array([[0,0,0],[200,100,50],[255,0,0]])
        points,colors,support=voxel_average(xyz,rgb,np.array([3,5,3]),1)
        np.testing.assert_allclose(points,[[.15,0,0],[2,0,0]])
        np.testing.assert_array_equal(colors,[[100,50,25],[255,0,0]])
        np.testing.assert_allclose(support,[4,3])

    def test_relative_error_ignores_invalid_geometry(self):
        report=relative_errors(np.array([1.1,np.nan,0,4]),np.array([1,2,3,4]))
        self.assertEqual(report['count'],2);self.assertAlmostEqual(report['median'],.05)


class DepthAPITests(unittest.TestCase):
    def test_stale_model_alignment_is_never_served_as_current(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);sid='20260925T185819Z-14c01c84';session=root/sid;session.mkdir()
            (session/'scene.json').write_text(json.dumps(dict(revision='new')))
            (session/'depth.json').write_text(json.dumps(dict(source_revision='old',points=[])))
            client=create_app(None,root).test_client()
            self.assertEqual(client.get('/map-api/depth/'+sid).status_code,409)
            (session/'depth.json').write_text(json.dumps(dict(source_revision='new',points=[])))
            self.assertEqual(client.get('/map-api/depth/'+sid).status_code,200)
            self.assertEqual(client.post('/map-api/live-depth/start',headers={'Origin':'http://foreign'}).status_code,403)
            self.assertEqual(client.get('/map-api/live-depth/preview.jpg').status_code,503)

    def test_old_live_result_is_not_fresh_and_dead_worker_is_stopped(self):
        with tempfile.TemporaryDirectory() as directory:
            manager=LiveDepth(None,Path(directory));process=Mock();process.poll.return_value=None;manager.process=process
            (Path(directory)/'status.json').write_text(json.dumps(dict(state='live',processed_at=time.time()-10)))
            self.assertFalse(manager.status()['fresh'])
            (Path(directory)/'status.json').write_text(json.dumps(dict(state='live',processed_at=time.time())))
            self.assertTrue(manager.status()['fresh'])
            process.poll.return_value=1;self.assertEqual(manager.status()['state'],'stopped');manager.process=None

    def test_pose_expiry_removes_coordinates_and_lost_state_never_leaks_a_position(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);manager=LivePose(None,root,root=root);process=Mock();process.poll.return_value=None;manager.process=process
            result=dict(state='tracking',processed_at=time.time(),source_age_seconds=.1,position=[1,2,3],rotation=[[1,0,0],[0,1,0],[0,0,1]])
            (root/'status.json').write_text(json.dumps(result));self.assertTrue(manager.status()['fresh'])
            result['source_age_seconds']=1
            (root/'status.json').write_text(json.dumps(result));self.assertNotIn('position',manager.status())
            result.update(state='lost',source_age_seconds=0)
            (root/'status.json').write_text(json.dumps(result));self.assertNotIn('rotation',manager.status())
            manager.process=None


if __name__=='__main__':unittest.main()
