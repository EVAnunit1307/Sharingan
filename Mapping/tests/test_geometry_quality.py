"""Regressions for misleading coverage and coordinate-gauge comparisons."""
from types import SimpleNamespace
import json
from pathlib import Path
import tempfile
import unittest

import numpy as np
import pycolmap

from Mapping.geometry_quality import compare, assess


def model(points, orientation=None):
    rotation=pycolmap.Rotation3d(np.eye(3) if orientation is None else orientation)
    images={}
    for index,point in enumerate(points):
        images[index]=SimpleNamespace(name=str(index),has_pose=True,
            projection_center=lambda p=point: np.array(p),
            cam_from_world=lambda: SimpleNamespace(rotation=rotation))
    return SimpleNamespace(images=images)


class GeometryQualityTests(unittest.TestCase):
    def setUp(self):
        self.points=np.random.default_rng(2).normal(size=(30,3))

    def test_scale_translation_rotation_do_not_count_as_shape_changes(self):
        angle=.7
        Q=np.array([[np.cos(angle),-np.sin(angle),0],[np.sin(angle),np.cos(angle),0],[0,0,1]])
        moved=3.2*(self.points@Q.T)+[7,-4,3]
        result=compare(model(self.points),model(moved,Q.T))
        self.assertLess(result['center_rmse_fraction_of_path_spread'],1e-10)
        self.assertLess(result['rotation_p90_degrees'],1e-5)
        self.assertEqual(assess(result,30)['status'],'repeatable_unvalidated')

    def test_equal_camera_count_does_not_hide_distorted_shape(self):
        warped=self.points.copy();warped[5:15]+=[0,5,0]
        result=assess(compare(model(self.points),model(warped)),30)
        self.assertEqual(result['status'],'unstable')

    def test_partial_repeatability_is_not_full_coverage(self):
        result=assess(compare(model(self.points),model(self.points)),70)
        self.assertEqual(result['status'],'repeatable_partial')
        self.assertEqual(result['physical_accuracy'],'unvalidated')

    def test_small_intersection_and_absent_baseline_are_inconclusive(self):
        for points in (self.points[:3],np.zeros((30,3))):
            result=assess(compare(model(points),model(points)),30)
            self.assertEqual(result['status'],'inconclusive')

    def test_dropped_views_and_nonfinite_diagnostics_cannot_pass(self):
        result=compare(model(self.points),model(self.points[:20]))
        self.assertEqual(assess(result,30)['status'],'unstable')
        result['center_rmse_fraction_of_path_spread']=float('nan')
        self.assertEqual(assess(result,30)['status'],'inconclusive')

    def test_published_scene_keeps_the_automatic_warning(self):
        from Mapping.reconstruct import atomic_json, export_reconstruction
        with tempfile.TemporaryDirectory() as temporary:
            session=Path(temporary);(session/'images').mkdir()
            for i in range(4):(session/'images'/f'{i}.jpg').touch()
            atomic_json(session/'manifest.json',{'session_id':'test','source':'synthetic_test_only'})
            output=session/'reconstructions'/'test-revision';output.mkdir(parents=True)
            quality=assess({'failure':'One build produced no usable model'},4)
            atomic_json(output/'quality.json',quality)
            reconstruction=model(self.points[:4])
            reconstruction.num_reg_images=lambda:4
            reconstruction.num_points3D=lambda:20
            reconstruction.compute_mean_reprojection_error=lambda:1.
            reconstruction.export_PLY=lambda path: path.write_text('test-only')
            reconstruction.points3D={i:SimpleNamespace(xyz=self.points[i],color=[100,100,100]) for i in range(20)}
            scene=export_reconstruction(session,output,{0:reconstruction},experimental=True)
            self.assertEqual(scene['geometry_quality']['status'],'inconclusive')
            self.assertEqual(scene['quality_warning'],quality['warning'])
            self.assertEqual(json.loads((session/'scene.json').read_text()),json.loads((output/'scene.json').read_text()))


if __name__=='__main__':unittest.main()
