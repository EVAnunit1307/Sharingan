"""Bounded stationary camera check: one auto-stopping clip, then metadata-only monitoring."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import tempfile
import time
from urllib.request import Request, urlopen

import cv2
import numpy as np

from Mapping.reconstruct import atomic_json
from Mapping.server import import_archive


def utc():
    return datetime.now(timezone.utc).isoformat()


def request(base, action, post=False):
    req=Request(base.rstrip('/')+'/mapping/'+action, data=b'{}' if post else None,
                headers={'Content-Type':'application/json'} if post else {})
    with urlopen(req,timeout=5) as response:
        return json.load(response)


def analyze(session):
    manifest=json.loads((session/'manifest.json').read_text())
    rows=[json.loads(line) for line in (session/'frames.jsonl').read_text().splitlines() if line.strip()]
    failures=[];brightness=[];contrast=[];features=[]
    detector=cv2.ORB_create(nfeatures=1000)
    for row in rows:
        frame=cv2.imread(str(session/row['image']),cv2.IMREAD_GRAYSCALE)
        if frame is None:
            failures.append(row['image']);continue
        brightness.append(float(np.mean(frame)));contrast.append(float(np.std(frame)))
        features.append(len(detector.detect(frame,None)))
    host=np.array([r['host_capture_mono_ns'] for r in rows],dtype=np.int64)
    sensor=[r.get('sensor_timestamp_ns') for r in rows]
    sensor_ok=all(isinstance(t,int) for t in sensor)
    times=np.array(sensor if sensor_ok else host,dtype=np.int64)
    gaps=np.diff(times)/1e9
    exposure=[r['exposure_us'] for r in rows if r.get('exposure_us') is not None]
    return dict(session_id=manifest['session_id'],status=manifest['status'],frames=manifest['frames_saved'],
        metadata_rows=len(rows),dropped_frames=manifest['dropped_frames'],decode_failures=failures,
        host_timestamps_increasing=bool(np.all(np.diff(host)>0)) if len(host)>1 else None,
        sensor_timestamps_increasing=bool(np.all(np.diff(times)>0)) if sensor_ok and len(times)>1 else None,
        duration_seconds=float((times[-1]-times[0])/1e9) if len(times)>1 else 0,
        saved_fps=1/float(np.median(gaps)) if len(gaps) and np.median(gaps)>0 else None,
        maximum_frame_gap_seconds=float(np.max(gaps)) if len(gaps) else None,
        exposure_us_median=float(np.median(exposure)) if exposure else None,
        grayscale_mean_median=float(np.median(brightness)) if brightness else None,
        grayscale_std_median=float(np.median(contrast)) if contrast else None,
        orb_features_median=float(np.median(features)) if features else None,
        stop_reason=manifest.get('stop_reason'),
        note='Static image quality and recorder timing only. Feature count is not successful motion tracking, 3D accuracy or people detection.')


def run(base,output,root,monitor_seconds):
    output.mkdir(parents=True,exist_ok=False)
    summary=dict(state='starting',started_at=utc(),monitor_seconds=monitor_seconds,
        purpose='Stationary doorway capture and connection check',pi_http=base,
        temperature_power='Not measured: no authenticated SSH session',
        image_retention='One bounded Pi recording; up to seven local preview samples. Remaining monitoring is status metadata only.',
        notes=['Camera is stationary; this cannot validate room geometry, moving-camera tracking or metric people positions.',
               'Monitored on laptop; closing the lid or losing the hotspot can interrupt monitoring. Pi recording has its own time limit.'])
    atomic_json(output/'summary.json',summary)
    initial=request(base,'status')
    if initial.get('state')!='ready' or not initial.get('camera_live'):
        raise ValueError('Camera must be live and idle; existing recording left untouched')
    if not 5<=initial.get('max_seconds',0)<=120:
        raise ValueError('Pi must already have an automatic recording limit of at most 120 seconds')
    started=time.monotonic();deadline=started+monitor_seconds
    status_samples=[];preview_samples=[]
    capture=request(base,'start',True)
    identifier=capture['session_id']
    summary.update(state='recording',session_id=identifier,pi_auto_stop_seconds=initial['max_seconds'])
    atomic_json(output/'summary.json',summary)
    print(json.dumps(dict(state='recording',session_id=identifier,output=str(output))),flush=True)

    def sample(preview=False):
        row=dict(at=utc(),elapsed_seconds=round(time.monotonic()-started,3))
        try:
            before=time.monotonic();status=request(base,'status')
            row.update(status=status,request_ms=round((time.monotonic()-before)*1000,2))
        except Exception as exc:
            row['error']=str(exc)
        status_samples.append(row)
        with (output/'status.jsonl').open('a') as file:file.write(json.dumps(row)+'\n')
        if preview and 'status' in row:
            try:
                before=time.monotonic()
                with urlopen(base.rstrip('/')+'/mapping/preview.jpg',timeout=5) as response:
                    jpeg=response.read(2*1024*1024)
                    item=dict(at=utc(),frame_id=response.headers.get('X-Frame-Id'),
                              reported_age_ms=response.headers.get('X-Frame-Age-Ms'),
                              request_ms=round((time.monotonic()-before)*1000,2))
                gray=cv2.imdecode(np.frombuffer(jpeg,np.uint8),cv2.IMREAD_GRAYSCALE)
                if gray is None:raise ValueError('Preview failed to decode')
                item.update(width=gray.shape[1],height=gray.shape[0],mean=float(gray.mean()),
                            dark_fraction=float(np.mean(gray<20)),sha256=hashlib.sha256(jpeg).hexdigest())
                # Preview rate is independent of the recording; retain only a few examples.
                if len(preview_samples)%10==0:
                    name=f'preview-{len(preview_samples):03}.jpg';(output/name).write_bytes(jpeg);item['sample']=name
                preview_samples.append(item)
                with (output/'preview.jsonl').open('a') as file:file.write(json.dumps(item)+'\n')
            except Exception as exc:
                with (output/'preview.jsonl').open('a') as file:file.write(json.dumps(dict(at=utc(),error=str(exc)))+'\n')
        return row

    try:
        while time.monotonic()-started<initial['max_seconds']+12:
            row=sample(preview=True)
            status=row.get('status',{})
            if status.get('session_id')==identifier and status.get('state')=='ready':break
            if status.get('session_id') not in (None,identifier):
                raise ValueError('Another recording appeared; leaving it untouched')
            time.sleep(2)
    finally:
        # Stop only our own session. The Pi's independent limit still applies if Wi-Fi drops.
        try:
            state=request(base,'status')
            if state.get('session_id')==identifier and state.get('state') in ('recording','saving'):
                request(base,'stop',True)
        except Exception as exc:summary['stop_check_error']=str(exc)
    try:
        with tempfile.TemporaryFile() as archive:
            with urlopen(base.rstrip('/')+'/mapping/sessions/'+identifier+'/archive',timeout=20) as response:
                size=0
                while block:=response.read(1024*1024):
                    size+=len(block)
                    if size>128*1024*1024:raise ValueError('Stationary archive exceeds 128 MiB')
                    archive.write(block)
            archive.seek(0);session=import_archive(archive,root,identifier)
        atomic_json(session/'annotations.json',dict(display_label='STATIONARY DOORWAY · camera stability test',
            purpose='Stationary timing/image-quality check; not a room scan',research_output=str(output)))
        summary['capture']=analyze(session)
        summary['archive_bytes']=size
        summary['local_session']=str(session)
        print(json.dumps(dict(state='capture_analyzed',capture=summary['capture'])),flush=True)
    except Exception as exc:
        summary['capture_analysis_error']=str(exc)

    def update(state):
        successes=[s for s in status_samples if 'status' in s]
        ids=[int(p['frame_id']) for p in preview_samples if str(p.get('frame_id','')).isdigit()]
        summary.update(state=state,last_update=utc(),elapsed_seconds=round(time.monotonic()-started,2),
            status_samples=len(status_samples),status_errors=sum('error' in s for s in status_samples),
            camera_not_live_samples=sum(not s['status'].get('camera_live') for s in successes),
            camera_error_samples=sum(bool(s['status'].get('error')) for s in successes),
            preview_samples=len(preview_samples),
            preview_sequence_increasing=all(b>a for a,b in zip(ids,ids[1:])) if len(ids)>1 else None,
            last_camera_state=successes[-1]['status']['state'] if successes else None)
        atomic_json(output/'summary.json',summary)

    update('monitoring_metadata')
    while time.monotonic()<deadline:
        time.sleep(min(15,max(0,deadline-time.monotonic())))
        if time.monotonic()>=deadline:break
        sample();update('monitoring_metadata')
    update('complete')
    print(json.dumps(summary),flush=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pi-http',required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--root',type=Path,default=Path('Saved/Mapping'))
    parser.add_argument('--monitor-seconds',type=int,default=600)
    args=parser.parse_args()
    if not 150<=args.monitor_seconds<=1800:parser.error('Use a bounded 150–1800 second check')
    if args.output.exists():parser.error('Choose a new output directory; existing results are preserved')
    try:run(args.pi_http,args.output.resolve(),args.root.resolve(),args.monitor_seconds)
    except Exception as exc:
        path=args.output/'summary.json'
        result=json.loads(path.read_text()) if path.exists() else {}
        result.update(state='failed',error=str(exc),last_update=utc())
        if path.parent.exists():atomic_json(path,result)
        raise
