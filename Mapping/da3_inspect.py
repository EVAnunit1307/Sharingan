"""Export DA3 trials to a local comparison, with diagnostic (not accuracy) checks."""
import argparse
import base64
import json
from pathlib import Path

import cv2
import numpy as np


def centers(extrinsics):
    return -np.einsum('nji,nj->ni', extrinsics[:,:,:3], extrinsics[:,:,3])


def similarity(source, target):
    a, b = source-source.mean(0), target-target.mean(0)
    u, singular, vt = np.linalg.svd(b.T @ a / len(a))
    sign = np.ones(3); sign[-1] = np.linalg.det(u @ vt)
    rotation = (u * sign) @ vt
    scale = (singular * sign).sum() / np.mean(np.sum(a*a, axis=1))
    translation = target.mean(0) - scale * rotation @ source.mean(0)
    return scale, rotation, translation


def compare_poses(a, b):
    common = sorted(set(a['report']['indices']) & set(b['report']['indices']))
    if len(common)<3:raise ValueError('At least three shared views are needed for pose comparison')
    ia = [a['report']['indices'].index(i) for i in common]
    ib = [b['report']['indices'].index(i) for i in common]
    ca, cb = centers(a['arrays']['extrinsics'])[ia], centers(b['arrays']['extrinsics'])[ib]
    if min(np.linalg.norm(ca-ca.mean(0)),np.linalg.norm(cb-cb.mean(0)))<1e-8:
        raise ValueError('Stationary camera centres cannot establish a similarity scale')
    scale, rot, trans = similarity(cb, ca)
    residuals = np.linalg.norm(scale * cb @ rot.T + trans - ca, axis=1)
    spread = np.sqrt(np.mean(np.sum((ca-ca.mean(0))**2,axis=1)))
    ra = a['arrays']['extrinsics'][ia,:,:3].transpose(0,2,1)
    rb = rot @ b['arrays']['extrinsics'][ib,:,:3].transpose(0,2,1)
    angle = np.degrees(np.arccos(np.clip((np.trace(ra.transpose(0,2,1)@rb,axis1=1,axis2=2)-1)/2,-1,1)))
    depth_a, depth_b = a['arrays']['depth'][ia], b['arrays']['depth'][ib]*scale
    return dict(common_views=len(common), alignment='similarity fit to all shared camera centres',
                path_rmse_fraction_of_spread=float(np.sqrt(np.mean(residuals**2))/spread),
                orientation_median_degrees=float(np.median(angle)),orientation_p90_degrees=float(np.percentile(angle,90)),
                aligned_depth_relative_change_median=float(np.median(np.abs(depth_a-depth_b)/depth_a)),
                note='Sensitivity to changed input views, not physical accuracy or independent ground truth.')


def diagnostics(data):
    depth, conf, ex, intr, images = [data[k] for k in ('depth','confidence','extrinsics','intrinsics','images')]
    sift=cv2.SIFT_create(nfeatures=800)
    features=[sift.detectAndCompute(cv2.cvtColor(im,cv2.COLOR_RGB2GRAY),None) for im in images]
    matcher=cv2.BFMatcher()
    results=[]
    for i in range(len(images)-1):
        j=i+1; ka, da=features[i]; kb, db=features[j]
        if da is None or db is None: continue
        reverse={m[0].queryIdx:m[0].trainIdx for m in matcher.knnMatch(db,da,k=2)
                 if len(m)==2 and m[0].distance < .75*m[1].distance}
        matches=[m[0] for m in matcher.knnMatch(da,db,k=2)
                 if len(m)==2 and m[0].distance < .75*m[1].distance and reverse.get(m[0].trainIdx)==m[0].queryIdx]
        errors=[]
        for match in matches:
            u,v=ka[match.queryIdx].pt; x,y=int(round(u)),int(round(v))
            if not (0<=y<depth.shape[1] and 0<=x<depth.shape[2]):continue
            if conf[i,y,x] < np.percentile(conf[i],40):continue
            pc=(np.linalg.inv(intr[i])@np.array([u,v,1.]))*depth[i,y,x]
            world=ex[i,:,:3].T@(pc-ex[i,:,3])
            q=ex[j,:,:3]@world+ex[j,:,3]
            if q[2]<=0:continue
            pixel=intr[j]@q; pixel=pixel[:2]/pixel[2]
            errors.append(float(np.linalg.norm(pixel-np.array(kb[match.trainIdx].pt))))
        results.append(dict(pair=[i,j],mutual_ratio_matches=len(matches),evaluated=len(errors),
                            median_pixels=float(np.median(errors)) if errors else None,
                            p90_pixels=float(np.percentile(errors,90)) if errors else None))
    medians=[r['median_pixels'] for r in results if r['evaluated']>=8]
    return dict(method='Mutual SIFT ratio matches, source depth confidence above per-view 40th percentile; predicted poses project source pixels into the next view.',
                image_width=int(depth.shape[2]),pairs=results,pairs_with_at_least_8_matches=len(medians),
                median_of_pair_medians_pixels=float(np.median(medians)) if medians else None,
                note='Image correspondence consistency only. Repeated textures, moving objects, calibration and occlusion can affect this diagnostic.')


