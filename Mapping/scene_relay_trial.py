"""Deterministic, file-only scene relay experiment using completed AI batches.

The measured warm model times provide an optimistic producer schedule. Capture
uplink, decoding, preprocessing and geometry fitting are not timed by this model.
No radio, camera, GPU model, live server or flight-control connection is opened.
"""
import argparse
from collections import Counter, deque
import hashlib
import heapq
import json
import math
from pathlib import Path
import random

from GroundStation.scene_transport import SCHEMA, SceneReceiver, chunk_scene, encode


PROFILES = [
    dict(name='clear', label='Clear link', bits_per_second=2_000_000, latency_seconds=.06,
         jitter_seconds=0., drop_probability=0., outage=None),
    dict(name='limited', label='Limited bandwidth', bits_per_second=256_000, latency_seconds=.12,
         jitter_seconds=0., drop_probability=0., outage=None),
    dict(name='interrupted', label='Loss + interruption', bits_per_second=512_000, latency_seconds=.15,
         jitter_seconds=.20, drop_probability=.05, outage=[43., 53.]),
]


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2, allow_nan=False) + '\n')


def prepare(replay_path, trials, output):
    import numpy as np
    if output.exists(): raise ValueError('Choose a new input path')
    raw = replay_path.read_bytes(); replay = json.loads(raw)
    if replay['accepted_windows'] != 3 or len(replay['steps']) != 3:
        raise ValueError('This fixed experiment requires the three retained couch sections')
    summaries = [json.loads((p / 'summary.json').read_text()) for p in trials]
    if any(s['session_id'] != replay['source_session'] for s in summaries):
        raise ValueError('Trial recording mismatch')
    if any(s['indices'] != step['indices'] for s, step in zip(summaries, replay['steps'])):
        raise ValueError('Trial selection mismatch')
    if any(s['checkpoint_sha256'] != summaries[0]['checkpoint_sha256'] for s in summaries):
        raise ValueError('Trial model mismatch')
    checksum = hashlib.sha256(raw).hexdigest()
    identity = dict(session_id='saved-couch-' + checksum[:16],
                    map_id='couch-streak-only-' + checksum[:16],
                    frame_id='seed-camera-' + checksum[:16],
                    clock_id='recording-relative-' + replay['source_session'])
    points = replay['points']
    # Fixed uniform index subsampling, for display/transport only. No new fitting.
    points = [points[round(i * (len(points) - 1) / 5999)] for i in range(min(6000, len(points)))] if len(points) > 6000 else points
    scenes = []
    for step in range(3):
        poses = [dict(position=p['position'], observed_ns=round(p['seconds'] * 1e9))
                 for p in replay['poses'] if p['step'] <= step]
        scenes.append(dict(schema=SCHEMA, **identity, revision=step + 1,
            live=False, mapping_eligible=False, units='arbitrary', observed_ns=poses[-1]['observed_ns'],
            points=[p[:6] for p in points if p[6] <= step], poses=poses))
    with np.load(trials[0] / 'prediction.npz', allow_pickle=False) as data:
        rotation = data['extrinsics'][0, :, :3].tolist()
    value = dict(schema='wallhack.scene_relay_input.v1', identity=identity,
        source_session=replay['source_session'], source_replay_sha256=checksum,
        scenes=scenes, images=replay['images'], first_camera_rotation=rotation,
        warm_model_seconds=[s['inference_seconds'] for s in summaries],
        source_model=dict(name=summaries[0]['model'], checkpoint_sha256=summaries[0]['checkpoint_sha256'],
                          source_commit=summaries[0]['source_commit']),
        notes=['Completed batches only; no future section is released before its final observation plus model time.',
               'Model-only times are measured; end-to-end pipeline latency and radio performance are not.',
               'No IMU, metric scale, live poses or real people. Images are local reference assets and are not sent over the simulated link.'])
    output.parent.mkdir(parents=True, exist_ok=True); write_json(output, value)
    return value


