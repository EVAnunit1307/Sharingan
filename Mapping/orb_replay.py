"""Replay a calibrated Pi recording through the isolated ORB-SLAM3 executable."""
import argparse
import csv
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import subprocess
import uuid

import numpy as np

from Mapping.camera_calibration import load as load_calibration
from Mapping.reconstruct import atomic_json

ROOT = Path(__file__).resolve().parent.parent
RESEARCH = ROOT/'Saved/MappingResearch/orb-slam3'
BINARY = RESEARCH/'build/orb_replay'
VOCABULARY = RESEARCH/'source/Vocabulary/ORBvoc.txt'
UPSTREAM = '4452a3c4ab75b1cde34e5505a36ec3f9edcdc4c4'
IMAGE = re.compile(r'images/[0-9]{9}\.jpg')
STATES = {-1:'starting', 0:'waiting', 1:'initializing', 2:'tracking', 3:'recently lost', 4:'lost', 5:'optical flow'}

def available():
    return BINARY.is_file() and VOCABULARY.is_file()

def prepare(session, output, features=1000):
    if not isinstance(features,int) or not 500 <= features <= 4000:
        raise ValueError('ORB feature count must be between 500 and 4000')
    session, output = Path(session), Path(output)
    calibration = load_calibration(session)
    if calibration is None:
        raise ValueError('Tracking replay requires a reviewed camera-calibration.json for this recording')
    manifest = json.loads((session/'manifest.json').read_text())
    rows = [json.loads(line) for line in (session/'frames.jsonl').read_text().splitlines() if line.strip()]
    if len(rows) < 2 or len({r.get('camera_generation') for r in rows}) != 1:
        raise ValueError('Need one camera generation and at least two frames')
    if any(not IMAGE.fullmatch(r.get('image','')) or not (session/r['image']).is_file() for r in rows):
        raise ValueError('Invalid or missing recorded image')
    if len({r['image'] for r in rows}) != len(rows):
        raise ValueError('Duplicate recorded image')
    clock = 'sensor_timestamp_ns' if all(isinstance(r.get('sensor_timestamp_ns'), int) for r in rows) else 'host_capture_mono_ns'
    times = [r.get(clock) for r in rows]
    if not all(isinstance(t, int) and t >= 0 for t in times) or any(b <= a for a,b in zip(times,times[1:])):
        raise ValueError('Frame timestamps must increase strictly')
    seconds = [(t-times[0])/1e9 for t in times]
    fps = float(1/np.median(np.diff(seconds)))
    output.mkdir(parents=True, exist_ok=True)
    atomic_json(output/'calibration.json', calibration)
    (output/'frames.txt').write_text(''.join(f'{t:.9f} {r["image"]}\n' for t,r in zip(seconds,rows)))
    settings = ['%YAML:1.0', 'File.version: "1.0"', 'Camera.type: "PinHole"']
    for key, value in zip(('fx','fy','cx','cy','k1','k2','p1','p2'), calibration['params']):
        settings.append(f'Camera1.{key}: {float(value):.12f}')
    settings += [f'Camera.width: {calibration["width"]}', f'Camera.height: {calibration["height"]}',
        f'Camera.fps: {max(1,round(fps))}', 'Camera.RGB: 0', f'ORBextractor.nFeatures: {features}',
        'ORBextractor.scaleFactor: 1.2', 'ORBextractor.nLevels: 8',
        'ORBextractor.iniThFAST: 20', 'ORBextractor.minThFAST: 7',
        'Viewer.KeyFrameSize: 0.05', 'Viewer.KeyFrameLineWidth: 1.0', 'Viewer.GraphLineWidth: 0.9',
        'Viewer.PointSize: 2.0', 'Viewer.CameraSize: 0.08', 'Viewer.CameraLineWidth: 3.0',
        'Viewer.ViewpointX: 0.0', 'Viewer.ViewpointY: -0.7', 'Viewer.ViewpointZ: -1.8', 'Viewer.ViewpointF: 500.0']
    (output/'camera.yaml').write_text('\n'.join(settings)+'\n')
    prepared = dict(session_id=manifest['session_id'], frames=rows, seconds=seconds,
                    fps=fps, features=features, timestamp_source=clock, calibration_sha256=calibration['source_sha256'])
    atomic_json(output/'input.json', prepared)
    return prepared

def read_csv(path):
    with Path(path).open() as file:
        return list(csv.DictReader(file))

def coarse_points(points):
    """Display-only aggregation in arbitrary map units; never infer occupied/free cells."""
    if not points:
        return [], None
    xyz = np.asarray(points, float)
    span = np.max(np.quantile(xyz, .95, axis=0)-np.quantile(xyz, .05, axis=0))
    size = max(float(span)/35, 1e-6)
    buckets = {}
    for point in xyz:
        buckets.setdefault(tuple(np.floor(point/size).astype(int)), []).append(point)
    return [[*np.mean(group, axis=0).tolist(), len(group)] for group in buckets.values()], size

