"""Reproducible laptop inference smoke test; not a physical accuracy benchmark."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import sys
import time
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
import cv2
from GroundStation.pose import PoseEstimator,extract_poses
from GroundStation.fusion import FusionConfig,FusionEngine

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image',type=Path,required=True)
    p.add_argument('--models',type=Path,default=ROOT/'Saved/PersonPose/models')
    p.add_argument('--out',type=Path,default=ROOT/'Saved/PersonPose/benchmark')
    args=p.parse_args();args.out.mkdir(parents=True,exist_ok=True)
    source=cv2.imread(str(args.image));assert source is not None
    source=cv2.resize(source,(640,round(source.shape[0]*640/source.shape[1])))
    image=cv2.cvtColor(source,cv2.COLOR_BGR2RGB);h,w=image.shape[:2]
    results=[]
    for name in ('lite','full'):
        model=args.models/f'pose_landmarker_{name}.task'
        estimator=PoseEstimator(model)
        times=[]
        for i in range(16):
            start=time.perf_counter();result=estimator.infer(image,1000+i*100)
            times.append((time.perf_counter()-start)*1000)
        estimator.close()
        assert result.pose_landmarks,'Public sample must yield a pose'
        frame=dict(source_session_id='pose-benchmark',generation=1,frame_id=16,capture_ms=2500,
            age_ms=0,width=w,height=h,hfov=62,people=[dict(id=1,observed=True,box=[1,1,w-1,h-1])])
        poses=extract_poses(result,frame,FusionConfig())
        assert len(poses)==1 and len(poses[0]['joints'])==33
        row=dict(model=name,sha256=hashlib.sha256(model.read_bytes()).hexdigest(),
            cold_ms=times[0],median_ms=statistics.median(times[1:]),p95_ms=sorted(times[1:])[-1],
            poses=len(poses),qualified_joints=sum(j[3]>=.5 for j in poses[0]['joints']))
        results.append(row)
        (args.out/f'{name}-pose.json').write_text(json.dumps(dict(frame=frame,poses=poses),indent=2))
    report=dict(input=str(args.image),input_sha256=hashlib.sha256(args.image.read_bytes()).hexdigest(),
        physical_accuracy_tested=False,results=results)
    (args.out/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))
if __name__=='__main__':main()