def simulate(source, profile, duration=85.):
    """One serialized bidirectional link, prioritized small messages, latest map.

    Retransmission requests consume bandwidth and can be lost like other packets.
    One map queue replaces superseded unsent chunks; in-flight chunks can arrive
    late. A missing-chunk request is sent every 2 s, only after a recent heartbeat
    advertises a revision. Sender-side pending indices suppress duplicate queuing.
    """
    rng = random.Random(73)
    identity = source['identity']
    receiver = SceneReceiver(**identity, clock_offset_ns=0)
    scenes = source['scenes']; chunks = {s['revision']: chunk_scene(s) for s in scenes}
    if len(scenes) != 3 or [s['revision'] for s in scenes] != [1, 2, 3] or len(source['warm_model_seconds']) != 3:
        raise ValueError('Expected three ordered recorded sections and their model times')
    if any(any(s[k] != v for k, v in identity.items()) for s in scenes):
        raise ValueError('Inconsistent scene identity')
    receiver_time = lambda seconds: round(seconds * 1e9)
    events = []; serial = 0
    def event(at, kind, value=None):
        nonlocal serial
        serial += 1; heapq.heappush(events, (round(at, 9), serial, kind, value))
    ready = []; done = 0.
    for scene, model_seconds in zip(scenes, source['warm_model_seconds']):
        if not math.isfinite(model_seconds) or model_seconds <= 0: raise ValueError('Expected measured positive model time')
        done = max(done, scene['observed_ns'] / 1e9) + model_seconds
        ready.append(done); event(done, 'publish', scene)
    for i in range(int(duration) + 1): event(float(i), 'heartbeat', i)
    for i in range(2, int(duration), 2): event(float(i), 'repair')
    for i in range(round(duration * 4) + 1): event(i / 4, 'sample')
    priority = deque(); bulk = deque(); queued = set(); busy = False
    latest = 0; counters = Counter(); trace = []; timeline = []; installations = []
    max_bulk = 0; max_priority = 0; bytes_sent = 0; bytes_delivered = 0
    def enqueue(p):
        nonlocal max_priority
        # Periodic heartbeats and repair requests supersede their unsent precursor.
        if p['kind'] in ('heartbeat', 'repair'):
            remaining = [v for v in priority if v['kind'] != p['kind']]
            priority.clear(); priority.extend(remaining)
        priority.append(p); max_priority = max(max_priority, len(priority))
    def queue_chunks(revision, indices):
        nonlocal max_bulk
        for index in indices:
            key = (revision, index)
            if key not in queued:
                bulk.append(chunks[revision][index]); queued.add(key)
        max_bulk = max(max_bulk, len(bulk))
    def outage(at):
        return profile['outage'] is not None and profile['outage'][0] <= at < profile['outage'][1]
    while events:
        now, _, kind, value = heapq.heappop(events)
        if now > duration: break
        if kind == 'publish':
            latest = value['revision']; counters['superseded_unsent_chunks'] += len(bulk)
            bulk.clear(); queued.clear(); queue_chunks(latest, range(len(chunks[latest])))
            last = value['poses'][-1]
            enqueue(dict(schema=SCHEMA, **identity, kind='camera', revision=latest,
                         sequence=latest, tracking=True, **last))
            trace.append(dict(seconds=now, event='batch_ready', revision=latest))
        elif kind == 'heartbeat':
            enqueue(dict(schema=SCHEMA, **identity, kind='heartbeat', revision=latest,
                         sequence=value, observed_ns=receiver_time(now)))
        elif kind == 'repair':
            state = receiver.state(receiver_time(now))
            revision = receiver.advertised_revision
            if state['link_recent'] and revision > state['revision']:
                missing = receiver.missing() if receiver.pending and receiver.pending['revision'] == revision else None
                enqueue(dict(schema=SCHEMA, **identity, kind='repair', revision=revision, missing=missing))
        elif kind == 'free':
            busy = False
        elif kind == 'deliver':
            packet, wire_bytes = value; bytes_delivered += wire_bytes
            if packet['kind'] == 'repair':
                revision = packet['revision']
                if revision == latest:
                    indices = packet['missing'] if packet['missing'] is not None else range(len(chunks[latest]))
                    queue_chunks(latest, indices)
                    counters['repair_requests_received'] += 1
            else:
                result = receiver.receive(packet, receiver_time(now)); counters[result] += 1
                if result == 'installed':
                    item = dict(seconds=now, revision=packet['revision'],
                        observation_age_seconds=now - receiver.scene['observed_ns'] / 1e9,
                        relay_seconds=now - ready[packet['revision'] - 1])
                    installations.append(item); trace.append(dict(event='installed', **item))
                elif result not in ('partial', 'heartbeat', 'duplicate_chunk'):
                    trace.append(dict(seconds=now, event=result, kind=packet['kind'], revision=packet.get('revision')))
        elif kind == 'sample':
            state = receiver.state(receiver_time(now))
            timeline.append(dict(seconds=now, **state, sender_revision=latest,
                                 wire_bytes_sent=bytes_sent, wire_bytes_delivered=bytes_delivered))
        if not busy and (priority or bulk):
            packet = priority.popleft() if priority else bulk.popleft()
            if packet['kind'] == 'chunk': queued.discard((packet['revision'], packet['index']))
            # Includes JSON envelope/base64, plus a newline delimiter; excludes
            # modem/IP framing/FEC. The assumed bit rate is application capacity.
            wire_bytes = len(encode(packet)) + 1
            bytes_sent += wire_bytes; counters['packets_sent'] += 1
            end = now + wire_bytes * 8 / profile['bits_per_second']
            arrival = end + profile['latency_seconds'] + rng.uniform(0, profile['jitter_seconds'])
            busy = True; event(end, 'free')
            blocked = outage(now) or outage(end) or outage(arrival)
            lost = rng.random() < profile['drop_probability']
            if blocked or lost:
                counters['dropped_packets'] += 1
                counters['outage_drops' if blocked else 'random_drops'] += 1
            else: event(arrival, 'deliver', (packet, wire_bytes))
    final = receiver.state(receiver_time(duration))
    summary = dict(profile=profile, duration_seconds=duration,
        model_ready_seconds=ready, installations=installations, final_revision=final['revision'],
        final_visible_poses=len(receiver.scene['poses']) if receiver.scene else 0,
        fresh_camera_samples=sum(s['current_camera'] is not None for s in timeline),
        max_queued_map_chunks=max_bulk, max_queued_priority_packets=max_priority,
        wire_bytes_sent=bytes_sent, wire_bytes_delivered=bytes_delivered, counters=dict(counters))
    return dict(summary=summary, timeline=timeline, trace=trace)


