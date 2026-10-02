import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch

import cv2
import numpy as np

from Mapping.reference_benchmark import (evaluate, extract_archive, nearest_index, prepare,
    quaternion_rotation, trajectory_metrics)
from Mapping.window_replay import apply_similarity, compose, local_screen, overlap_transform, replay, robust_similarity


class ReferenceTests(unittest.TestCase):
    def test_timestamp_association_rejects_outside_tolerance(self):
        times=np.array([1_000_000_000,1_100_000_000,1_200_000_000],dtype=np.int64)
        self.assertEqual(nearest_index(times,1_115_000_000),1)
        self.assertIsNone(nearest_index(times,1_150_000_000))
        self.assertIsNone(nearest_index(times,500_000_000))
        self.assertEqual(nearest_index(times,1_200_000_000),2)

    def test_tum_quaternion_order_and_reference_alignment(self):
        rotation=quaternion_rotation([0,0,np.sqrt(.5),np.sqrt(.5)])
        np.testing.assert_allclose(rotation@[1,0,0],[0,1,0],atol=1e-12)
        p=np.array([[0,0,0],[1,0,0],[0,1,0],[0,0,1],[1,1,1]],float)
        g=2*p@rotation.T+[3,4,5]
        metrics=trajectory_metrics(p,g)
        self.assertAlmostEqual(metrics['fitted_scale'],2)
        self.assertLess(metrics['ate_rmse_metres'],1e-10)
        with self.assertRaises(ValueError):quaternion_rotation([0,0,0,0])

    def test_early_alignment_keeps_later_drift_visible(self):
        p=np.array([[0,0,0],[1,0,0],[0,1,0],[0,0,1],[1,1,1],[2,1,1]],float)
        g=2*p+[3,4,5]
        p[4:]+=[.5,0,0]
        metrics=trajectory_metrics(p,g,4)
        self.assertAlmostEqual(metrics['heldout_rmse_metres'],1)
        with self.assertRaises(ValueError):trajectory_metrics(np.zeros((6,3)),g)

    def test_import_keeps_reference_out_of_rgb_frames(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);dataset=root/'dataset';dataset.mkdir()
            (dataset/'rgb').mkdir();(dataset/'depth').mkdir()
            rgb=[];depth=[];poses=[]
            for i in range(4):
                stamp=f'1305031910.{i:09}'
                cv2.imwrite(str(dataset/f'rgb/{i}.png'),np.full((8,8,3),i*30,np.uint8))
                cv2.imwrite(str(dataset/f'depth/{i}.png'),np.full((8,8),5000,np.uint16))
                rgb.append(f'{stamp} rgb/{i}.png');depth.append(f'{stamp} depth/{i}.png')
                poses.append(f'{stamp} {i} 0 0 0 0 0 1')
            for name,rows in [('rgb',rgb),('depth',depth),('groundtruth',poses)]:
                (dataset/f'{name}.txt').write_text('\n'.join(rows))
            output=root/'input';prepare(dataset,output,stride=1)
            frames=[json.loads(x) for x in (output/'frames.jsonl').read_text().splitlines()]
            reference=json.loads((output/'reference.json').read_text())
            self.assertEqual(frames[1]['sensor_timestamp_ns']-frames[0]['sensor_timestamp_ns'],1)
            self.assertNotIn('position',frames[0]);self.assertNotIn('depth',frames[0])
            self.assertEqual((output/frames[2]['image']).read_bytes(),(dataset/'rgb/2.png').read_bytes())
            self.assertEqual(reference['frames'][2]['position'],[2.,0.,0.])

    def test_archive_does_not_extract_traversal_or_links(self):
        for kind in ('traversal','link'):
            with self.subTest(kind=kind),tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp);archive=root/'bad.tgz'
                with tarfile.open(archive,'w:gz') as tar:
                    item=tarfile.TarInfo('../escape' if kind=='traversal' else 'linked')
                    if kind=='link':item.type=tarfile.SYMTYPE;item.linkname='/tmp'
                    else:item.size=1
                    tar.addfile(item,io.BytesIO(b'x'))
                with self.assertRaises(ValueError):extract_archive(archive,root/'out')
                self.assertFalse((root/'out').exists())

    def test_depth_evaluation_uses_png_factor_and_one_trajectory_scale(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);session=root/'input';trial=root/'trial';session.mkdir();trial.mkdir()
            c=np.array([[0,0,0],[1,0,0],[0,1,0],[0,0,1]],float)
            ex=np.concatenate([np.tile(np.eye(3),(4,1,1)),(-c)[:,:,None]],axis=2)
            np.savez(trial/'prediction.npz',extrinsics=ex,depth=np.ones((4,16,16)))
            (trial/'summary.json').write_text(json.dumps(dict(state='complete',session_id='input',indices=list(range(4)))))
            frames=[]
            for i in range(4):
                raw=np.full((16,16),10000,np.uint16);raw[0,0]=0
                cv2.imwrite(str(session/f'{i}.png'),raw)
                frames.append(dict(position=(2*c[i]+[3,4,5]).tolist(),rotation_world_from_camera=np.eye(3).tolist(),depth=f'{i}.png'))
            (session/'reference.json').write_text(json.dumps(dict(dataset='synthetic',depth_divisor=5000,frames=frames)))
            result=evaluate(trial,session)
            self.assertAlmostEqual(result['trajectory']['fitted_scale'],2)
            self.assertAlmostEqual(result['depth']['mean_frame_abs_relative'],0)
            self.assertEqual(result['depth']['frames'][0]['valid_pixels'],255)
            self.assertFalse(result['reference_used_for_inference'])


