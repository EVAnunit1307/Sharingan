"""Render a local, self-contained Mac benchmark and window-replay report."""
import argparse
import json
from pathlib import Path

import numpy as np

from Mapping.reference_benchmark import write_json


def render(root, output):
    root,output=Path(root),Path(output)
    profiles=json.loads((root/'profiles.json').read_text())
    for profile in profiles:
        report=json.loads((root/profile['name']/'summary.json').read_text())
        profile.update(frames=len(report['indices']),width=report['shapes']['depth'][2],height=report['shapes']['depth'][1],
                       pipeline_seconds=report['pipeline_seconds'])
    scenes=[]
    for label,name in [('Stool recording','chair-replay'),('Public TUM room','tum-replay-v3'),('Our room sweep','room-replay-v2')]:
        scene=json.loads((root/name/'replay.json').read_text());scene['label']=label;scenes.append(scene)
    checks=[json.loads(p.read_text()) for p in sorted(root.glob('tum-window-*/reference-evaluation.json'))]
    reference=dict(windows=len(checks),source=checks[0]['source'],
        median_window_ate_metres=float(np.median([c['trajectory']['ate_rmse_metres'] for c in checks])),
        min_window_ate_metres=min(c['trajectory']['ate_rmse_metres'] for c in checks),
        max_window_ate_metres=max(c['trajectory']['ate_rmse_metres'] for c in checks),
        mean_window_depth_abs_relative=float(np.mean([c['depth']['mean_frame_abs_relative'] for c in checks])),
        warning='Each short window is separately aligned to reference positions with scale fitted. This does not establish a metric or continuous whole-room trajectory.')
    runs=json.loads((root/'window-runs.json').read_text())
    first=json.loads((root/profiles[0]['name']/'summary.json').read_text())
    payload=dict(hardware=json.loads((root/'hardware.json').read_text()),profiles=profiles,scenes=scenes,reference=reference,
        window_batches=len(runs),window_process_seconds=sum(r['process_wall_seconds'] for r in runs),
        mps_recommended_bytes=first['mps_recommended_bytes'],mps_allocator_limit_bytes=first['mps_allocator_limit_bytes'])
    write_json(root/'report-summary.json',{k:v for k,v in payload.items() if k!='scenes'})
    template=(Path(__file__).parent/'static'/'mac-report.html').read_text()
    output.write_text(template.replace('/*REPORT_DATA*/null',json.dumps(payload,separators=(',',':'),allow_nan=False).replace('<','\\u003c')))
    return dict(output=str(output),reference=reference,accepted={s['label']:s['accepted_windows'] for s in scenes})


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--root',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();print(json.dumps(render(a.root,a.output),indent=2))