def run(input_path, output):
    if output.exists(): raise ValueError('Choose a new result directory')
    raw = input_path.read_bytes(); source = json.loads(raw)
    if source['schema'] != 'wallhack.scene_relay_input.v1': raise ValueError('Unsupported input')
    output.mkdir(parents=True)
    results = [simulate(source, p) for p in PROFILES]
    for result in results:
        name = result['summary']['profile']['name']
        write_json(output / (name + '.json'), result)
    summary = dict(schema='wallhack.scene_relay_result.v1', input_sha256=hashlib.sha256(raw).hexdigest(),
        results=[r['summary'] for r in results],
        assumptions=dict(pose_ttl_seconds=2, clock_offset_ns=0, clock_mapping='Exact shared simulation clock only',
            transport='One serialized bidirectional application-byte link. Seed 73, 2 s selective repair requests.',
            producer='Last selected observation + measured warm model call. Optimistic schedule, not measured streaming latency.',
            excluded='Image uplink, preprocessing, alignment, map serialization CPU, radio headers/FEC and hardware behavior.',
            link_scope='Ground-computed scene to a display/relay consumer; does not show the Pi computing these scenes.',
            current_camera='All scene poses remain timestamped at capture. Withheld if older than 2 s even after reconnect.'))
    write_json(output / 'summary.json', summary)
    payload = dict(source=source, results=results, assumptions=summary['assumptions'])
    template = (Path(__file__).parent / 'static/scene-relay.html').read_text()
    (output / 'index.html').write_text(template.replace('/*RELAY_DATA*/null', encode(payload).decode().replace('<', '\\u003c')))
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('prepare'); p.add_argument('--replay', type=Path, required=True)
    p.add_argument('--trials', type=Path, nargs=3, required=True); p.add_argument('--output', type=Path, required=True)
    p = sub.add_parser('run'); p.add_argument('--input', type=Path,
        default=Path(__file__).parent / 'examples/scene-relay-20261003/input.json')
    p.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.command == 'prepare': prepare(args.replay, args.trials, args.output)
    else: print(json.dumps(run(args.input, args.output), indent=2))


if __name__ == '__main__': main()
