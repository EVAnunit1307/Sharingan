"""Bounded live Pi/pose check, separate from the running dashboard and Quest."""
import argparse
from dataclasses import asdict
import json
from pathlib import Path
import sys
import threading
import time

ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT))
from GroundStation.fusion import FusionConfig
from GroundStation.server import RelayState,run_upstream
from GroundStation.pose import run_pose

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ip',default='172.20.10.3')
    parser.add_argument('--seconds',type=float,default=10)
    parser.add_argument('--out',type=Path,default=ROOT/'Saved/PersonPose/live-check.json')
    args=parser.parse_args()
    config=FusionConfig(**json.loads((ROOT/'Saved/GroundStation/fusion.json').read_text()))
    # A moving rig has no measured world pose; all checks here are sensor-relative.
    config=FusionConfig(**dict(asdict(config),rig_motion_mode='untracked'))
    state=RelayState(config);stop=threading.Event()
    workers=[threading.Thread(target=run_upstream,args=(state,f'ws://{args.ip}:8765/',stop),daemon=True),
        threading.Thread(target=run_pose,args=(state,f'http://{args.ip}:8766',ROOT/'Saved/PersonPose/models/pose_landmarker_full.task',stop),daemon=True)]
    for worker in workers:worker.start()
    samples=[]
    try:
        start=time.monotonic()
        while time.monotonic()-start<max(1,min(60,args.seconds)):
            time.sleep(.2)
            packet=state.snapshot()
            samples.append(dict(elapsed=round(time.monotonic()-start,3),pose=packet['pose_pipeline'],
                camera_connected=packet.get('camera_connected'),radar_status=packet.get('radar',{}).get('status'),
                tracks=len(packet['spatial_people']['tracks']),rig_pose_valid=packet['spatial_people']['rig_pose_valid'],
                error=state.error))
    finally:
        stop.set()
        for worker in workers:worker.join(timeout=7)
    result=dict(config=asdict(config),samples=samples,
        fresh_pose_frames=len({s['pose'].get('frame_id') for s in samples if s['pose']['status']=='live'}),
        camera_observed=any(s['camera_connected'] for s in samples),
        world_pose_measured=False)
    args.out.parent.mkdir(parents=True,exist_ok=True);args.out.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k not in ('samples','config')},indent=2))
    if result['fresh_pose_frames']==0:raise SystemExit('No fresh pose-worker frames; see output for diagnostics')

if __name__=='__main__':main()
