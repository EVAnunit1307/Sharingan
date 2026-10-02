"""Verify and reproduce the committed DA3 room examples without a Pi.

Only `fetch` uses the network. `run` preserves the checked-in inputs/results and
requires a new output directory. Open the bundled index.html for the saved result.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
from types import SimpleNamespace
from urllib.request import urlopen


BUNDLE = Path(__file__).parent / 'examples/room-draft-20261002'
ASSETS = Path('Saved/MappingResearch/team-demo-assets')


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def contained(root, name):
    root = Path(root).resolve()
    path = (root / name).resolve()
    if not path.is_relative_to(root):
        raise ValueError(f'Path outside bundle: {name}')
    return path


def verify(bundle=BUNDLE):
    entries = json.loads((bundle / 'SHA256SUMS.json').read_text())
    for name, expected in entries.items():
        if digest(contained(bundle, name)) != expected:
            raise ValueError(f'Checksum mismatch: {name}')
    config = json.loads((bundle / 'demo.json').read_text())
    for scene in config['scenes'].values():
        session = contained(bundle, scene['session'])
        rows = [json.loads(line) for line in (session / 'frames.jsonl').read_text().splitlines()]
        if len(rows) != config['frames'] or [r['source_row_index'] for r in rows] != scene['source_indices']:
            raise ValueError('Selected-frame provenance does not match demo')
        for row in rows:
            image = contained(session, row['image'])
            if image.relative_to(bundle.resolve()).as_posix() not in entries:
                raise ValueError(f'Image not covered by checksums: {image.name}')
        if any(b['sensor_timestamp_ns'] <= a['sensor_timestamp_ns'] for a, b in zip(rows, rows[1:])):
            raise ValueError('Input timestamps must increase')
    return config


def verify_assets(source, model, config):
    commit = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    dirty = subprocess.check_output(['git', '-C', str(source), 'status', '--porcelain'], text=True).strip()
    if commit != config['source_commit'] or dirty:
        raise ValueError('DA3 source must be clean and at the pinned commit')
    for name, entry in config['model']['files'].items():
        if digest(model / name) != entry['sha256']:
            raise ValueError(f'Model checksum mismatch: {name}')


def fetch(assets, config):
    assets.mkdir(parents=True, exist_ok=True)
    source, model = assets / 'source', assets / 'model'
    if not source.exists():
        subprocess.run(['git', 'init', str(source)], check=True)
    if not (source / '.git').is_dir():
        raise ValueError('Choose an asset folder containing a DA3 git checkout or a new folder')
    head = subprocess.run(['git', '-C', str(source), 'rev-parse', '--verify', 'HEAD'], capture_output=True)
    if head.returncode:
        subprocess.run(['git', '-C', str(source), 'fetch', '--depth=1', config['source_repository'], config['source_commit']], check=True, timeout=300)
        subprocess.run(['git', '-C', str(source), 'checkout', '--detach', config['source_commit']], check=True)
    model.mkdir(exist_ok=True)
    for name, entry in config['model']['files'].items():
        output = model / name
        if output.exists():
            if digest(output) != entry['sha256']:
                raise ValueError(f'Existing model differs: {output}; choose a new asset folder')
            continue
        url = f"https://huggingface.co/{config['model']['repo']}/resolve/{config['model']['revision']}/{name}"
        print(f"Downloading {name} ({entry['bytes'] / 1e6:.1f} MB)", flush=True)
        temporary = output.with_suffix(output.suffix + '.part')
        with urlopen(url, timeout=120) as response, temporary.open('wb') as stream:
            shutil.copyfileobj(response, stream)
        if digest(temporary) != entry['sha256']:
            raise ValueError(f'Download checksum mismatch: {name}')
        temporary.rename(output)
    verify_assets(source, model, config)


def reproduce(args, config):
    if args.output.exists():
        raise ValueError('Choose a new output directory; previous results are preserved')
    if args.ray_pose and args.device != 'cpu':
        raise ValueError('Use CPU for the alternate ray-head example; MPS ray fitting failed on the tested Mac')
    source = args.source or args.assets / 'source'
    model = args.model or args.assets / 'model'
    verify_assets(source, model, config)
    from Mapping.da3_trial import run
    from Mapping.ai_room_draft import generate
    import numpy as np

    scene = config['scenes'][args.scene]
    trial = args.output / 'trial'
    run(SimpleNamespace(source=source, model=model, session=contained(BUNDLE, scene['session']),
        output=trial, start_frame=0, stride=1, frames=config['frames'], duration_seconds=None,
        resolution=config['resolution'], device=args.device, ray_pose=args.ray_pose,
        repeats=1, mps_memory_fraction=.6))
    # Cached label predictions belong to these exact processed RGB pixels. They
    # are independent of depth/poses, and are not a fresh segmentation benchmark.
    with np.load(trial / 'prediction.npz', allow_pickle=False) as prediction:
        pixels_hash = hashlib.sha256(prediction['images'].tobytes()).hexdigest()
    if pixels_hash != scene['processed_rgb_sha256']:
        raise ValueError('Preprocessed images differ; refusing to attach cached semantic labels')
    for name in ('semantics.npz', 'semantics.json'):
        shutil.copyfile(contained(BUNDLE, scene['labels']) / name, trial / name)
    generate([trial], args.output / 'index.html', [scene['display_label']])
    (args.output / 'reproduction.json').write_text(json.dumps(dict(
        scene=args.scene, source_indices=scene['source_indices'], bundle_manifest_sha256=digest(BUNDLE / 'SHA256SUMS.json'),
        segmentation='bundled cached SegFormer predictions, verified against processed RGB',
        pose_head='ray_head' if args.ray_pose else 'camera_decoder', device=args.device,
        note='Fresh DA3 inference. Floating-point results can differ across devices/versions. No ground truth or metric scale.'), indent=2) + '\n')
    print(f"Open {args.output / 'index.html'}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('verify')
    download = sub.add_parser('fetch')
    download.add_argument('--assets', type=Path, default=ASSETS)
    run = sub.add_parser('run')
    run.add_argument('--assets', type=Path, default=ASSETS)
    run.add_argument('--source', type=Path, help='Existing clean pinned DA3 source checkout')
    run.add_argument('--model', type=Path, help='Existing pinned checkpoint folder')
    run.add_argument('--scene', choices=('room-sweep', 'lit-sofa'), default='room-sweep')
    run.add_argument('--device', choices=('mps', 'cpu'), default='cpu')
    run.add_argument('--ray-pose', action='store_true')
    run.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    config = verify()
    if args.command == 'fetch':
        fetch(args.assets, config)
        print(f'Verified pinned DA3 source and model in {args.assets}')
    elif args.command == 'run':
        reproduce(args, config)
    else:
        print('Verified both 24-image selections, cached labels, provenance and saved renders.')


if __name__ == '__main__':
    main()
