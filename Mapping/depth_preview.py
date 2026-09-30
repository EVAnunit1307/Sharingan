"""Benchmark Apple's Core ML Depth Anything V2 Small on saved images.

Outputs model-inferred relative depth only: no metric scale, pose, map fusion,
occluded-surface recovery or obstacle-clearance claim.
"""
import argparse
import json
from pathlib import Path
import statistics
import time

import cv2
import numpy as np
from PIL import Image


def run(session,package,output):
    import coremltools as ct
    output.mkdir(parents=True,exist_ok=True)
    start=time.perf_counter()
    model=ct.models.MLModel(str(package),compute_units=ct.ComputeUnit.ALL)
    load_seconds=time.perf_counter()-start
    spec=model.get_spec();input_spec=spec.description.input[0]
    size=(input_spec.type.imageType.width,input_spec.type.imageType.height)
    rows=[json.loads(l) for l in (session/'frames.jsonl').read_text().splitlines()]
    indices=np.linspace(0,len(rows)-1,5).astype(int)
    timings=[];panels=[]
    for index in indices:
        row=rows[index]
        frame=Image.open(session/row['image']).convert('RGB').resize(size)
        start=time.perf_counter();result=model.predict({input_spec.name:frame})
        timings.append((time.perf_counter()-start)*1000)
        depth=np.asarray(result['depth'],dtype=np.float32)
        np.save(output/(Path(row['image']).stem+'-relative-depth.npy'),depth)
        low,high=np.percentile(depth,[2,98])
        # Grayscale avoids suggesting a thermal camera. Bright = predicted nearer.
        shade=(np.clip((depth-low)/max(high-low,1e-6),0,1)*255).astype(np.uint8)
        color=cv2.cvtColor(shade,cv2.COLOR_GRAY2BGR)
        rgb=cv2.cvtColor(np.asarray(frame),cv2.COLOR_RGB2BGR)
        panel=np.concatenate([rgb,color],axis=1)
        title=np.full((64,panel.shape[1],3),24,dtype=np.uint8)
        cv2.putText(title,'NORMAL CAMERA  |  AI DEPTH ESTIMATE - NOT THERMAL',
                    (8,25),cv2.FONT_HERSHEY_SIMPLEX,.55,(235,235,235),1,cv2.LINE_AA)
        cv2.putText(title,'Dark: farther   Bright: nearer   |   Relative per image; no metres or temperature',
                    (8,49),cv2.FONT_HERSHEY_SIMPLEX,.45,(190,190,190),1,cv2.LINE_AA)
        panels.append(np.concatenate([title,panel],axis=0))
    # Warm inference on one input, excluding model load and image I/O.
    for _ in range(15):
        start=time.perf_counter();model.predict({input_spec.name:frame})
        timings.append((time.perf_counter()-start)*1000)
    cv2.imwrite(str(output/'relative-depth-preview.jpg'),np.concatenate(panels,axis=0))
    cv2.imwrite(str(output/'relative-depth-example.jpg'),panels[0])
    report={'session_id':session.name,'model':'apple/coreml-depth-anything-v2-small / F16',
            'compute_units':'ALL; actual hardware allocation selected by Core ML',
            'input_size':size,'model_load_seconds':load_seconds,'first_prediction_ms':timings[0],
            'warm_median_prediction_ms':statistics.median(timings[1:]),
            'warm_p95_prediction_ms':sorted(timings[1:])[int(.95*len(timings[1:]))],
            'output':'relative depth, independently visualized per frame; no metric accuracy or temporal consistency measured',
            'not_a_persistent_map':True,'input_frames':[rows[i]['image'] for i in indices]}
    (output/'summary.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('session','package','output'):parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args();run(args.session,args.package,args.output)
