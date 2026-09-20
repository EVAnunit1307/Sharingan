"""Optional Quest/ground-station WebSocket handoff using the existing rig schema.

No world contacts without fresh IMU pose or an explicit stationary bench mode.
Radar measurements remain separate until camera/radar alignment is calibrated.
"""
import json
import math
import threading
import time


def valid_pose(message):
    rig = message.get('rig') if isinstance(message, dict) else None
    if not isinstance(rig, dict) or rig.get('tracking_ok') is not True:
        return None
    for key in ('x', 'y', 'heading_deg'):
        value = rig.get(key)
        if isinstance(value, bool) or not isinstance(value, (float, int)) or not math.isfinite(value):
            return None
    return {key: float(rig[key]) for key in ('x','y','heading_deg')}


def make_packet(snapshot, rig, pose_status):
    valid = rig is not None
    rig = rig or {'x': 0., 'y': 0., 'heading_deg': 0.}
    angle = math.radians(rig['heading_deg'])
    detections = []
    if valid and snapshot['camera_connected']:
        for person in snapshot['people']:
            if not person['observed'] or person['x_m'] is None or person['y_m'] is None:
                continue
            x, y = person['x_m'], person['y_m']
            detections.append(dict(id=person['id'],
                x=rig['x']+x*math.cos(angle)+y*math.sin(angle),
                y=rig['y']-x*math.sin(angle)+y*math.cos(angle),
                conf=person['score'], source='camera', range_source=person['range_source']))
    return {'schema_version': 1, 't': time.time(),
            'source_session_id': snapshot.get('source_session_id'),
            'camera_generation': snapshot.get('camera_generation', 0),
            'camera_capture_mono_ms': snapshot.get('camera_capture_mono_ms'),
            'camera_frame_width': snapshot.get('frame_width'),
            'camera_frame_height': snapshot.get('frame_height'),
            'camera_hfov_deg': snapshot.get('camera_hfov_deg'),
            'rig': dict(rig, tracking_ok=valid, tracking_note=pose_status),
            'detections': detections, 'camera_connected': snapshot['camera_connected'],
            'camera_frame_id': snapshot['frame_id'], 'camera_timestamp': snapshot['timestamp'],
            'camera_age_ms': snapshot.get('frame_age_ms'),
            'stale_after_ms': {'camera': 750, 'radar': 500, 'rig_pose': 1000},
            'camera_people': snapshot['people'], 'radar': snapshot['radar'],
            # Radar must remain available behind an opaque wall even with no
            # camera observation or world pose. No invented person confidence.
            'drone_relative_radar_targets': [
                {'id': t['id'], 'right_m': t['drone_position_m']['right'],
                 'forward_m': t['drone_position_m']['forward'],
                 'source': 'ld2450', 'classification': 'unverified_radar_target',
                 'reference_origin': snapshot['radar'].get('reference_origin'),
                 'confidence': None, 'timestamp': snapshot['radar'].get('timestamp')}
                for t in snapshot['radar'].get('targets', [])
                if snapshot['radar'].get('status') == 'live' and t.get('drone_position_m') is not None]}


class QuestBridge:
    def __init__(self, pipeline, host='0.0.0.0', port=8765, stationary=False):
        self.pipeline, self.host, self.port = pipeline, host, port
        self.stationary = stationary
        self.lock = threading.Lock()
        self.rig = None
        self.pose_at = None
        self.clients = 0
        self.server = None
        self.worker = None
        self.stop_event = threading.Event()
        self.connections = set()

    def pose(self):
        with self.lock:
            if self.pose_at is not None and time.monotonic()-self.pose_at <= 1.0:
                return dict(self.rig), 'live rig pose'
        if self.stationary:
            return {'x': 0., 'y': 0., 'heading_deg': 0.}, 'stationary sensor frame'
        return None, 'awaiting rig pose'

    def status(self):
        _, label = self.pose()
        with self.lock:
            return {'status': 'listening' if self.server else 'disabled',
                    'clients': self.clients, 'port': self.port, 'pose_status': label}

    def accept_pose(self, message):
        pose = valid_pose(message)
        # A tracking failure from the pose sender invalidates its previous fix.
        if isinstance(message, dict) and 'rig' in message:
            with self.lock:
                self.rig = pose
                self.pose_at = time.monotonic() if pose is not None else None

    def handle(self, socket):
        from websockets.exceptions import ConnectionClosed
        with self.lock:
            self.clients += 1
            self.connections.add(socket)
        try:
            while not self.stop_event.is_set():
                try:
                    data = socket.recv(timeout=.1)
                    try:
                        self.accept_pose(json.loads(data))
                    except (ValueError, TypeError):
                        pass
                except TimeoutError:
                    pass
                rig, label = self.pose()
                socket.send(json.dumps(make_packet(self.pipeline.snapshot(), rig, label), allow_nan=False))
        except ConnectionClosed:
            pass
        finally:
            with self.lock:
                self.clients -= 1
                self.connections.discard(socket)

    def start(self):
        from websockets.sync.server import serve
        # Bind before claiming readiness; surface an occupied port at startup.
        self.stop_event.clear()
        self.server = serve(self.handle, self.host, self.port, max_size=65536, max_queue=4, close_timeout=1)
        self.worker = threading.Thread(target=self.server.serve_forever, daemon=True, name='quest-bridge')
        self.worker.start()

    def stop(self):
        self.stop_event.set()
        if self.server:
            self.server.shutdown()
        # shutdown() closes the listener, not established connections. Their
        # non-daemon receive threads otherwise keep the Pi process alive after
        # SIGTERM, especially when the relay is still attached during restart.
        with self.lock:
            connections = tuple(self.connections)
        for socket in connections:
            socket.close(code=1001, reason='Sensor bridge stopping')
        if self.worker:
            self.worker.join(timeout=2)
        self.server = None
