"""Local semantic labels for saved DA3 images. No geometry or sensor fusion here."""
import argparse
import hashlib
import json
from pathlib import Path
import time

import numpy as np


def run(model_path, trials, cache):
    import torch
    import torch.nn.functional as F
    from transformers import SegformerImageProcessor, SegformerForSemanticSegmentation
    if not torch.backends.mps.is_available():raise RuntimeError('This experiment requires macOS GPU access')
    cache.mkdir(parents=True,exist_ok=True)
    source=json.loads((model_path/'source.json').read_text())
    model_hash=source['files']['model.safetensors']['sha256']
    processor=SegformerImageProcessor.from_pretrained(str(model_path),local_files_only=True)
    model=SegformerForSemanticSegmentation.from_pretrained(str(model_path),local_files_only=True).eval().to('mps')
    for trial in trials:
        output=trial/'semantics.npz'
        if output.exists():raise ValueError(f'Semantic output already exists: {trial}')
        images=np.load(trial/'prediction.npz',allow_pickle=False)['images']
        labels=[];scores=[];timings=[];hits=0;keys=[]
        for image in images:
            key=hashlib.sha256(image.tobytes()+model_hash.encode()).hexdigest();keys.append(key)
            entry=cache/(key+'.npz')
            if entry.exists():
                data=np.load(entry,allow_pickle=False);lab=data['labels'];score=data['scores'];hits+=1
            else:
                inputs=processor(images=image,return_tensors='pt').to('mps')
                torch.mps.synchronize();started=time.perf_counter()
                with torch.inference_mode():
                    logits=model(**inputs).logits
                    logits=F.interpolate(logits,size=image.shape[:2],mode='bilinear',align_corners=False)
                    score,lab=logits.softmax(1).max(1)
                torch.mps.synchronize();timings.append(time.perf_counter()-started)
                lab=lab[0].cpu().numpy().astype(np.uint8);score=score[0].cpu().numpy().astype(np.float16)
                np.savez_compressed(entry,labels=lab,scores=score)
            labels.append(lab);scores.append(score)
        np.savez_compressed(output,labels=np.array(labels),scores=np.array(scores))
        report=dict(model=source,device='mps',id2label=model.config.id2label,
                    image_cache_keys=keys,cache_hits=hits,new_inference_seconds=timings,
                    score_note='Model softmax score; not calibrated probability of correctness.',
                    labels_are_inferred=True)
        (trial/'semantics.json').write_text(json.dumps(report,indent=2)+'\n')
        print(trial.name,'labels complete',len(images),'images',hits,'cached',flush=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--model',type=Path,required=True)
    parser.add_argument('--trials',type=Path,nargs='+',required=True)
    parser.add_argument('--cache',type=Path,required=True)
    args=parser.parse_args();run(args.model,args.trials,args.cache)