def jpeg_data(image):
    ok, encoded=cv2.imencode('.jpg',image[:,:,::-1],[cv2.IMWRITE_JPEG_QUALITY,85])
    if not ok: raise ValueError('JPEG encoding failed')
    return 'data:image/jpeg;base64,'+base64.b64encode(encoded).decode()


def export_trial(path):
    report=json.loads((path/'summary.json').read_text())
    if report['state']!='complete':raise ValueError('Trial did not complete')
    arrays=dict(np.load(path/'prediction.npz',allow_pickle=False))
    depth, conf, ex, intr, images = [arrays[k] for k in ('depth','confidence','extrinsics','intrinsics','images')]
    points=[];threshold=np.percentile(conf,40); far=np.percentile(depth,99)
    h,w=depth.shape[1:];ys,xs=np.mgrid[0:h:3,0:w:3]
    pixels=np.stack([xs,ys,np.ones_like(xs)],axis=-1).reshape(-1,3)
    for i in range(len(depth)):
        d=depth[i,ys,xs].ravel(); keep=(conf[i,ys,xs].ravel()>=threshold)&(d<=far)
        cam=(pixels@np.linalg.inv(intr[i]).T)*d[:,None]
        world=(cam-ex[i,:,3])@ex[i,:,:3]
        points.append(np.column_stack([world[keep],images[i,ys,xs].reshape(-1,3)[keep],np.full(keep.sum(),i)]))
    points=np.concatenate(points)
    if len(points)>45000:points=points[np.linspace(0,len(points)-1,45000,dtype=int)]
    point_rows=[[round(float(v),5) for v in row[:3]]+[int(v) for v in row[3:]] for row in points]
    with (path/'inferred-points.ply').open('w') as f:
        f.write(f'ply\nformat ascii 1.0\nelement vertex {len(point_rows)}\nproperty float x\nproperty float y\nproperty float z\nproperty uchar red\nproperty uchar green\nproperty uchar blue\nend_header\n')
        for row in point_rows:f.write(' '.join(map(str,row[:6]))+'\n')
    lo,hi=np.percentile(depth,[2,98]); gray=np.uint8(255*(1-np.clip((depth-lo)/(hi-lo),0,1)))
    views=[jpeg_data(np.concatenate([im,np.repeat(d[:,:,None],3,axis=2)],axis=1)) for im,d in zip(images,gray)]
    check=diagnostics(arrays)
    (path/'diagnostics.json').write_text(json.dumps(check,indent=2)+'\n')
    origin=report['frames'][0]['sensor_timestamp_ns']
    visible=dict(name=path.name,points=point_rows,centers=centers(ex).round(5).tolist(),
                 forwards=ex[:,2,:3].round(5).tolist(),images=views,
                 indices=report['indices'],seconds=[(r['sensor_timestamp_ns']-origin)/1e9 for r in report['frames']],
                 inference=report['inference_seconds'],diagnostics=check,
                 filtering='Lowest 40% of predicted confidence and farthest 1% of depth omitted; display sampled to 45,000 points. These filters do not validate the geometry.')
    return dict(report=report,arrays=arrays,visible=visible)


def main(args):
    trials=[export_trial(p) for p in args.trials]
    comparison=compare_poses(trials[0],trials[1]) if len(trials)>1 else None
    args.output.parent.mkdir(parents=True,exist_ok=True)
    (args.output.parent/'comparison-metrics.json').write_text(json.dumps(comparison,indent=2)+'\n')
    template=(Path(__file__).parent/'static'/'da3-trial.html').read_text()
    payload=json.dumps(dict(trials=[t['visible'] for t in trials],comparison=comparison),separators=(',',':'),allow_nan=False).replace('<','\\u003c')
    args.output.write_text(template.replace('/*TRIAL_DATA*/null',payload))
    print(json.dumps({'output':str(args.output),'comparison':comparison,'reprojection':[t['visible']['diagnostics']['median_of_pair_medians_pixels'] for t in trials]},indent=2))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--trials',type=Path,nargs='+',required=True)
    parser.add_argument('--output',type=Path,required=True)
    main(parser.parse_args())
