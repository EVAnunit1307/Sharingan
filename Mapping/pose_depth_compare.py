"""Paired image/depth consistency on identical processed pixels, not ground truth."""
import argparse
import hashlib
import json
from pathlib import Path

import cv2
import numpy as np


def sample(depth, pixels):
    return cv2.remap(depth.astype(np.float32), pixels[:, 0].astype(np.float32)[None],
                     pixels[:, 1].astype(np.float32)[None], cv2.INTER_LINEAR)[0]


def errors(data, i, j, source, target):
    ex, k = data['extrinsics'], data['intrinsics']
    rays = np.c_[source, np.ones(len(source))] @ np.linalg.inv(k[i]).T
    camera = rays * sample(data['depth'][i], source)[:, None]
    world = (camera - ex[i, :, 3]) @ ex[i, :, :3]
    other = world @ ex[j, :, :3].T + ex[j, :, 3]
    projected = other @ k[j].T
    valid = (other[:, 2] > 0) & np.isfinite(projected).all(axis=1)
    projected = projected[:, :2] / np.where(valid, projected[:, 2], 1)[:, None]
    target_depth = sample(data['depth'][j], target)
    depth_error = 2 * np.abs(other[:, 2]-target_depth) / np.maximum(other[:, 2]+target_depth, 1e-8)
    return np.linalg.norm(projected-target, axis=1), depth_error, valid


def compare(trials):
    if len(trials) < 2:
        raise ValueError('Compare at least two trials')
    reports = [json.loads((p/'summary.json').read_text()) for p in trials]
    if any(r.get('state') != 'complete' for r in reports):
        raise ValueError('Comparison requires completed trials')
    arrays = [dict(np.load(p/'prediction.npz', allow_pickle=False)) for p in trials]
    images = arrays[0]['images']
    if len(images) < 6:
        raise ValueError('Fixed revisit comparison requires at least six views')
    if any(not np.array_equal(images, d['images']) for d in arrays[1:]):
        raise ValueError('Fair comparison requires identical processed image pixels')
    if any(r['session_id'] != reports[0]['session_id'] or r['indices'] != reports[0]['indices'] for r in reports):
        raise ValueError('Trial source selection differs')
    sift = cv2.SIFT_create(nfeatures=1000)
    features = [sift.detectAndCompute(cv2.cvtColor(im, cv2.COLOR_RGB2GRAY), None) for im in images]
    matcher = cv2.BFMatcher()
    checks = [[] for _ in trials]
    # Adjacent views plus fixed start/end revisits. Revisit slots are selected
    # before looking at errors, and are kept separate from adjacent summaries.
    pairs = [(i,i+1,'adjacent') for i in range(len(images)-1)]
    pairs += [(0,len(images)-1,'revisit'), (1,len(images)-2,'revisit'), (2,len(images)-3,'revisit')]
    for i,j,kind in pairs:
        ka,da=features[i]; kb,db=features[j]
        if da is None or db is None:
            matches=[]
        else:
            reverse={m[0].queryIdx:m[0].trainIdx for m in matcher.knnMatch(db,da,k=2)
                     if len(m)==2 and m[0].distance<.75*m[1].distance}
            matches=[m[0] for m in matcher.knnMatch(da,db,k=2)
                     if len(m)==2 and m[0].distance<.75*m[1].distance and reverse.get(m[0].trainIdx)==m[0].queryIdx]
        if len(matches) < 8:
            for check in checks: check.append(dict(pair=[i,j],kind=kind,matches=len(matches),evaluated=0,reason='fewer than 8 mutual ratio matches'))
            continue
        pa=np.array([ka[m.queryIdx].pt for m in matches]);pb=np.array([kb[m.trainIdx].pt for m in matches])
        values=[errors(d,i,j,pa,pb) for d in arrays]
        common=np.logical_and.reduce([v[2] for v in values])
        for check,(pixels,depth,valid) in zip(checks,values):
            check.append(dict(pair=[i,j],kind=kind,matches=len(matches),evaluated=int(common.sum()),
                invalid_projections=int((~valid).sum()),
                median_pixels=float(np.median(pixels[common])) if common.any() else None,
                p90_pixels=float(np.percentile(pixels[common],90)) if common.any() else None,
                median_symmetric_depth_difference=float(np.median(depth[common])) if common.any() else None))
    results=[]
    for path,report,check in zip(trials,reports,checks):
        group={}
        for kind in ('adjacent','revisit'):
            kept=[p for p in check if p['kind']==kind and p['evaluated']>=8]
            group[kind]=dict(supported_pairs=len(kept),
                median_pair_pixels=float(np.median([p['median_pixels'] for p in kept])) if kept else None,
                median_pair_depth_difference=float(np.median([p['median_symmetric_depth_difference'] for p in kept])) if kept else None)
        results.append(dict(trial=path.name,model=report['model'],pose_conditioned=report['pose_conditioned'],
            inference_seconds=report['inference_samples_seconds'],memory=report['memory'],groups=group,pairs=check))
    return dict(method='Identical mutual SIFT ratio .75 matches, no confidence filtering. Evaluate identical positive-depth correspondences across models; report invalid counts. Bilinear depth sampling.',
        units='pixels at processed resolution / symmetric relative depth difference',
        processed_rgb_sha256=hashlib.sha256(images.tobytes()).hexdigest(),
        note='Same-input consistency only, not ground truth. Supplied ORB poses impose camera agreement. Revisit pairs are fixed image pairs, not exact physical returns. Repeated textures, occlusion and moving objects can affect matches.',results=results)


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--trials',nargs='+',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    if args.output.exists(): raise ValueError('Choose a new comparison output')
    result=compare(args.trials)
    args.output.write_text(json.dumps(result,indent=2,allow_nan=False)+'\n')
    print(json.dumps([dict(trial=r['trial'],groups=r['groups'],seconds=r['inference_seconds']) for r in result['results']],indent=2))