def summarize(output, prepared):
    output = Path(output)
    states = read_csv(output/'tracking.csv')
    if len(states) != len(prepared['frames']) or [int(s['index']) for s in states] != list(range(len(states))):
        raise ValueError('Replay did not process every frame')
    by_time = {round(float(row['timestamp']), 6): row for row in read_csv(output/'poses.csv')}
    frames, maps = [], {}
    for row, raw, seconds in zip(states, prepared['frames'], prepared['seconds']):
        if abs(float(row['timestamp'])-seconds) > 1e-6:
            raise ValueError('Replay timestamps do not match the recording')
        state = int(row['state'])
        frame = dict(image=raw['image'], seconds=seconds, state=state, status=STATES.get(state,'unknown'),
                     landmarks=int(row['landmarks']), processing_ms=float(row['processing_ms']),
                     features=int(row['features']) if row.get('features') else None,
                     map_id=None, position=None, quaternion=None)
        pose = by_time.get(round(seconds,6))
        # A recently-lost prediction is not a measured camera position.
        if state == 2 and pose:
            position = [float(pose[k]) for k in ('x','y','z')]
            quaternion = [float(pose[k]) for k in ('qx','qy','qz','qw')]
            if not np.isfinite(position+quaternion).all():
                raise ValueError('Non-finite exported pose')
            map_id = int(pose['map_id'])
            frame.update(map_id=map_id, position=position, quaternion=quaternion)
            maps.setdefault(map_id, dict(id=map_id, points=[], frames=[]))['frames'].append(len(frames))
        frames.append(frame)
    for p in read_csv(output/'points.csv'):
        xyz = [float(p[k]) for k in ('x','y','z')]
        if not np.isfinite(xyz).all():
            raise ValueError('Non-finite exported landmark')
        # Ignore two-view-only landmarks for this coarse display.
        if int(p['observations']) >= 3:
            maps.setdefault(int(p['map_id']), dict(id=int(p['map_id']), points=[], frames=[]))['points'].append(xyz)
    components = sorted(maps.values(), key=lambda m: (len(m['frames']),len(m['points'])), reverse=True)
    longest = run = 0
    previous = None
    for frame in frames:
        if frame['position'] is not None:
            run = run+1 if frame['map_id'] == previous else 1
            previous = frame['map_id']
        else:
            run, previous = 0, None
        longest = max(longest,run)
    for component in components:
        component['coarse'], component['cell_size_arbitrary'] = coarse_points(component['points'])
    retained = sum(f['position'] is not None for f in frames)
    report = dict(schema_version=1, kind='orb_tracking_replay', session_id=prepared['session_id'], revision=output.name,
        created_at=datetime.now(timezone.utc).isoformat(), upstream_revision=UPSTREAM,
        units='arbitrary', scale='unknown', gravity_aligned=False, live=False,
        input_frames=len(frames), fps=prepared['fps'], duration_seconds=prepared['seconds'][-1],
        requested_features=prepared.get('features',1000),
        tracked_online=sum(int(s['state']) == 2 for s in states), retained_poses=retained,
        longest_contiguous_frames=longest, maps=components, frames=frames,
        calibration_sha256=prepared['calibration_sha256'],
        warning='Experimental replay. Separate fragments have independent origins and scales. Room layout and physical accuracy are unvalidated.',
        low_rate_warning='This recording saves only about 3 frames/second; test higher-rate capture before judging continuous tracking.' if prepared['fps'] < 10 else None,
        geometry_note='Coarse landmarks are grouped visual points, not measured walls or free space. No people positions have been added.')
    atomic_json(output/'result.json', report)
    return report

def run(session,features=1000):
    if not available():
        raise ValueError('Build the optional ORB-SLAM3 research executable first; see Mapping/orb/README.md')
    session = Path(session).resolve()
    revision = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')+'-'+uuid.uuid4().hex[:8]
    output = session/'tracking'/revision
    prepared = prepare(session, output,features)
    atomic_json(session/'reconstruction.json', dict(state='running', kind='tracking', phase='Replaying camera tracking at recorded speed'))
    provenance = dict(upstream_revision=UPSTREAM, binary_sha256=hashlib.sha256(BINARY.read_bytes()).hexdigest(),
                      settings_sha256=hashlib.sha256((output/'camera.yaml').read_bytes()).hexdigest())
    atomic_json(output/'build.json',provenance)
    with (output/'replay.log').open('w') as log:
        subprocess.run([str(BINARY),str(VOCABULARY),str(output/'camera.yaml'),str(output/'frames.txt'),str(session),str(output)],
                       cwd=output,stdout=log,stderr=subprocess.STDOUT,check=True,
                       timeout=max(180,prepared['seconds'][-1]+120))
    result = summarize(output,prepared)
    atomic_json(session/'tracking.json', result)
    atomic_json(session/'reconstruction.json', dict(state='complete', kind='tracking', revision=revision,
        phase=f'Tracking replay complete: {result["retained_poses"]}/{result["input_frames"]} saved poses across {len(result["maps"])} fragments'))
    return result

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--session', type=Path, required=True)
    parser.add_argument('--features',type=int,default=1000)
    args = parser.parse_args()
    try:
        result = run(args.session,args.features)
        print(json.dumps({k:v for k,v in result.items() if k not in ('frames','maps')}))
    except Exception as exc:
        atomic_json(args.session/'reconstruction.json', dict(state='failed',kind='tracking',phase=str(exc)))
        raise

if __name__ == '__main__':
    main()
