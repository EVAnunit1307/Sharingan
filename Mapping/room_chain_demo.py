"""Reproduce the saved-room AI chain and overlapping-window experiment."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
from types import SimpleNamespace

from Mapping.room_demo import contained, digest, fetch, verify_assets

BUNDLE = Path(__file__).parent / 'examples/room-chain-20261003'


def verify():
    for name, expected in json.loads((BUNDLE / 'SHA256SUMS.json').read_text()).items():
        if digest(contained(BUNDLE, name)) != expected:
            raise ValueError('Checksum mismatch: ' + name)
    config = json.loads((BUNDLE / 'demo.json').read_text())
    for scene in config['scenes'].values():
        session = contained(BUNDLE, scene['session'])
        rows = [json.loads(line) for line in (session / 'frames.jsonl').read_text().splitlines()]
        if [r['source_row_index'] for r in rows] != scene['indices']:
            raise ValueError('Source frame selection changed')
        for row in rows:
            if not contained(session, row['image']).is_file():
                raise ValueError('Missing input image')
        if scene['undistort']:
            from Mapping.camera_calibration import load
            calibration = load(session)
            if calibration is None or calibration.get('status') != 'reviewed_for_recording':
                raise ValueError('Missing reviewed recording calibration')
    return config


def run(args, config):
    import numpy as np
    from Mapping.da3_trial import run as infer
    from Mapping.room_completion import generate
    from Mapping.window_replay import replay, overlap_transform, load_trial
    if args.output.exists(): raise ValueError('Choose a new output directory')
    source = args.source or args.assets / args.variant / 'source'
    model = args.model or args.assets / args.variant / 'model'
    verify_assets(source, model, dict(config, model=config['models'][args.variant]))
    scene = config['scenes'][args.scene]
    paths = []
    for i, window in enumerate(scene['windows']):
        trial = args.output / f'{args.variant}-{args.scene}-{i}'
        infer(SimpleNamespace(source=source, model=model,
            session=contained(BUNDLE, scene['session']), output=trial,
            start_frame=window['start'], stride=1, frames=24, duration_seconds=None,
            resolution=504, device=args.device, variant=args.variant,
            undistort=scene['undistort'], center_principal=False, pose_tracking=None,
            ray_pose=False, repeats=2, mps_memory_fraction=.6))
        with np.load(trial / 'prediction.npz', allow_pickle=False) as a:
            pixels_hash = hashlib.sha256(a['images'].tobytes()).hexdigest()
        if pixels_hash != window['processed_rgb_sha256']:
            raise ValueError('Processed pixels differ; cannot reuse semantic labels')
        labels = contained(BUNDLE, window['labels'])
        for name in ('semantics.json', 'semantics.npz'):
            shutil.copyfile(labels / name, trial / name)
        paths.append(trial)
    generate([paths[-1]], args.output / 'completion', [f'{args.scene.title()} · {args.variant.title()}'])
    if len(paths) > 1:
        replay(paths, args.output / 'stitch')
        checks = []
        for a, b in zip(paths, paths[1:]):
            _, check = overlap_transform(load_trial(a), load_trial(b))
            checks.append(check)
        (args.output / 'pair-checks.json').write_text(json.dumps(checks, indent=2) + '\n')
    print('Open', args.output / 'completion/index.html')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('verify')
    for command in ('fetch', 'run'):
        p = sub.add_parser(command)
        p.add_argument('--variant', choices=('small', 'base', 'large'), required=True)
        p.add_argument('--assets', type=Path, default=Path('Saved/MappingResearch/room-chain-assets'))
        if command == 'run':
            p.add_argument('--scene', choices=('couch', 'room'), required=True)
            p.add_argument('--source', type=Path)
            p.add_argument('--model', type=Path)
            p.add_argument('--device', choices=('cpu', 'mps'), default='cpu')
            p.add_argument('--output', type=Path, required=True)
    args = parser.parse_args(); config = verify()
    if args.command == 'fetch':
        fetch(args.assets / args.variant, dict(config, model=config['models'][args.variant]))
    elif args.command == 'run': run(args, config)
    else: print('Verified 64 source frames, calibration, cached image-matched labels, results and renders.')


if __name__ == '__main__': main()
