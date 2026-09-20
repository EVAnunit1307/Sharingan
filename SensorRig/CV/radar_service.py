"""LD2450 tracking and explicit sensor-to-drone 2D coordinates."""
from collections import deque
import json
import math
from pathlib import Path
import struct
import threading
import time

HEADER = b'\xaa\xff\x03\x00'
FOOTER = b'\x55\xcc'
FRAME_LEN = 30
CONFIG_PATH = Path(__file__).with_name('radar_config.json')
DEFAULT_CONFIG = dict(invert_x=False, yaw_deg=0., offset_right_m=0., offset_forward_m=0.,
                      mount_level=False, mounting_confirmed=False,
                      min_range_m=.1, max_range_m=6.)


def validate_config(config):
    if not isinstance(config, dict) or set(config) != set(DEFAULT_CONFIG):
        raise ValueError('Radar configuration must contain exactly the documented fields')
    for key in ('invert_x', 'mount_level', 'mounting_confirmed'):
        if type(config[key]) is not bool:
            raise ValueError(f'{key} must be true or false')
    for key in ('yaw_deg', 'offset_right_m', 'offset_forward_m', 'min_range_m', 'max_range_m'):
        if type(config[key]) not in (int, float) or not math.isfinite(config[key]):
            raise ValueError(f'{key} must be a finite number')
    if not -180 <= config['yaw_deg'] <= 180:
        raise ValueError('Yaw must be between -180 and 180 degrees')
    if any(abs(config[k]) > 2 for k in ('offset_right_m', 'offset_forward_m')):
        raise ValueError('Mount offsets must be within 2 metres of the drone origin')
    if not .05 <= config['min_range_m'] < config['max_range_m'] <= 6:
        raise ValueError('Require 0.05 <= minimum range < maximum range <= 6 metres')
    if config['mounting_confirmed'] and not config['mount_level']:
        raise ValueError('Horizontal output requires a level, upright or upside-down mounting')
    return dict(config)


def sensor_to_drone(x_m, y_m, config):
    """Clockwise mount yaw, +right/+forward; height/elevation is unobserved."""
    if config['invert_x']:
        x_m = -x_m
    angle = math.radians(config['yaw_deg'])
    return (config['offset_right_m'] + x_m*math.cos(angle) + y_m*math.sin(angle),
            config['offset_forward_m'] - x_m*math.sin(angle) + y_m*math.cos(angle))


def signed(raw):
    magnitude = raw & 0x7fff
    return magnitude if raw & 0x8000 else -magnitude


def decode_frame(frame, invert_x=False):
    if len(frame) != FRAME_LEN or frame[:4] != HEADER or frame[-2:] != FOOTER:
        raise ValueError('Invalid LD2450 frame')
    detections = []
    for i in range(3):
        x, y, speed, resolution = struct.unpack_from('<HHHH', frame, 4 + 8*i)
        x, y, speed = signed(x), signed(y), signed(speed)
        if invert_x:
            x = -x
        # Range is radial, not a +/-3m rectangle. A real target near the
        # outer edge of the 120-degree beam can be >3m to either side.
        if y >= 0 and .05 <= math.hypot(x, y)/1000 <= 6:
            detections.append(dict(x=x, y=y, spd=speed, slot=i,
                                   resolution_mm=resolution))
    return detections


class FrameParser:
    def __init__(self):
        self.buffer = bytearray()

    def feed(self, chunk):
        self.buffer.extend(chunk)
        frames = []
        while True:
            index = self.buffer.find(HEADER)
            if index < 0:
                self.buffer[:] = self.buffer[-3:]
                break
            if index:
                del self.buffer[:index]
            if len(self.buffer) < FRAME_LEN:
                break
            if self.buffer[28:30] != FOOTER:
                del self.buffer[0]
                continue
            frames.append(bytes(self.buffer[:FRAME_LEN]))
            del self.buffer[:FRAME_LEN]
        return frames


class RadarTracker:
    def __init__(self):
        self.tracks = []
        self.next_id = 1

    def update(self, detections, now):
        self.tracks = [t for t in self.tracks if now-t['seen'] <= .4]
        pairs = sorted((math.hypot(t['x']-d['x'], t['y']-d['y']), ti, di)
                       for ti, t in enumerate(self.tracks) for di, d in enumerate(detections))
        matches, used = {}, set()
        for distance, ti, di in pairs:
            if distance <= 450 and ti not in matches and di not in used:
                matches[ti] = di
                used.add(di)
        for ti, track in enumerate(self.tracks):
            track['observed'] = ti in matches
            if ti in matches:
                d = detections[matches[ti]]
                for key in ('x', 'y', 'spd'):
                    track[key] = .5*d[key] + .5*track[key]
                track['raw'] = d
                track['seen'] = now
                track['hits'] += 1
                track['confirmed'] |= track['hits'] >= 3
            else:
                track['hits'] = 0
        for di, d in enumerate(detections):
            if di not in used:
                self.tracks.append(dict(d, id=self.next_id, seen=now, first_seen=now,
                                        hits=1, confirmed=False, observed=True, raw=d))
                self.next_id += 1
        result = []
        for t in self.tracks:
            if t['confirmed'] and t['observed']:
                result.append(dict(id=t['id'], x=round(t['x']), y=round(t['y']),
                    spd=round(t['spd']), raw_x=t['raw']['x'], raw_y=t['raw']['y'],
                    resolution_mm=t['raw'].get('resolution_mm', 0), slot=t['raw'].get('slot'),
                    track_age_s=round(now-t['first_seen'], 2), observed=True))
        return result


