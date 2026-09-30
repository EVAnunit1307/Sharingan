"""Short, reversible capture comparisons through the existing camera owner."""
import argparse
import io
import json
from pathlib import Path
import time
from urllib.request import Request,urlopen

import cv2
import numpy as np

from Mapping.server import import_archive


def run(pi_http,root,output,seconds=6):
    def api(action,payload=None):
        request=Request(pi_http.rstrip('/')+'/mapping/'+action,
                        data=json.dumps(payload).encode() if payload is not None else None,
                        headers={'Content-Type':'application/json'})
        with urlopen(request,timeout=15) as response:return json.load(response)
    original=api('status')
    if original['state']!='ready':raise ValueError('A recording is active; capture trials will not interrupt it')
    restore=dict(selection=original.get('frame_selection','uniform'),
                 exposure_us=original.get('requested_exposure_us'),gain=original.get('requested_gain',8))
    output.mkdir(parents=True,exist_ok=False)
    reports=[];started_id=None
    try:
        profiles=[('uniform_auto',dict(selection='uniform',exposure_us=None,gain=8)),
                  ('burst_auto',dict(selection='sharpest',exposure_us=None,gain=8)),
                  ('burst_20ms',dict(selection='sharpest',exposure_us=20000,gain=8)),
                  ('burst_10ms',dict(selection='sharpest',exposure_us=10000,gain=8))]
        for label,settings in profiles:
            api('configure',settings);time.sleep(1.5)
            state=api('start',{});started_id=state['session_id'];time.sleep(seconds)
            api('stop',{});status=api('status')
            with urlopen(pi_http.rstrip('/')+'/mapping/sessions/'+started_id+'/archive',timeout=30) as response:
                session=import_archive(io.BytesIO(response.read()),root,started_id)
            (session/'annotations.json').write_text(json.dumps(dict(display_label='CAPTURE TEST · '+label,purpose='controlled capture settings experiment'))+'\n')
            rows=[json.loads(line) for line in (session/'frames.jsonl').read_text().splitlines() if line.strip()]
            brightness=[];black=[];white=[]
            for row in rows:
                gray=cv2.cvtColor(cv2.imread(str(session/row['image'])),cv2.COLOR_BGR2GRAY)
                brightness.append(float(np.mean(gray)));black.append(float(np.mean(gray<8)));white.append(float(np.mean(gray>247)))
            times=np.array([row['host_capture_mono_ns'] for row in rows])/1e9
            report=dict(profile=label,settings=settings,session_id=started_id,frames=len(rows),
                        median_exposure_us=float(np.median([row['exposure_us'] for row in rows])),
                        median_gain=float(np.median([row['analogue_gain'] for row in rows])),
                        median_luminance=float(np.median(brightness)),near_black_fraction=float(np.median(black)),
                        near_white_fraction=float(np.median(white)),
                        median_sharpness=float(np.median([row['sharpness'] for row in rows])),
                        median_candidates=float(np.median([row.get('selection_candidates',1) for row in rows])),
                        max_frame_gap_seconds=float(np.max(np.diff(times))) if len(times)>1 else None,
                        dropped_frames=json.loads((session/'manifest.json').read_text())['dropped_frames'],
                        applied_controls=status.get('applied_controls'))
            reports.append(report);print(json.dumps(report),flush=True);started_id=None
            if rows:
                image=cv2.imread(str(session/rows[len(rows)//2]['image']))
                cv2.imwrite(str(output/(label+'.jpg')),image)
    finally:
        state=api('status')
        if started_id and state.get('session_id')==started_id and state['state'] in ('recording','saving'):api('stop',{})
        if api('status')['state']=='ready':api('configure',restore)
        (output/'report.json').write_text(json.dumps(dict(trials=reports,restored_settings=restore,
            note='Sequential short captures; scene/camera motion was not controlled. Sharpness also reflects texture and noise, not motion blur alone.'),indent=2)+'\n')


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--pi-http',required=True)
    p.add_argument('--root',type=Path,default=Path('Saved/Mapping'));p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();run(a.pi_http,a.root,a.output)
