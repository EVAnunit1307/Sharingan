"""Bounded, offline DA3 experiment. Keeps inferred geometry separate from saved maps.

Uses the official network and input/output processors directly, avoiding imports
of unused CUDA Gaussian renderers and PyCOLMAP exporters on Apple Silicon.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time

import numpy as np


def select_indices(rows, start_frame, stride, frames=16, duration_seconds=None):
    if stride < 1 or not 0 <= start_frame < len(rows):
        raise ValueError('Use a valid start frame and positive stride')
    if duration_seconds is None:
        indices=list(range(start_frame,start_frame+frames*stride,stride))
    else:
        if not np.isfinite(duration_seconds) or not 0 < duration_seconds <= 30:
            raise ValueError('Duration must be greater than zero and at most 30 seconds')
        times=np.array([r['sensor_timestamp_ns'] for r in rows],dtype=np.int64)
        if not np.all(np.diff(times)>0):raise ValueError('Sensor timestamps must increase')
        cutoff=times[start_frame]+round(duration_seconds*1e9)
        indices=[i for i in range(start_frame,len(rows),stride) if times[i]<cutoff]
    if not 2<=len(indices)<=24 or indices[-1]>=len(rows):
        raise ValueError('Select 2–24 available images; increase stride for longer windows')
    if len({rows[i]['camera_generation'] for i in indices})!=1:
        raise ValueError('A trial cannot cross camera reconnects')
    return indices


def run(args):
    import torch
    from safetensors.torch import load_file
    sys.path.insert(0, str(args.source.resolve() / 'src'))
    from depth_anything_3.cfg import create_object, load_config
    from depth_anything_3.utils.io.input_processor import InputProcessor
    from depth_anything_3.utils.io.output_processor import OutputProcessor

    if args.output.exists():
        raise ValueError('Choose a new output directory; previous trials are immutable')
    rows = [json.loads(line) for line in (args.session / 'frames.jsonl').read_text().splitlines()]
    duration=getattr(args,'duration_seconds',None)
    indices=select_indices(rows,args.start_frame,args.stride,args.frames,duration)
    paths = [(args.session / rows[i]['image']).resolve() for i in indices]
    if any(not p.is_relative_to(args.session.resolve() / 'images') for p in paths):
        raise ValueError('Image path outside the recording')
    args.output.mkdir(parents=True)
    device = args.device
    if device == 'mps' and not torch.backends.mps.is_available():
        raise RuntimeError('Metal unavailable; run with macOS GPU access or explicitly select CPU')
    torch.manual_seed(42)
    torch.set_num_threads(4)
    source_commit = subprocess.check_output(['git', '-C', str(args.source), 'rev-parse', 'HEAD'], text=True).strip()
    report = dict(state='loading', model='depth-anything/DA3-SMALL', source_commit=source_commit,
                  device=device, torch=torch.__version__, session_id=args.session.name,
                  indices=indices, frames=[rows[i] for i in indices], resolution=args.resolution,
                  requested_window_seconds=duration,
                  selected_span_seconds=(rows[indices[-1]]['sensor_timestamp_ns']-rows[indices[0]]['sensor_timestamp_ns'])/1e9,
                  imu_used=False,
                  source_sha256={p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},
                  checkpoint_sha256=hashlib.sha256((args.model / 'model.safetensors').read_bytes()).hexdigest(),
                  pose_conditioned=False, scale='unknown', units='arbitrary', validated=False,
                  warning='AI-inferred geometry and camera poses. No measured scale, tracking validation or unseen-room coverage.')
    def save():
        (args.output / 'summary.json').write_text(json.dumps(report, indent=2, allow_nan=False) + '\n')
    save()
    try:
        started = time.perf_counter()
        net = create_object(load_config(str(args.source / 'src/depth_anything_3/configs/da3-small.yaml')))
        weights = load_file(str(args.model / 'model.safetensors'))
        # The released checkpoint is wrapped in the API's `model` member.
        if not all(k.startswith('model.') for k in weights):
            raise ValueError('Unexpected checkpoint key prefix')
        weights = {k.removeprefix('model.'): v for k, v in weights.items()}
        # Safetensors stores tied parameters once. Restore only aliases proven
        # to share the very same Parameter, then still require a strict load.
        names = dict(net.named_parameters(remove_duplicate=False))
        stored_names = set(weights)
        restored = {}
        for key in net.state_dict():
            if key in weights: continue
            aliases = [name for name, value in names.items()
                       if key in names and value is names[key] and name in stored_names]
            if len(aliases) != 1:
                raise ValueError(f'Missing checkpoint parameter without a unique tied alias: {key}')
            weights[key] = weights[aliases[0]]
            restored[key] = aliases[0]
        net.load_state_dict(weights, strict=True)
        report['restored_tied_parameters'] = restored
        del weights
        net.eval().to(device)
        report['model_load_seconds'] = time.perf_counter() - started
        imgs, _, _ = InputProcessor()([str(p) for p in paths], process_res=args.resolution,
                                     process_res_method='upper_bound_resize', sequential=True)
        pixels = imgs.permute(0, 2, 3, 1).numpy()
        pixels = np.rint(np.clip(pixels * [0.229, 0.224, 0.225] + [0.485, 0.456, 0.406], 0, 1) * 255).astype(np.uint8)
        batch = imgs[None].to(device)
        report.update(state='inference', input_shape=list(batch.shape))
        save()
        if device == 'mps': torch.mps.synchronize()
        started = time.perf_counter()
        with torch.inference_mode(), torch.autocast(device_type=device, dtype=torch.float16, enabled=device == 'mps'):
            raw = net(batch, use_ray_pose=False, ref_view_strategy='saddle_balanced')
        if device == 'mps': torch.mps.synchronize()
        report['inference_seconds'] = time.perf_counter() - started
        pred = OutputProcessor()(raw)
        arrays = dict(depth=pred.depth, confidence=pred.conf, extrinsics=pred.extrinsics,
                      intrinsics=pred.intrinsics, images=pixels)
        if any(v is None or not np.isfinite(v).all() for v in arrays.values()):
            raise ValueError('Missing or non-finite model output')
        if (pred.depth <= 0).any(): raise ValueError('Nonpositive predicted depth')
        np.savez_compressed(args.output / 'prediction.npz', **arrays)
        report.update(state='complete', shapes={k:list(v.shape) for k,v in arrays.items()},
                      predicted_depth_percentiles=np.percentile(pred.depth,[1,50,99]).tolist(),
                      confidence_percentiles=np.percentile(pred.conf,[10,50,90]).tolist())
        if device == 'mps':
            report['mps_driver_allocated_bytes_after_inference'] = torch.mps.driver_allocated_memory()
        save()
        print(json.dumps({k:v for k,v in report.items() if k not in ('frames','source_sha256')}, indent=2))
    except Exception as exc:
        report.update(state='failed', error=f'{type(exc).__name__}: {exc}')
        save()
        raise


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('source','model','session','output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--start-frame', type=int, default=48)
    parser.add_argument('--stride', type=int, default=4)
    parser.add_argument('--frames', type=int, default=16)
    parser.add_argument('--duration-seconds', type=float, help='Select a timestamp-bounded prefix instead of --frames')
    parser.add_argument('--resolution', type=int, choices=(280,392,504), default=392)
    parser.add_argument('--device', choices=('mps','cpu'), default='mps')
    run(parser.parse_args())