class RadarService:
    def __init__(self, port='/dev/serial0', invert_x=None, config_path=CONFIG_PATH):
        self.port = port
        self.config_path = Path(config_path)
        self.config = validate_config(json.loads(self.config_path.read_text()) if self.config_path.exists()
                                      else DEFAULT_CONFIG)
        if invert_x is not None:
            self.config['invert_x'] = invert_x
        self.lock = threading.Lock()
        self.stop_event = threading.Event()
        self.targets, self.raw_targets = [], []
        self.last_frame = None
        self.frame_timestamp = None
        self.frame_id = 0
        self.frame_times = deque(maxlen=30)
        self.diagnostics = {}
        self.error = None
        self.worker = None

    def set_config(self, config):
        config = validate_config(config)
        # Atomic replacement preserves the last usable calibration on failure.
        with self.lock:
            temporary = self.config_path.with_suffix('.json.tmp')
            temporary.write_text(json.dumps(config, indent=2)+'\n')
            temporary.replace(self.config_path)
            self.config = config
        return config

    def start(self):
        self.worker = threading.Thread(target=self.run, daemon=True, name='radar')
        self.worker.start()

    def stop(self):
        self.stop_event.set()
        if self.worker:
            self.worker.join(timeout=3)

    def ingest(self, frame, tracker, now=None):
        now = time.monotonic() if now is None else now
        raw = decode_frame(frame)
        with self.lock:
            config = dict(self.config)
        detections = [d for d in raw if config['min_range_m'] <= math.hypot(d['x'], d['y'])/1000 <= config['max_range_m']]
        targets = tracker.update(detections, now)
        with self.lock:
            self.targets, self.raw_targets = targets, raw
            self.last_frame, self.frame_timestamp, self.error = now, time.time(), None
            self.frame_id += 1
            self.frame_times.append(now)

    def run(self):
        import serial
        from radar_protocol import read_diagnostics
        while not self.stop_event.is_set():
            try:
                with serial.Serial(self.port, 256000, timeout=.1, write_timeout=.5, exclusive=True) as uart:
                    diagnostics = read_diagnostics(uart)
                    with self.lock:
                        self.diagnostics = diagnostics
                    parser, tracker = FrameParser(), RadarTracker()
                    while not self.stop_event.is_set():
                        chunk = uart.read(max(1, min(uart.in_waiting, 4096)))
                        # If buffered packets arrive together, publish the newest
                        # one; a backlog must not count as fresh confirmation.
                        frames = parser.feed(chunk)
                        if frames:
                            self.ingest(frames[-1], tracker)
                        if self.last_frame is not None and time.monotonic()-self.last_frame > .5:
                            tracker.tracks.clear()
            except Exception as exc:
                with self.lock:
                    self.error, self.targets, self.raw_targets, self.last_frame = str(exc), [], [], None
                    self.frame_times.clear()
                self.stop_event.wait(1)

    def snapshot(self):
        with self.lock:
            age = time.monotonic()-self.last_frame if self.last_frame is not None else None
            live = age is not None and age <= .5 and not self.error
            targets = list(self.targets) if live else []
            raw = list(self.raw_targets) if live else []
            config = dict(self.config)
            times = list(self.frame_times)
            state = dict(status='live' if live else 'disconnected',
                         age_ms=round(age*1000, 1) if age is not None else None,
                         timestamp=self.frame_timestamp, frame_id=self.frame_id,
                         error=self.error, diagnostics=dict(self.diagnostics))
        calibrated = config['mounting_confirmed'] and config['mount_level']
        output = []
        for t in targets:
            if not config['min_range_m'] <= math.hypot(t['raw_x'], t['raw_y'])/1000 <= config['max_range_m']:
                continue
            sx, sy = t['x']/1000, t['y']/1000
            right, forward = sensor_to_drone(sx, sy, config)
            raw_right, raw_forward = sensor_to_drone(t['raw_x']/1000, t['raw_y']/1000, config)
            # Keep x/y compatibility in mm in the displayed frame.
            output.append(dict(t, x=round(right*1000), y=round(forward*1000),
                right_m=round(right, 3), forward_m=round(forward, 3),
                raw_right_m=round(raw_right, 3), raw_forward_m=round(raw_forward, 3),
                sensor_x_m=sx, sensor_y_m=sy,
                range_m=round(math.hypot(right, forward), 3),
                bearing_deg=round(math.degrees(math.atan2(right, forward)), 2),
                radial_speed_mps=t['spd']/100,
                drone_position_m={'right': round(right, 3), 'forward': round(forward, 3)} if calibrated else None,
                classification='unverified_radar_target', through_wall_verified=False))
        state.update(targets=output, raw_targets=raw, config=config,
            update_hz=round((len(times)-1)/(times[-1]-times[0]), 1) if live and len(times)>1 and times[-1]>times[0] else 0,
            units='mm', position_units='m', invert_x=config['invert_x'],
            coordinate_frame='drone_body_2d' if calibrated else 'unverified_mount_2d',
            axes={'right_m': 'positive toward drone right', 'forward_m': 'positive toward drone nose'},
            horizontal_projection_enabled=calibrated, camera_alignment='unverified',
            reference_origin='radar' if config['offset_right_m'] == config['offset_forward_m'] == 0 else 'configured_drone_reference',
            position_accuracy_verified=False, elevation_measured=False,
            through_wall_status='not_tested', motion_compensation=False,
            confidence_available=False)
        return state
