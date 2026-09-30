"""Reconstruct a reversible keyframe subset and report original-capture coverage."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time

from Mapping.keyframes import sharpest_windows


def run(session,output):
    rows=[json.loads(l) for l in (session/'frames.jsonl').read_text().splitlines()]
    indices=sharpest_windows(rows)
    output.mkdir(parents=True,exist_ok=False);(output/'images').mkdir()
    for i in indices:
        (output/rows[i]['image']).symlink_to((session/rows[i]['image']).resolve())
    manifest=json.loads((session/'manifest.json').read_text())
    manifest.update(source='drone_camera_keyframe_experiment',parent_session_id=session.name,
                    frames_saved=len(indices),keyframe_window_seconds=2/3)
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    (output/'frames.jsonl').write_text(''.join(json.dumps(rows[i])+'\n' for i in indices))
    started=time.perf_counter()
    with (output/'reconstruction.log').open('w') as log:
        completed=subprocess.run([sys.executable,'-m','Mapping.reconstruct','--session',str(output)],stdout=log,stderr=subprocess.STDOUT)
    elapsed=time.perf_counter()-started
    state=json.loads((output/'reconstruction.json').read_text())
    result={'parent_session_id':session.name,'original_images':len(rows),'selected_images':len(indices),
            'selection':'sharpest observed image per 2/3-second window plus endpoints',
            'elapsed_seconds':elapsed,'exit_code':completed.returncode,'reconstruction':state,
            'caveat':'Subset registration fraction is not coverage of the original walk. No physical accuracy measurement.'}
    (output/'trial.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--session',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();run(args.session,args.output)
