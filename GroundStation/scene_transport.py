"""Transport-independent receiver for recorded, arbitrary-scale scene drafts.

No sockets, live sensor integration or flight commands. A future transport must
establish session identity and clock mapping explicitly before using this state.
SHA-256 checks assembly integrity; it is not sender authentication.
"""
import base64
import hashlib
import json
import math


SCHEMA = 'wallhack.recorded_scene.v1'
MAX_BYTES = 1_000_000
CHUNK_BYTES = 1024
MAX_CHUNKS = (MAX_BYTES + CHUNK_BYTES - 1) // CHUNK_BYTES


def encode(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), allow_nan=False).encode()


def integer(value, minimum=0, maximum=2**53 - 1):
    return type(value) is int and minimum <= value <= maximum


def vector(value, length):
    return (isinstance(value, list) and len(value) == length
            and all(type(v) in (float, int) and -1e9 <= v <= 1e9 and math.isfinite(v) for v in value))


def validate_scene(value, identity, revision):
    if not isinstance(value, dict) or any(value.get(k) != v for k, v in identity.items()):
        raise ValueError('Scene identity mismatch')
    if (not integer(value.get('revision'), 1) or value.get('revision') != revision or value.get('schema') != SCHEMA
            or value.get('live') is not False or value.get('mapping_eligible') is not False
            or value.get('units') != 'arbitrary' or not integer(value.get('observed_ns'))):
        raise ValueError('Invalid recorded-scene metadata')
    points, poses = value.get('points'), value.get('poses')
    if not isinstance(points, list) or not 1 <= len(points) <= 10000:
        raise ValueError('Invalid point count')
    if any(not vector(p, 6) or any(not integer(c, 0, 255) for c in p[3:]) for p in points):
        raise ValueError('Invalid point geometry or colour')
    if not isinstance(poses, list) or not 1 <= len(poses) <= 1000:
        raise ValueError('Invalid camera path')
    previous = -1
    for pose in poses:
        if (not isinstance(pose, dict) or not vector(pose.get('position'), 3)
                or not integer(pose.get('observed_ns'))
                or not previous < pose['observed_ns'] <= value['observed_ns']):
            raise ValueError('Invalid path timing or geometry')
        previous = pose['observed_ns']
    if previous != value['observed_ns']:
        raise ValueError('Scene timestamp must match last observation')


def chunk_scene(scene):
    identity = {k: scene[k] for k in ('session_id', 'map_id', 'frame_id', 'clock_id')}
    validate_scene(scene, identity, scene['revision'])
    if not integer(scene['revision'], 1):
        raise ValueError('Invalid revision')
    payload = encode(scene)
    if len(payload) > MAX_BYTES:
        raise ValueError('Scene exceeds bounded snapshot size')
    checksum = hashlib.sha256(payload).hexdigest()
    pieces = [payload[i:i + CHUNK_BYTES] for i in range(0, len(payload), CHUNK_BYTES)]
    return [dict(schema=SCHEMA, kind='chunk', **identity, revision=scene['revision'],
                 count=len(pieces), index=i, total_bytes=len(payload), sha256=checksum,
                 payload=base64.b64encode(part).decode()) for i, part in enumerate(pieces)]


