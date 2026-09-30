"""Read-only capture-quality / feature benchmark on saved frames (Pi or laptop)."""
import argparse
import json
from pathlib import Path
import platform
import subprocess
import time

import cv2
import numpy as np


def telemetry():
    out={}
    for key,command in [('temperature',['vcgencmd','measure_temp']),('throttled',['vcgencmd','get_throttled'])]:
        try:out[key]=subprocess.check_output(command,text=True,timeout=2).strip()
        except (FileNotFoundError,subprocess.SubprocessError):pass
    return out


def run(session,output,model=None,threads=2,cycles=2):
    cv2.setNumThreads(1)
    rows=[json.loads(s) for s in (session/'frames.jsonl').read_text().splitlines() if s.strip()]
    indices=np.linspace(0,len(rows)-2,min(24,len(rows)-1)).astype(int)
    needed=sorted(set(indices.tolist()+[i+1 for i in indices]))
    images={i:cv2.imread(str(session/rows[i]['image'])) for i in needed}
    methods={'ORB':cv2.ORB_create(nfeatures=1024),'SIFT':cv2.SIFT_create(nfeatures=1024)}
    edge=None
    if model:
        from Mapping.edge_features import EdgeFeatures,mutual_matches
        edge=EdgeFeatures(model,threads=threads)
        methods['XFeat_ONNX']=edge
    report=dict(machine=platform.machine(),system=platform.platform(),threads=threads,
                source_session=session.name,telemetry_start=telemetry(),methods={},cycles=cycles)
    start=time.perf_counter()
    for name,extractor in methods.items():
        times=[];features={}
        for _ in range(cycles):
            for index,frame in images.items():
                before=time.perf_counter()
                if name=='XFeat_ONNX':result=extractor.extract(frame)
                else:
                    k,d=extractor.detectAndCompute(cv2.cvtColor(frame,cv2.COLOR_BGR2GRAY),None)
                    result=dict(keypoints=np.array([point.pt for point in k]),descriptors=d)
                times.append((time.perf_counter()-before)*1000);features[index]=result
        pairs=[]
        for i in indices:
            a,b=features[i],features[i+1];before=time.perf_counter()
            if name=='XFeat_ONNX':matches=mutual_matches(a,b)
            else:
                matches=[]
                if a['descriptors'] is not None and b['descriptors'] is not None:
                    matcher=cv2.BFMatcher(cv2.NORM_HAMMING if name=='ORB' else cv2.NORM_L2,crossCheck=True)
                    matches=np.array([[m.queryIdx,m.trainIdx] for m in matcher.match(a['descriptors'],b['descriptors'])],dtype=int)
            elapsed=(time.perf_counter()-before)*1000
            count=0
            if len(matches)>=8:
                _,mask=cv2.findFundamentalMat(a['keypoints'][matches[:,0]],b['keypoints'][matches[:,1]],cv2.USAC_MAGSAC,1.5,.999)
                count=int(mask.sum()) if mask is not None else 0
            pairs.append(dict(first=int(i),inliers=count,matches=len(matches),matching_ms=elapsed))
        report['methods'][name]=dict(median_extraction_ms=float(np.median(times)),p95_extraction_ms=float(np.percentile(times,95)),
                                   median_inliers=float(np.median([p['inliers'] for p in pairs])),
                                   pairs_with_30_inliers=sum(p['inliers']>=30 for p in pairs),pairs=pairs)
    quality=[];flow=[]
    for i in indices:
        frame=images[i];before=time.perf_counter();gray=cv2.cvtColor(cv2.resize(frame,(320,240)),cv2.COLOR_BGR2GRAY)
        score=float(cv2.Laplacian(gray,cv2.CV_32F).var());quality.append((time.perf_counter()-before)*1000)
        following=cv2.cvtColor(cv2.resize(images[i+1],(320,240)),cv2.COLOR_BGR2GRAY)
        before=time.perf_counter();corners=cv2.goodFeaturesToTrack(gray,200,.01,8)
        if corners is not None:cv2.calcOpticalFlowPyrLK(gray,following,corners,None,winSize=(21,21),maxLevel=3)
        flow.append((time.perf_counter()-before)*1000)
    report.update(quality_score_median_ms=float(np.median(quality)),optical_flow_median_ms=float(np.median(flow)),
                  telemetry_end=telemetry(),total_seconds=time.perf_counter()-start,
                  note='Saved-image benchmark, not sustained full-rate camera tracking, SLAM, or flight validation')
    output.parent.mkdir(parents=True,exist_ok=True);output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:v for k,v in report.items() if k!='methods'},indent=2))
    for name,data in report['methods'].items():print(name,{k:v for k,v in data.items() if k!='pairs'})


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--session',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--model',type=Path);p.add_argument('--threads',type=int,default=2);p.add_argument('--cycles',type=int,default=2)
    a=p.parse_args();run(a.session,a.output,a.model,a.threads,a.cycles)
