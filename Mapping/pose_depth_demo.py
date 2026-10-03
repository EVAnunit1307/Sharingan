"""Reproduce the fixed Small/Base and ORB-conditioning experiment without a Pi."""
import argparse
import json
from pathlib import Path
import shutil
from types import SimpleNamespace

from Mapping.room_demo import contained, digest, fetch, verify_assets

BUNDLE = Path(__file__).parent / 'examples/pose-depth-20261003'


def verify():
    for name, expected in json.loads((BUNDLE/'SHA256SUMS.json').read_text()).items():
        if digest(contained(BUNDLE,name)) != expected:
            raise ValueError('Checksum mismatch: '+name)
    return json.loads((BUNDLE/'demo.json').read_text())


def run(args, config):
    if args.output.exists():
        raise ValueError('Choose a new output directory')
    from Mapping.da3_trial import run as infer
    from Mapping.ai_room_draft import generate
    import hashlib
    import numpy as np
    source = args.source or args.assets/args.variant/'source'
    model = args.model or args.assets/args.variant/'model'
    asset_config = dict(config,model=config['models'][args.variant])
    verify_assets(source,model,asset_config)
    session=contained(BUNDLE,config['session'])
    trial=args.output/'trial'
    infer(SimpleNamespace(source=source,model=model,session=session,output=trial,
        start_frame=0,stride=1,frames=config['frames'],duration_seconds=None,
        resolution=config['resolution'],device=args.device,variant=args.variant,
        undistort=True,center_principal=not args.original_principal,
        pose_tracking=session/'tracking.json' if args.guided else None,
        ray_pose=False,repeats=2,mps_memory_fraction=.6))
    with np.load(trial/'prediction.npz',allow_pickle=False) as data:
        rgb_hash=hashlib.sha256(data['images'].tobytes()).hexdigest()
    expected = config['processed_rgb_sha256'] if args.original_principal else config['centered_processed_rgb_sha256']
    if rgb_hash != expected:
        raise ValueError('Processed pixels differ; refusing cached labels')
    labels = BUNDLE/'labels' if args.original_principal else BUNDLE/'labels/centered'
    for name in ('semantics.npz','semantics.json'):
        shutil.copyfile(labels/name,trial/name)
    label=args.variant.title()+(' · ORB-guided depth' if args.guided else ' · estimated cameras')
    generate([trial],args.output/'index.html',[label])
    print('Open',args.output/'index.html')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    sub=parser.add_subparsers(dest='command',required=True)
    sub.add_parser('verify')
    for name in ('fetch','run'):
        p=sub.add_parser(name)
        p.add_argument('--variant',choices=('small','base'),required=True)
        p.add_argument('--assets',type=Path,default=Path('Saved/MappingResearch/pose-depth-assets'))
        if name=='run':
            p.add_argument('--source',type=Path)
            p.add_argument('--model',type=Path)
            p.add_argument('--device',choices=('cpu','mps'),default='cpu')
            p.add_argument('--guided',action='store_true')
            p.add_argument('--original-principal',action='store_true',help='Reproduce the first diagnostic with off-center K; default is the corrected centered crop')
            p.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();config=verify()
    if args.command=='fetch':
        fetch(args.assets/args.variant,dict(config,model=config['models'][args.variant]))
    elif args.command=='run': run(args,config)
    else: print('Verified fixed 24-frame inputs, supplied poses, labels, results and renders.')


if __name__=='__main__': main()