class OverlapTests(unittest.TestCase):
    def test_local_screen_requires_at_least_half_of_adjacent_pairs(self):
        data=dict(images=np.empty((16,1,1,3)))
        check=dict(median_of_pair_medians_pixels=1.,image_width=392,pairs_with_at_least_8_matches=7)
        with patch('Mapping.window_replay.diagnostics',return_value=check):
            self.assertFalse(local_screen(data)['accepted'])
            check['pairs_with_at_least_8_matches']=8
            self.assertTrue(local_screen(data)['accepted'])

    def test_outliers_do_not_hide_known_similarity(self):
        rng=np.random.default_rng(8);source=rng.normal(size=(300,3))
        rotation=quaternion_rotation([0,0,np.sqrt(.5),np.sqrt(.5)])
        expected=(2.,rotation,np.array([4.,-3.,1.]))
        target=apply_similarity(source,expected);target[:70]+=rng.normal(0,20,(70,3))
        actual,fraction=robust_similarity(source,target,.02)
        np.testing.assert_allclose(apply_similarity(source[70:],actual),target[70:],atol=1e-8)
        self.assertGreater(fraction,.7)
        with self.assertRaises(ValueError):robust_similarity(np.zeros((20,3)),np.zeros((20,3)),.1)

    def test_transform_composition_preserves_scale_rotation_translation(self):
        r=quaternion_rotation([0,0,np.sqrt(.5),np.sqrt(.5)])
        a=(2.,r,np.array([3.,2.,1.]));b=(.4,r.T,np.array([-2.,1.,3.]))
        p=np.array([[1,2,3],[-2,1,-3]],float)
        np.testing.assert_allclose(apply_similarity(p,compose(a,b)),apply_similarity(apply_similarity(p,b),a))

    def windows(self):
        h,w=126,168;ys,xs=np.mgrid[:h,:w]
        rotation=quaternion_rotation([0,0,np.sqrt(.5),np.sqrt(.5)])
        transform=(2.,rotation,np.array([4.,-3.,1.]))
        k=np.array([[150.,0,w/2],[0,150.,h/2],[0,0,1]])
        def window(ids,moving):
            poses=[];depths=[]
            for i in ids:
                c=np.array([i*.12,np.sin(i)*.02,0.])
                r=rotation if moving else np.eye(3)
                if moving:c=(c-transform[2])@rotation/2
                poses.append(np.column_stack([r,-r@c]))
                depths.append((2+xs/w*.4+ys/h*.15+i*.01)/(2 if moving else 1))
            return dict(report=dict(session_id='test',indices=ids,
                        frames=[{'image':f'images/{i:09}.png'} for i in ids],
                        source_sha256={f'{i:09}.png':str(i) for i in ids}),
                        arrays=dict(depth=np.array(depths),confidence=np.ones((len(ids),h,w)),
                                    extrinsics=np.array(poses),intrinsics=np.tile(k,(len(ids),1,1))))
        return window(list(range(8)),False),window(list(range(4,12)),True),transform

    def test_shared_pixels_recover_camera_and_surface_alignment(self):
        a,b,expected=self.windows();fit,check=overlap_transform(a,b)
        self.assertTrue(check['accepted'],check)
        np.testing.assert_allclose(fit[0],expected[0],atol=1e-7)
        np.testing.assert_allclose(fit[1],expected[1],atol=1e-7)
        np.testing.assert_allclose(fit[2],expected[2],atol=1e-7)
        self.assertEqual(check['fit_views'],[4,6]);self.assertEqual(check['validation_views'],[5,7])

    def test_changed_heldout_views_and_wrong_recording_are_rejected(self):
        a,b,_=self.windows();b['arrays']['depth'][[1,3]]*=1.5
        _,check=overlap_transform(a,b)
        self.assertFalse(check['accepted']);self.assertIn('held-out overlap surfaces disagree',check['reasons'])
        a,b,_=self.windows();b['report']['session_id']='other'
        with self.assertRaises(ValueError):overlap_transform(a,b)
        a,b,_=self.windows();b['report']['source_sha256']['000000004.png']='changed'
        with self.assertRaises(ValueError):overlap_transform(a,b)

    def test_failed_initialization_can_retry_but_lost_map_never_restarts_silently(self):
        a,_,_=self.windows();trials=[]
        for start in (0,8,16,24):
            ids=list(range(start,start+8));data=dict(a['arrays'])
            data['images']=np.zeros((8,126,168,3),np.uint8)
            report=dict(a['report'],indices=ids,frames=[dict(image=f'images/{i:09}.png',sensor_timestamp_ns=i*100_000_000) for i in ids])
            trials.append(dict(report=report,arrays=data))
        screens=[{'accepted':x} for x in (False,True,False,True)]
        with tempfile.TemporaryDirectory() as tmp,patch('Mapping.window_replay.load_trial',side_effect=trials),patch('Mapping.window_replay.local_screen',side_effect=screens):
            reference=Path(tmp)/'test';reference.mkdir()
            (reference/'reference.json').write_text(json.dumps(dict(frames=[dict(position=[(i%8)*.12,np.sin(i%8)*.02,0.]) for i in range(32)])))
            result=replay([Path(f'window-{i}') for i in range(4)],Path(tmp)/'replay',reference)
        self.assertEqual([s['state'] for s in result['steps']],['withheld','seed','withheld','blocked'])
        self.assertEqual([p['index'] for p in result['poses']],list(range(8,16)))
        self.assertEqual(result['steps'][-1]['retained_poses'],8)
        self.assertNotIn('unavailable',result['reference'])
        self.assertEqual(result['reference']['first_window_alignment']['fit_poses'],8)
