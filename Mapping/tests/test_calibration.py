import unittest
import json
from pathlib import Path
import tempfile
from unittest.mock import patch
import cv2
import numpy as np
from Mapping.calibrate import fit_views,run


class CalibrationTests(unittest.TestCase):
    def test_varied_synthetic_board_views_recover_intrinsics(self):
        rng=np.random.default_rng(12);obj=np.zeros((54,3),np.float32);obj[:,:2]=np.mgrid[0:9,0:6].T.reshape(-1,2)
        K=np.array([[780,0,320],[0,790,240],[0,0,1]],float);dist=np.array([-.12,.02,0,0,0],float)
        corners=[]
        for _ in range(35):
            rotation=rng.uniform(-.45,.45,3);translation=np.array([rng.uniform(-6,-2),rng.uniform(-4,-1),rng.uniform(16,23)])
            points,_=cv2.projectPoints(obj,rotation,translation,K,dist)
            points+=rng.normal(0,.03,points.shape).astype(np.float32);corners.append(points)
        result=fit_views(corners,(640,480))
        self.assertLess(abs(result['matrix'][0][0]-780),5)
        self.assertLess(result['held_out_median_px'],.15)
        self.assertEqual(result['status'],'candidate_not_applied')

    def test_too_few_views_cannot_claim_calibration(self):
        with self.assertRaises(ValueError):fit_views([],(640,480))

    def test_recording_accepts_flat_corner_arrays_and_deduplicates_repeats(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);session=root/'recording';(session/'images').mkdir(parents=True)
            (session/'manifest.json').write_text(json.dumps({'camera':{'width':640,'height':480}}))
            flat=np.mgrid[0:9,0:6].T.reshape(-1,2).astype(np.float32)*25+20
            detections=[flat,flat.copy(),flat+25]
            for i in range(3):(session/'images'/f'{i:09d}.jpg').touch()
            with patch('Mapping.calibrate.cv2.imread',return_value=np.zeros((480,640),np.uint8)), \
                 patch('Mapping.calibrate.cv2.findChessboardCornersSB',side_effect=[(True,p) for p in detections]), \
                 patch('Mapping.calibrate.fit_views',return_value={'status':'candidate_not_applied'}) as fit, \
                 patch('builtins.print'):
                run(session,root/'candidate.json')
            retained,size=fit.call_args.args
            self.assertEqual(size,(640,480));self.assertEqual(len(retained),2)
            self.assertEqual(retained[0].shape,(54,1,2))
            self.assertEqual(json.loads((root/'candidate.json').read_text())['images'],['000000000.jpg','000000002.jpg'])


if __name__=='__main__':unittest.main()
