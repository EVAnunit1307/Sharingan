import json
from pathlib import Path
import tempfile
import unittest

import cv2
import numpy as np
import pycolmap

from Mapping.camera_calibration import load,colmap_params,opencv_parameters,apply_database


class AppliedCalibrationTests(unittest.TestCase):
    def data(self):
        geometry=dict(width=640,height=480,rotation=180,source='picamera2')
        return dict(passes_basic_checks=True,camera_model='OPENCV',width=640,height=480,
                    pixel_coordinates='opencv_integer_pixel_centers',processed_geometry=geometry,
                    params=[793,795,299,350,-.36,.19,-.001,.002])

    def test_opt_in_and_camera_geometry_mismatch(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);self.assertIsNone(load(root));data=self.data()
            (root/'manifest.json').write_text(json.dumps({'camera':data['processed_geometry']}))
            (root/'camera-calibration.json').write_text(json.dumps(data))
            self.assertEqual(load(root)['params'],data['params'])
            for key,value in [('rotation',0),('width',1280),('source','another_camera')]:
                bad=dict(data['processed_geometry']);bad[key]=value
                (root/'manifest.json').write_text(json.dumps({'camera':bad}))
                with self.assertRaises(ValueError):load(root)

    def test_failed_candidate_and_unknown_pixel_convention_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);data=self.data()
            (root/'manifest.json').write_text(json.dumps({'camera':data['processed_geometry']}))
            for key,value in [('passes_basic_checks',False),('pixel_coordinates','unknown'),('params',[1]*7)]:
                bad=dict(data);bad[key]=value
                (root/'camera-calibration.json').write_text(json.dumps(bad))
                with self.assertRaises(ValueError):load(root)

    def test_opencv_colmap_projection_agree_after_pixel_center_conversion(self):
        data=self.data();camera=pycolmap.Camera(model='OPENCV',width=640,height=480,params=colmap_params(data))
        K,dist=opencv_parameters(camera);xyz=np.array([[-1,-.8,4],[.8,.7,3],[0,0,5]],float)
        projected,_=cv2.projectPoints(xyz,np.zeros(3),np.zeros(3),K,dist)
        np.testing.assert_allclose(projected.reshape(-1,2),camera.img_from_cam(xyz),atol=1e-9)
        K_original=K.copy();K_original[:2,2]-=.5
        old,_=cv2.projectPoints(xyz,np.zeros(3),np.zeros(3),K_original,dist)
        np.testing.assert_allclose(projected-old,.5,atol=1e-9)

    def test_database_lens_change_discards_old_verified_geometry(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'database.db'
            with pycolmap.Database.open(path) as db:
                db.write_camera(pycolmap.Camera(camera_id=1,model='SIMPLE_RADIAL',width=640,height=480,params=[400,320,240,0]),use_camera_id=True)
                db.write_image(pycolmap.Image(image_id=1,name='one.jpg',camera_id=1),use_image_id=True)
                db.write_image(pycolmap.Image(image_id=2,name='two.jpg',camera_id=1),use_image_id=True)
                geometry=pycolmap.TwoViewGeometry();geometry.inlier_matches=np.array([[0,0]],np.uint32)
                db.write_two_view_geometry(1,2,geometry)
            apply_database(path,self.data())
            with pycolmap.Database.open(path) as db:
                self.assertEqual(db.read_camera(1).model_name,'OPENCV')
                self.assertEqual(db.num_verified_image_pairs(),0)


if __name__=='__main__':unittest.main()
