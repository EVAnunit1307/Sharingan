"""Calibrate the actual processed camera geometry from a checkerboard recording.

Board has 9 x 6 inner corners (10 x 7 squares). Square size can be one arbitrary
unit for lens intrinsics; this does not establish metric scale in the room map.
Writes a candidate file only; does not silently replace active map calibration.
"""
import argparse
import json
from pathlib import Path

import cv2
import numpy as np


def fit_views(corners,size):
    if len(corners)<15:raise ValueError('Need at least 15 clear board views at varied positions and tilts')
    # OpenCV versions expose either (N, 2) or (N, 1, 2) corner arrays.
    corners=[np.asarray(c,dtype=np.float32).reshape(54,1,2) for c in corners]
    obj=np.zeros((9*6,3),np.float32);obj[:,:2]=np.mgrid[0:9,0:6].T.reshape(-1,2)
    test=[i for i in range(len(corners)) if i%4==0];train=[i for i in range(len(corners)) if i not in test]
    rms,K,dist,rvecs,tvecs=cv2.calibrateCamera([obj]*len(train),[corners[i] for i in train],size,None,None,flags=cv2.CALIB_FIX_K3)
    held=[]
    for i in test:
        ok,r,t=cv2.solvePnP(obj,corners[i],K,dist)
        if not ok:continue
        projected,_=cv2.projectPoints(obj,r,t,K,dist)
        held.extend(np.linalg.norm(projected[:,0]-corners[i][:,0],axis=1).tolist())
    all_xy=np.concatenate([c[:,0] for c in corners]);span=np.ptp(all_xy,axis=0)/size
    accepted=bool(rms<1.5 and held and np.median(held)<1.5 and min(span)>.5 and
                  .1*size[0]<K[0,0]<5*size[0] and .1*size[0]<K[1,1]<5*size[0])
    return dict(camera_model='OPENCV',pixel_coordinates='opencv_integer_pixel_centers',width=size[0],height=size[1],matrix=K.tolist(),distortion=dist.ravel().tolist(),
                params=[float(K[0,0]),float(K[1,1]),float(K[0,2]),float(K[1,2]),*dist.ravel()[:4].tolist()],
                training_rms_px=float(rms),held_out_median_px=float(np.median(held)) if held else None,
                held_out_p90_px=float(np.percentile(held,90)) if held else None,
                board_image_span_fraction=span.tolist(),training_views=len(train),held_out_views=len(test),
                passes_basic_checks=accepted,status='candidate_not_applied',
                note='Lens calibration candidate; planar-board held-out reprojection only. No measured room-map scale or physical accuracy.')


def run(session,output):
    manifest=json.loads((session/'manifest.json').read_text());corners=[];names=[];size=None
    for file in sorted((session/'images').glob('*.jpg')):
        image=cv2.imread(str(file),cv2.IMREAD_GRAYSCALE)
        if image is None:continue
        current=(image.shape[1],image.shape[0])
        if size and current!=size:raise ValueError('Mixed image dimensions in calibration recording')
        size=current
        found,points=cv2.findChessboardCornersSB(image,(9,6),flags=cv2.CALIB_CB_NORMALIZE_IMAGE)
        if not found:continue
        points=np.asarray(points,dtype=np.float32).reshape(54,1,2)
        # Avoid treating repeated near-identical frames as distinct calibration poses.
        if corners and min(float(np.mean(np.linalg.norm(points-c,axis=2))) for c in corners)<12:continue
        corners.append(points);names.append(file.name)
    report=fit_views(corners,size)
    report.update(source_session=session.name,images=names,processed_geometry=manifest.get('camera'))
    output.parent.mkdir(parents=True,exist_ok=True);output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--session',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True);args=parser.parse_args();run(args.session,args.output)
