"""Export the pinned XFeat network for an ONNX-only edge benchmark."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

import cv2
import numpy as np


def run(repo,image,output):
    import torch
    import onnxruntime as ort
    torch.set_num_threads(4)
    sys.path.insert(0,str(repo.resolve()))
    from modules.xfeat import XFeat
    weights=torch.load(repo/'weights/xfeat.pt',map_location='cpu',weights_only=True)
    model=XFeat(weights=weights,top_k=1024).net.eval()
    rgb=cv2.cvtColor(cv2.imread(str(image)),cv2.COLOR_BGR2RGB).astype(np.float32)/255.
    tensor=torch.from_numpy(rgb.transpose(2,0,1)[None].copy())
    output.parent.mkdir(parents=True,exist_ok=True)
    torch.onnx.export(model,tensor,str(output),input_names=['image'],
                      output_names=['features','keypoint_logits','reliability'],
                      opset_version=17,dynamo=False)
    opts=ort.SessionOptions();opts.intra_op_num_threads=4;opts.inter_op_num_threads=1
    session=ort.InferenceSession(str(output),sess_options=opts,providers=['CPUExecutionProvider'])
    with torch.inference_mode():reference=[x.numpy() for x in model(tensor)]
    actual=session.run(None,{'image':tensor.numpy()})
    errors={name:dict(max_abs=float(np.max(np.abs(a-b))),mean_abs=float(np.mean(np.abs(a-b))))
            for name,a,b in zip(['features','keypoint_logits','reliability'],reference,actual)}
    report=dict(source='https://github.com/verlab/accelerated_features',
                upstream_commit='e92685f57f8318b18725c5c8c0bd28c7fe188d9a',
                model_sha256=hashlib.sha256(output.read_bytes()).hexdigest(),bytes=output.stat().st_size,
                input_shape=list(tensor.shape),opset=17,equivalence=errors,
                note='Network tensor parity only; edge keypoint/descriptor sampling is evaluated separately')
    output.with_suffix('.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('repo','image','output'):p.add_argument('--'+name,required=True,type=Path)
    a=p.parse_args();run(a.repo,a.image,a.output)
