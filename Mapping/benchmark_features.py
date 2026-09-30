"""Offline CPU feature/matching comparison on a saved real walk; no map promotion.

Run in the separate research venv with --xfeat-repo pointing at the upstream
verlab/accelerated_features checkout. Pairwise inliers are not 3D accuracy.
"""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import subprocess
import sys
import time

import cv2
import numpy as np

from Mapping.keyframes import sharpest_windows


def run(session, repo, output):
    import torch
    torch.set_num_threads(4)
    cv2.setNumThreads(1)
    sys.path.insert(0, str(repo.resolve()))
    from modules.xfeat import XFeat
    weights_path = repo/'weights/xfeat.pt'
    weights = torch.load(weights_path, map_location='cpu', weights_only=True)
    xfeat = XFeat(weights=weights, top_k=1024)
    rows = [json.loads(line) for line in (session/'frames.jsonl').read_text().splitlines()]
    indices = sorted(set(np.linspace(0, len(rows)-2, min(24, len(rows)-1)).astype(int).tolist()))
    pairs = [(i, i+1) for i in indices]
    pairs += [(i, min(i+6,len(rows)-1)) for i in indices[::3]]
    needed = sorted({i for pair in pairs for i in pair})
    images = {i:cv2.imread(str(session/rows[i]['image'])) for i in needed}
    sift = cv2.SIFT_create(nfeatures=1024)
    orb = cv2.ORB_create(nfeatures=1024)
    results = {}
    for name in ['SIFT', 'ORB', 'XFeat']:
        def extract(image):
            if name == 'XFeat':
                data = xfeat.detectAndCompute(cv2.cvtColor(image,cv2.COLOR_BGR2RGB).astype(np.float32)/255)[0]
                return data['keypoints'].numpy(), data['descriptors'].numpy()
            detector = sift if name == 'SIFT' else orb
            keypoints, descriptors = detector.detectAndCompute(cv2.cvtColor(image,cv2.COLOR_BGR2GRAY),None)
            return np.array([p.pt for p in keypoints],dtype=np.float32), descriptors
        for _ in range(3): extract(images[needed[0]])
        features, timings = {}, []
        for i in needed:
            started = time.perf_counter(); features[i] = extract(images[i])
            timings.append((time.perf_counter()-started)*1000)
        pair_results=[]
        for i,j in pairs:
            p0,d0=features[i];p1,d1=features[j]
            started=time.perf_counter()
            if d0 is None or d1 is None or len(d0)<8 or len(d1)<8:
                matched=[]
            elif name == 'XFeat':
                a,b=xfeat.match(torch.from_numpy(d0),torch.from_numpy(d1),min_cossim=.82)
                matched=list(zip(a.tolist(),b.tolist()))
            else:
                norm=cv2.NORM_L2 if name=='SIFT' else cv2.NORM_HAMMING
                bf=cv2.BFMatcher(norm)
                forward=bf.knnMatch(d0,d1,k=2);reverse=bf.match(d1,d0)
                back={m.queryIdx:m.trainIdx for m in reverse}
                matched=[(m.queryIdx,m.trainIdx) for pair in forward if len(pair)==2
                         for m,n in [pair] if m.distance<.8*n.distance and back.get(m.trainIdx)==m.queryIdx]
            matching_ms=(time.perf_counter()-started)*1000
            inliers=0
            if len(matched)>=8:
                a,b=np.asarray(matched).T
                cv2.setRNGSeed(0)
                _,mask=cv2.findFundamentalMat(p0[a],p1[b],cv2.USAC_MAGSAC,1.5,.999,10000)
                inliers=int(mask.sum()) if mask is not None else 0
            pair_results.append(dict(indices=[i,j],matches=len(matched),geometric_inliers=inliers,matching_ms=matching_ms))
        results[name]={'median_extraction_ms':statistics.median(timings),
                       'median_matching_ms':statistics.median(p['matching_ms'] for p in pair_results),
                       'median_inliers':statistics.median(p['geometric_inliers'] for p in pair_results),
                       'pairs_with_at_least_30_inliers':sum(p['geometric_inliers']>=30 for p in pair_results),
                       'pairs_tested':len(pair_results),'pairs':pair_results}
        print(name, {k:v for k,v in results[name].items() if k!='pairs'},flush=True)
    selected=sharpest_windows(rows)
    report={'session_id':session.name,'hardware':'Apple M5, 24 GB RAM',
            'torch_version':torch.__version__,'device':'cpu','torch_threads':4,'opencv_threads':1,'max_keypoints':1024,
            'xfeat_commit':subprocess.check_output(['git','-C',str(repo),'rev-parse','HEAD'],text=True).strip(),
            'weights_sha256':hashlib.sha256(weights_path.read_bytes()).hexdigest(),
            'caveat':'CPU pair benchmark, not full SLAM or validated 3D accuracy; extractor and matcher time exclude geometric verification.',
            'methods':results,'keyframes':{'input_frames':len(rows),'selected_frames':len(selected),
                'window_seconds':2/3,'selected_indices':selected,
                'median_input_sharpness':statistics.median(r['sharpness'] for r in rows),
                'median_selected_sharpness':statistics.median(rows[i]['sharpness'] for i in selected)}}
    output.parent.mkdir(parents=True,exist_ok=True)
    output.write_text(json.dumps(report,indent=2)+'\n')
    print('Saved',output,flush=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--session',type=Path,required=True)
    parser.add_argument('--xfeat-repo',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    run(args.session,args.xfeat_repo,args.output)