class SceneReceiver:
    """One explicit session/frame; newest complete map survives loss/reordering.

    Clock offset is receiver-time minus source-time in ns. None means the clock
    relationship is unknown, so no camera marker can be considered fresh.
    Timeouts use receiver time; pose age uses mapped observation time, never
    packet receipt time. A new session requires a new receiver (explicit reset).
    """
    def __init__(self, *, session_id, map_id, frame_id, clock_id,
                 clock_offset_ns=None, pose_ttl_ns=2_000_000_000,
                 assembly_ttl_ns=20_000_000_000):
        self.identity = dict(session_id=session_id, map_id=map_id, frame_id=frame_id, clock_id=clock_id)
        if any(not isinstance(v, str) or not 1 <= len(v) <= 200 for v in self.identity.values()):
            raise ValueError('Invalid session identity')
        if clock_offset_ns is not None and (type(clock_offset_ns) is not int or abs(clock_offset_ns) >= 2**53):
            raise ValueError('Invalid clock offset')
        if not integer(pose_ttl_ns, 1) or not integer(assembly_ttl_ns, 1):
            raise ValueError('Invalid timeout')
        self.clock_offset_ns = clock_offset_ns
        self.pose_ttl_ns = pose_ttl_ns
        self.assembly_ttl_ns = assembly_ttl_ns
        self.scene = None
        self.pending = None
        self.highest_started_revision = 0
        self.camera = None
        self.camera_sequence = -1
        self.heartbeat_sequence = -1
        self.advertised_revision = 0
        self.last_heartbeat_ns = None
        self.now_ns = -1

    def advance(self, now_ns):
        if not integer(now_ns) or now_ns < self.now_ns:
            raise ValueError('Receiver time must be monotonic integer ns')
        self.now_ns = now_ns
        if self.pending and now_ns - self.pending['started_ns'] > self.assembly_ttl_ns:
            self.pending = None

    def receive(self, packet, now_ns):
        self.advance(now_ns)
        if (not isinstance(packet, dict) or packet.get('schema') != SCHEMA
                or any(packet.get(k) != v for k, v in self.identity.items())):
            return 'identity_rejected'
        kind = packet.get('kind')
        if kind == 'heartbeat':
            sequence, observed = packet.get('sequence'), packet.get('observed_ns')
            if (not integer(sequence) or not integer(observed)
                    or not integer(packet.get('revision'))): return 'invalid_heartbeat'
            if self.clock_offset_ns is None: return 'unknown_clock'
            age = now_ns - observed - self.clock_offset_ns
            if not 0 <= age <= 2_000_000_000: return 'stale_heartbeat'
            if sequence <= self.heartbeat_sequence: return 'old_heartbeat'
            self.heartbeat_sequence = sequence
            self.advertised_revision = max(self.advertised_revision, packet['revision'])
            self.last_heartbeat_ns = now_ns
            return 'heartbeat'
        if kind == 'camera':
            seq, stamp, revision = packet.get('sequence'), packet.get('observed_ns'), packet.get('revision')
            if (not integer(seq) or not integer(stamp) or not integer(revision, 1)
                    or not vector(packet.get('position'), 3) or type(packet.get('tracking')) is not bool):
                return 'invalid_camera'
            if seq <= self.camera_sequence: return 'old_camera'
            if self.clock_offset_ns is not None and stamp + self.clock_offset_ns > now_ns:
                return 'future_camera'
            if self.camera and stamp < self.camera['observed_ns']: return 'old_camera'
            self.camera_sequence = seq
            self.camera = dict(packet)
            return 'camera'
        if kind != 'chunk': return 'unknown_kind'
        rev, count, index, size = (packet.get(k) for k in ('revision', 'count', 'index', 'total_bytes'))
        digest = packet.get('sha256')
        if (not integer(rev, 1) or not integer(size, 1, MAX_BYTES)
                or not integer(count, 1, MAX_CHUNKS) or count != (size + CHUNK_BYTES - 1) // CHUNK_BYTES
                or not integer(index, 0, count - 1) or not isinstance(digest, str)
                or len(digest) != 64 or any(c not in '0123456789abcdef' for c in digest)):
            return 'invalid_chunk'
        encoded = packet.get('payload')
        if not isinstance(encoded, str) or len(encoded) > 4 * ((CHUNK_BYTES + 2) // 3):
            return 'invalid_chunk'
        try: part = base64.b64decode(encoded, validate=True)
        except (ValueError, base64.binascii.Error): return 'invalid_chunk'
        expected = CHUNK_BYTES if index < count - 1 else size - CHUNK_BYTES * (count - 1)
        if len(part) != expected: return 'invalid_chunk'
        installed = self.scene['revision'] if self.scene else 0
        if rev <= installed or rev < self.highest_started_revision: return 'old_revision'
        if self.pending is None or rev > self.pending['revision']:
            self.highest_started_revision = rev
            self.pending = dict(revision=rev, count=count, total_bytes=size, sha256=digest,
                                started_ns=now_ns, parts={})
        assembly = self.pending
        if any(assembly[k] != packet[k] for k in ('revision', 'count', 'total_bytes', 'sha256')):
            return 'conflicting_chunk'
        if index in assembly['parts']:
            return 'duplicate_chunk' if assembly['parts'][index] == part else 'conflicting_chunk'
        assembly['parts'][index] = part
        if len(assembly['parts']) < count: return 'partial'
        payload = b''.join(assembly['parts'][i] for i in range(count))
        self.pending = None
        if hashlib.sha256(payload).hexdigest() != digest: return 'checksum_rejected'
        try:
            scene = json.loads(payload)
            validate_scene(scene, self.identity, rev)
        except (ValueError, TypeError, KeyError): return 'invalid_scene'
        self.scene = scene
        return 'installed'

    def missing(self):
        if self.pending is None: return []
        return [i for i in range(self.pending['count']) if i not in self.pending['parts']]

    def state(self, now_ns):
        self.advance(now_ns)
        camera = self.camera
        age = None if camera is None or self.clock_offset_ns is None else now_ns - camera['observed_ns'] - self.clock_offset_ns
        fresh = bool(camera and camera['tracking'] and age is not None and 0 <= age <= self.pose_ttl_ns
                     and self.scene and camera['revision'] <= self.scene['revision'])
        return dict(revision=self.scene['revision'] if self.scene else 0,
                    buffered_chunks=len(self.pending['parts']) if self.pending else 0,
                    missing_chunks=len(self.missing()),
                    link_recent=self.last_heartbeat_ns is not None and now_ns - self.last_heartbeat_ns <= 2_000_000_000,
                    camera_age_seconds=None if age is None else age / 1e9,
                    current_camera=camera['position'] if fresh else None,
                    camera_state=('recent_recorded_estimate' if fresh else 'withheld'),
                    live=False, mapping_eligible=False)
