#!/usr/bin/env python3
"""Check a running sensor station from the Pi or Quest development computer.

Reads real HTTP/WebSocket data; never sends a pose or modifies sensor settings.
This validates transport and schema, not a headset build or physical alignment.
"""
import argparse
import json
import math
from pathlib import Path
import time
import urllib.request
from urllib.parse import urlsplit


def fetch(url):
    with urllib.request.urlopen(url, timeout=5) as response:
        return response.read()


def validate_packet(packet):
    if packet.get('schema_version') != 1:
        raise ValueError('Unexpected telemetry schema version')
    radar = packet['radar']
    contacts = packet['drone_relative_radar_targets']
    if not isinstance(contacts, list):
        raise ValueError('Radar contact array is missing')
    if not packet['rig']['tracking_ok'] and packet['detections']:
        raise ValueError('World contacts exist without a valid world pose')
    if not packet['camera_connected'] and packet['camera_people']:
        raise ValueError('Stale camera observations were exported')
    if radar['status'] != 'live' and contacts:
        raise ValueError('Stale radar observations were exported')
    targets = {t['id']: t for t in radar.get('targets', []) if t.get('drone_position_m') is not None}
    expected = set(targets) if radar['status'] == 'live' else set()
    if {t['id'] for t in contacts} != expected or len(contacts) != len(expected):
        raise ValueError('Drone-relative radar array disagrees with fresh mounted targets')
    for target in contacts:
        for axis in ('right_m', 'forward_m'):
            value = target[axis]
            if type(value) not in (int, float) or not math.isfinite(value):
                raise ValueError('Radar position must be finite metres')
        if target['confidence'] is not None or target['source'] != 'ld2450':
            raise ValueError('Radar source/confidence contract changed')
        if target['reference_origin'] != radar['reference_origin']:
            raise ValueError('Reference origins disagree')
        position = targets[target['id']]['drone_position_m']
        if abs(target['right_m']-position['right']) > .001 or abs(target['forward_m']-position['forward']) > .001:
            raise ValueError('Radar coordinates disagree between outputs')
    return radar


def check(base_url, seconds=3):
    from websockets.sync.client import connect
    base_url = base_url.rstrip('/')
    manifest = json.loads(fetch(base_url+'/handoff.json'))
    for path in ('/', '/quest'):
        page = fetch(base_url+path).decode()
        if 'id="scope"' not in page or 'id="feed"' not in page:
            raise ValueError(f'{path} does not contain both sensors')
    for path in ('/assets/dashboard.js', '/assets/dashboard.css'):
        if not fetch(base_url+path):
            raise ValueError(f'Empty browser asset: {path}')
    jpeg = fetch(base_url+'/snapshot.jpg')
    if not jpeg.startswith(b'\xff\xd8') or not jpeg.endswith(b'\xff\xd9'):
        raise ValueError('Camera snapshot is not a JPEG')
    if not manifest['websocket_url']:
        raise ValueError('Start the dashboard with --quest-port 8765')
    count = camera_live = radar_live = 0
    camera_frames, radar_frames = set(), set()
    maximum_age = 0
    started = time.monotonic()
    with connect(manifest['websocket_url'], open_timeout=5, max_size=1048576) as socket:
        while time.monotonic()-started < seconds:
            packet = json.loads(socket.recv(timeout=3))
            radar = validate_packet(packet)
            count += 1
            camera_live += int(packet['camera_connected'])
            radar_live += int(radar['status'] == 'live')
            camera_frames.add(packet['camera_frame_id'])
            radar_frames.add(radar.get('frame_id'))
            if radar['status'] == 'live':
                maximum_age = max(maximum_age, radar['age_ms'])
                if radar['age_ms'] > 500:
                    raise ValueError('Live radar packet exceeds freshness limit')
    passed = (count > 0 and count == camera_live == radar_live and len(camera_frames) > 1 and len(radar_frames) > 1)
    return dict(ready_for_browser_handoff=passed, checked_at=time.time(), base_url=base_url,
                packets=count, camera_live_packets=camera_live, radar_live_packets=radar_live,
                distinct_camera_frames=len(camera_frames), distinct_radar_frames=len(radar_frames),
                maximum_radar_age_ms=maximum_age, manifest=manifest,
                headset_tested=False, native_build_tested=False, through_wall_tested=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', default='http://127.0.0.1:8766')
    parser.add_argument('--seconds', type=float, default=3)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    url = urlsplit(args.url)
    if url.scheme not in ('http', 'https') or not url.hostname or not 1 <= args.seconds <= 60:
        parser.error('Use an HTTP(S) station URL and 1–60 seconds')
    try:
        report = check(args.url, args.seconds)
    except Exception as exc:
        report = dict(ready_for_browser_handoff=False, error=str(exc))
    output = json.dumps(report, indent=2, allow_nan=False)+'\n'
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(output)
    print(output, end='')
    raise SystemExit(0 if report['ready_for_browser_handoff'] else 1)


if __name__ == '__main__':
    main()
