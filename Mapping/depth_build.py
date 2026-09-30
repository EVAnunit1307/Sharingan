"""Dashboard worker for an optional inferred surface layer, isolated from Torch."""
import argparse
from datetime import datetime,timezone
import json
from pathlib import Path
import subprocess
import sys
import uuid

from Mapping.learned import ROOT,RESEARCH,RESEARCH_PYTHON
from Mapping.reconstruct import atomic_json

PACKAGE=RESEARCH/'models'/'DepthAnythingV2SmallF16.mlpackage'


def available():
    return RESEARCH_PYTHON.is_file() and (PACKAGE/'Manifest.json').is_file()


def run(session):
    if not available():raise ValueError('Core ML depth environment is not installed; see Mapping/RESEARCH.md')
    if not (session/'scene.json').is_file():raise ValueError('Build a sparse map before adding inferred surfaces')
    revision=datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ-')+uuid.uuid4().hex[:8]
    def progress(phase):atomic_json(session/'reconstruction.json',dict(state='running',kind='depth',revision=revision,phase=phase))
    cache=RESEARCH/'depth-cache'/session.name
    progress('Predicting relative depth from camera images — not thermal imaging')
    subprocess.run([str(RESEARCH_PYTHON),'-m','Mapping.depth_infer','--session',str(session),
                    '--package',str(PACKAGE),'--output',str(cache)],cwd=ROOT,check=True)
    progress('Aligning depth and checking agreement across camera views')
    from Mapping.depth_fusion import run as fuse
    report=fuse(session,cache,session/'inferences'/revision,publish=True,
                relative_tolerance=.03,minimum_neighbors=3,cycle_pixels=1.)
    atomic_json(session/'reconstruction.json',dict(state='complete',kind='depth',revision=revision,
                source_revision=report['source_revision'],points=report['fused_points'],
                phase=f"Experimental inferred layer: {report['fused_points']:,} points; scale and physical accuracy unverified"))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--session',required=True,type=Path)
    args=parser.parse_args()
    try:run(args.session)
    except Exception as exc:
        atomic_json(args.session/'reconstruction.json',dict(state='failed',kind='depth',phase=str(exc)))
        print(str(exc),file=sys.stderr);raise SystemExit(1)
