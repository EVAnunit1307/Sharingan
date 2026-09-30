"""Core ML process: cache raw relative predictions; never imports COLMAP."""
import argparse
import hashlib
import json
from pathlib import Path
import time

import numpy as np
from PIL import Image


def run(session, package, output):
    import coremltools as ct
    output.mkdir(parents=True, exist_ok=True)
    rows = [json.loads(line) for line in (session/'frames.jsonl').read_text().splitlines() if line.strip()]
    started = time.perf_counter()
    model = ct.models.MLModel(str(package), compute_units=ct.ComputeUnit.ALL)
    fingerprint=hashlib.sha256()
    for file in sorted(package.rglob('*')):
        if file.is_file():
            fingerprint.update(str(file.relative_to(package)).encode());fingerprint.update(file.read_bytes())
    model_hash=fingerprint.hexdigest()
    input_spec = model.get_spec().description.input[0]
    size = (input_spec.type.imageType.width, input_spec.type.imageType.height)
    loaded = time.perf_counter()
    timings, entries = [], []
    for index, row in enumerate(rows):
        source = session/row['image']
        digest = hashlib.sha256(source.read_bytes()).hexdigest()
        target = output/(source.stem+'.npz')
        if target.exists():
            try:
                with np.load(target, allow_pickle=False) as cache:
                    if str(cache['image_sha256']) == digest and str(cache['model_sha256'])==model_hash:
                        entries.append(dict(image=source.name, cache=target.name, cached=True))
                        continue
            except (ValueError,OSError,KeyError):pass  # Interrupted or older cache; rebuild.
        frame = Image.open(source).convert('RGB').resize(size)
        before = time.perf_counter()
        depth = np.asarray(model.predict({input_spec.name: frame})['depth'], dtype=np.float32).squeeze()
        elapsed = (time.perf_counter()-before)*1000
        if depth.ndim != 2 or not np.isfinite(depth).all():
            raise ValueError('Depth model returned an invalid prediction')
        temporary=target.with_suffix('.tmp')
        with temporary.open('wb') as out:
            np.savez_compressed(out, prediction=depth, image_sha256=digest,model_sha256=model_hash,
                                model='apple/coreml-depth-anything-v2-small/F16')
        temporary.replace(target)
        timings.append(elapsed)
        entries.append(dict(image=source.name, cache=target.name, cached=False))
        if index % 20 == 0:
            print(f'Depth prediction {index+1}/{len(rows)}', flush=True)
    report = dict(model='apple/coreml-depth-anything-v2-small/F16',model_sha256=model_hash,input_size=size,
                  model_load_seconds=loaded-started, total_seconds=time.perf_counter()-started,
                  newly_predicted=len(timings), frames=len(rows), entries=entries,
                  median_inference_ms=float(np.median(timings)) if timings else None,
                  p95_inference_ms=float(np.percentile(timings,95)) if timings else None,
                  meaning='Relative inverse-depth-like prediction, not temperature or measured distance')
    (output/'inference.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps({k:v for k,v in report.items() if k!='entries'}), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('session', 'package', 'output'):
        parser.add_argument('--'+name, required=True, type=Path)
    args = parser.parse_args()
    run(args.session, args.package, args.output)
