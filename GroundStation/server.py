"""Live Pi -> laptop -> Quest bridge. No sensor hardware is opened on this host."""
import argparse
import copy
from dataclasses import asdict
import json
import logging
from pathlib import Path
import threading
import time
from urllib.error import URLError
from urllib.parse import urlsplit
from urllib.request import urlopen
import uuid

from flask import Flask, Response, jsonify, render_template, request, send_from_directory
from jinja2 import ChoiceLoader, FileSystemLoader

from .fusion import FusionConfig, FusionEngine, number
from .replay import reject_constant

HERE = Path(__file__).resolve().parent
SENSOR_UI = HERE.parent / "SensorRig" / "CV"


class RelayState:
    def __init__(self, config=None, clock=time.monotonic, recording=None, replay=False, record_pose_frames=False):
        self.lock = threading.RLock()
        self.clock = clock
        self.engine = FusionEngine(config, clock)
        self.packet = None
        self.received_at = 0
        self.session = uuid.uuid4().hex
        self.sequence = 0
        self.error = None
        self.clients = 0
        self.recording = recording
        self.is_replay = replay
        self.record_pose_frames=record_pose_frames
        self.pose_status = dict(status='disabled',error=None)
        self.pose_image = None
        self.pose_image_at = 0
        self.registration_generation=0
        self.rig_motion=dict(status='unobserved',motion_alarm=False,metric_pose_available=False)
        self.quest_rig=None

    def accept_controller_report(self, message):
        """Diagnostic uplink only; it never enables world placement on another client."""
        if (not isinstance(message,dict) or message.get('kind')!='quest_controller_rig'
                or type(message.get('aligned')) is not bool or type(message.get('tracked')) is not bool
                or not isinstance(message.get('status'),str) or len(message['status'])>180):
            return False
        with self.lock:
            self.quest_rig=dict(aligned=message['aligned'],tracked=message['tracked'],
                                status=message['status'],received_at=self.clock())
        return True

    def accept_poses(self, frame, poses, elapsed_ms):
        with self.lock:
            now=self.clock()
            current=self.engine.camera
            valid=(frame['source_session_id']==self.engine.session and current is not None
                   and frame['generation']==current.generation and frame['frame_id']<=current.frame_id
                   and 0<=frame['age_ms']<=350 and current.age(now)<=750)
            accepted=0
            if valid:
                observed={p['id'] for p in current.values}
                for pose in poses:
                    if pose['camera_id'] in observed:
                        accepted+=self.engine.tracker.attach_pose(('C',frame['generation'],pose['camera_id']),pose,now)
            self.pose_status=dict(status='live' if valid else 'stale',error=None,
                source_session_id=frame['source_session_id'],generation=frame['generation'],frame_id=frame['frame_id'],
                capture_ms=frame['capture_ms'],age_ms=frame['age_ms'],received_at=now,
                processing_ms=round(elapsed_ms,1),poses=len(poses),accepted=accepted)
            if self.recording and valid:
                recorded_frame=dict(frame)
                if not self.record_pose_frames:
                    recorded_frame.pop('jpeg_base64',None)
                self.recording.write(json.dumps(dict(received_at=now,kind='pose',frame=recorded_frame,
                    poses=poses,processing_ms=elapsed_ms,rig_motion=self.rig_motion),allow_nan=False)+'\n')
                self.recording.flush()
            return valid

    def publish_pose_image(self, image, poses, frame, accepted):
        if not accepted:
            return
        import cv2
        from .pose import CONNECTIONS
        for pose in poses:
            points=pose['image_points']
            for a,b in CONNECTIONS:
                if min(points[a][2],points[b][2])>=.5:
                    cv2.line(image,tuple(round(x) for x in points[a][:2]),tuple(round(x) for x in points[b][:2]),(140,235,180),2)
        cv2.putText(image,f"POSE FRAME {frame['frame_id']} / {len(poses)} PERSONS",(12,22),cv2.FONT_HERSHEY_SIMPLEX,.5,(255,255,255),1)
        ok,jpeg=cv2.imencode('.jpg',image,[cv2.IMWRITE_JPEG_QUALITY,85])
        if ok:
            with self.lock:
                self.pose_image=jpeg.tobytes()
                self.pose_image_at=self.clock()+max(0,(750-frame['age_ms'])/1000)

    def ingest(self, packet):
        with self.lock:
            now = self.clock()
            self.engine.ingest(packet, now)
            self.packet = copy.deepcopy(packet)
            self.received_at = now
            self.error = None
            if self.recording:
                self.recording.write(json.dumps(dict(received_at=now, packet=packet), allow_nan=False) + "\n")
                self.recording.flush()

    def disconnect(self, message):
        with self.lock:
            self.engine.disconnect()
            self.packet = None
            self.error = message
            self.pose_image = None

    def configure(self, value, config_path):
        if not isinstance(value, dict):
            raise ValueError("Configuration must be an object")
        try:
            config = FusionConfig(**value)
        except TypeError as error:
            raise ValueError(str(error)) from error
        with self.lock:
            config_path.parent.mkdir(parents=True, exist_ok=True)
            temporary = config_path.with_suffix(".tmp")
            temporary.write_text(json.dumps(asdict(config), indent=2) + "\n")
            temporary.replace(config_path)
            self.engine.set_config(config)
            self.registration_generation+=1
            self.rig_motion=dict(status='unobserved',motion_alarm=False,metric_pose_available=False)
            return asdict(config)

    def snapshot(self):
        with self.lock:
            now = self.clock()
            root = copy.deepcopy(self.packet) if self.packet else dict(
                schema_version=1, rig=dict(x=0, y=0, heading_deg=0, tracking_ok=False),
                detections=[], camera_connected=False, camera_people=[],
                radar=dict(status="disconnected", targets=[]), drone_relative_radar_targets=[])
            elapsed = max(0, now - self.received_at) * 1000
            if not isinstance(root.get("rig"), dict):
                root["rig"] = dict(x=0, y=0, heading_deg=0, tracking_ok=False)
            root.setdefault("detections", [])
            root.setdefault("drone_relative_radar_targets", [])
            for obj, field, limit in ((root, "camera_age_ms", 750), (root["radar"], "age_ms", 500)):
                age = obj.get(field)
                obj[field] = age + elapsed if number(age) else None
                if obj[field] is None or obj[field] > limit:
                    if obj is root:
                        root.update(camera_connected=False, camera_people=[], detections=[])
                    else:
                        obj.update(status="stale", targets=[], raw_targets=[])
                        root["drone_relative_radar_targets"] = []
            if elapsed > 1000:
                root["rig"]["tracking_ok"] = False
                root["detections"] = []
            self.sequence += 1
            root.update(spatial_people=self.engine.snapshot(now), relay_session_id=self.session,
                        relay_sequence=self.sequence, is_replay=self.is_replay)
            status=dict(self.pose_status)
            if 'received_at' in status:
                status['age_ms']+=max(0,now-status.pop('received_at'))*1000
                if status['age_ms']>350:
                    status['status']='stale'
            root['pose_pipeline']=status
            root['rig_motion']=dict(self.rig_motion)
            if self.quest_rig:
                report=dict(self.quest_rig)
                report['age_ms']=max(0,now-report.pop('received_at'))*1000
                if report['age_ms']>750:
                    report.update(tracked=False,status='Quest controller report stale')
                root['quest_controller_rig']=report
            if self.registration_generation and root['spatial_people']['reference_id']:
                root['spatial_people']['reference_id']+=f'/registration-{self.registration_generation}'
            if self.rig_motion['motion_alarm']:
                root['spatial_people']['rig_pose_valid']=False
            return root


def run_upstream(state, uri, stop):
    from websockets.sync.client import connect
    from websockets.exceptions import WebSocketException
    while not stop.is_set():
        try:
            with connect(uri, open_timeout=5, close_timeout=1, max_size=1024 * 1024, max_queue=2) as socket:
                last_received = state.clock()
                while not stop.is_set():
                    # Poll for shutdown without reconnecting on a brief Wi-Fi pause.
                    # Snapshot ages still expire contacts at 500/750 ms. A sustained
                    # stall reconnects even if the socket's ping/pong remains alive.
                    try:
                        raw = socket.recv(timeout=1)
                    except TimeoutError:
                        if state.clock() - last_received >= 5:
                            raise TimeoutError("No sensor packets for 5 seconds")
                        continue
                    last_received = state.clock()
                    try:
                        state.ingest(json.loads(raw, parse_constant=reject_constant))
                    except (ValueError, TypeError) as error:
                        with state.lock:
                            state.error = f"Rejected sensor packet: {error}"
                        logging.warning("%s", state.error)
        except (OSError, TimeoutError, WebSocketException, ValueError) as error:
            state.disconnect(str(error))
            stop.wait(1)
    state.disconnect("Ground station stopped")


def run_recording(state, path, stop):
    previous = None
    try:
        with path.open() as records:
            for line in records:
                if not line.strip():
                    continue
                row = json.loads(line, parse_constant=reject_constant)
                if not isinstance(row, dict):
                    raise ValueError("Recording rows must be objects")
                at = row.get("received_at")
                if not number(at) or (previous is not None and at < previous):
                    raise ValueError("Invalid recording timeline")
                if previous is not None and stop.wait(at - previous):
                    return
                previous = at
                if row.get('kind')=='pose':
                    from .pose import validate_frame
                    frame=row['frame'];validate_frame(frame)
                    with state.lock:
                        state.rig_motion=dict(row.get('rig_motion',state.rig_motion))
                    accepted=state.accept_poses(frame,row['poses'],row['processing_ms'])
                    if accepted and frame.get('jpeg_base64'):
                        import base64
                        import cv2
                        import numpy as np
                        image=cv2.imdecode(np.frombuffer(base64.b64decode(frame['jpeg_base64'],validate=True),np.uint8),cv2.IMREAD_COLOR)
                        if image is None or image.shape[:2]!=(frame['height'],frame['width']):
                            raise ValueError('Recorded pose image/metadata mismatch')
                        state.publish_pose_image(image,row['poses'],frame,accepted)
                else:
                    state.ingest(row["packet"])
    except (OSError, ValueError, TypeError, KeyError) as error:
        state.disconnect(f"Replay failed: {error}")
        return
    # Leave the last sample to expire naturally so the last frame can be viewed.
    with state.lock:
        state.error = "Replay finished"


def start_websocket(state, host, port, stop):
    from websockets.sync.server import serve
    from websockets.exceptions import ConnectionClosed

    def handle(socket):
        with state.lock:
            state.clients += 1
        try:
            while not stop.is_set():
                socket.send(json.dumps(state.snapshot(), allow_nan=False))
                # Bounded nonblocking reads preserve telemetry cadence even if
                # a client sends malformed or excessive diagnostic messages.
                for _ in range(4):
                    try:
                        incoming=socket.recv(timeout=0)
                    except TimeoutError:
                        break
                    try:
                        state.accept_controller_report(json.loads(incoming,parse_constant=reject_constant))
                    except (ValueError,TypeError,UnicodeError):
                        pass
                stop.wait(.1)
        except ConnectionClosed:
            pass
        finally:
            with state.lock:
                state.clients -= 1

    server = serve(handle, host, port, max_size=65536, max_queue=2, close_timeout=1)
    thread = threading.Thread(target=server.serve_forever, daemon=True, name="quest-relay")
    thread.start()
    return server, thread


def create_app(state, pi_http, websocket_port=8765, config_path=Path("Saved/GroundStation/fusion.json")):
    app = Flask(__name__, template_folder=str(SENSOR_UI), static_folder=None)
    app.jinja_loader = ChoiceLoader([FileSystemLoader(str(HERE / "templates")), app.jinja_loader])

    @app.after_request
    def no_cache(response):
        response.headers["Cache-Control"] = "no-store"
        return response

    @app.get("/")
    @app.get("/quest")
    def index():
        return render_template("dashboard.html", quest_mode=request.path == "/quest", ground_station=True)

    @app.get("/assets/<path:name>")
    def assets(name):
        directory = HERE / "static" if name in ("ground.js", "ground.css") else SENSOR_UI / "static"
        return send_from_directory(directory, name)

    @app.get("/fusion.json")
    def fusion():
        with state.lock:
            return jsonify(spatial_people=state.snapshot()["spatial_people"], error=state.error,
                           clients=state.clients, is_replay=state.is_replay,
                           config=asdict(state.engine.config))

    @app.post("/fusion/config")
    def config():
        # Same-origin dashboard controls only; this endpoint never configures Pi hardware.
        if request.headers.get("Origin") not in (None, request.host_url.rstrip("/")):
            return jsonify(error="Use the ground-station dashboard to configure alignment"), 403
        try:
            return jsonify(state.configure(request.get_json(), config_path))
        except (ValueError, OSError) as error:
            return jsonify(error=str(error)), 400

    @app.get("/diagnostics.json")
    def diagnostics():
        # One age-corrected packet keeps raw observations, associations and
        # graphs on the same sample. Reading never renews sensor freshness.
        with state.lock:
            packet = state.snapshot()
            return jsonify(packet=packet, error=state.error, clients=state.clients,
                           is_replay=state.is_replay, config=asdict(state.engine.config),
                           received_age_ms=round(max(0, state.clock()-state.received_at)*1000, 3)
                           if state.packet else None)

    @app.get("/handoff.json")
    def handoff():
        host = urlsplit(request.host_url).hostname
        if ":" in host:
            host = f"[{host}]"
        return jsonify(schema_version=1, quest_url=request.host_url + "quest",
                       websocket_url=f"ws://{host}:{websocket_port}/", native_mode="WallhackSensorPeople",
                       camera_radar_association="ground_station", headset_validation="requires_physical_check")

    @app.get('/pose/snapshot.jpg')
    def pose_image():
        with state.lock:
            if state.pose_image is None or state.clock()>state.pose_image_at:
                return jsonify(error='No fresh pose image'),503
            return Response(state.pose_image,mimetype='image/jpeg')

    @app.get("/stream")
    @app.get("/snapshot.jpg")
    @app.get("/detections")
    @app.get("/radar/targets")
    @app.get("/radar/trials")
    def proxy():
        if not pi_http:
            return jsonify(error="Video and raw dashboard unavailable during offline replay"), 503
        try:
            upstream = urlopen(pi_http.rstrip("/") + request.path, timeout=2)
        except (OSError, URLError) as error:
            return jsonify(error=str(error)), 502
        content_type = upstream.headers.get("Content-Type", "application/octet-stream")
        if request.path == "/detections":
            with upstream:
                raw = upstream.read(1024 * 1024)
            try:
                result = json.loads(raw, parse_constant=reject_constant)
            except (ValueError, TypeError):
                return jsonify(error="Invalid sensor dashboard response"), 502
            with state.lock:
                result["quest"] = dict(status="listening", clients=state.clients,
                                       pose_status="Set sensor origin and heading in native Quest app")
            return jsonify(result)
        def chunks():
            try:
                while True:
                    chunk = upstream.read1(65536)
                    if not chunk:
                        return
                    yield chunk
            finally:
                upstream.close()
        return Response(chunks(), content_type=content_type)

    return app


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--pi-http", help="Pi dashboard URL, e.g. http://larp-pi.local:8766")
    source.add_argument("--replay", type=Path, help="Replay recorded input to Quest, explicitly labelled")
    parser.add_argument("--pi-websocket", help="Default: ws://<Pi dashboard host>:8765/")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8766)
    parser.add_argument("--quest-port", type=int, default=8765)
    parser.add_argument("--config", type=Path, default=Path("Saved/GroundStation/fusion.json"))
    parser.add_argument("--record", type=Path, help="Create a new input JSONL recording; never overwrite")
    parser.add_argument('--pose-model',type=Path,help='Enable laptop pose estimation with a MediaPipe .task model')
    parser.add_argument('--record-pose-frames',action='store_true',help='Include exact camera JPEGs in --record for pose-overlay replay')
    args = parser.parse_args(argv)
    if args.pose_model and (not args.pi_http or not args.pose_model.is_file()):
        parser.error('--pose-model requires a live Pi and an existing .task model')
    if args.record_pose_frames and not args.record:
        parser.error('--record-pose-frames requires --record')
    if args.pi_http:
        parsed = urlsplit(args.pi_http)
        if parsed.scheme not in ("http", "https") or not parsed.hostname:
            parser.error("--pi-http must be an HTTP(S) URL")
    config = FusionConfig(**json.loads(args.config.read_text(), parse_constant=reject_constant)) if args.config.exists() else FusionConfig()
    recording = args.record.open("x") if args.record else None
    state = RelayState(config, recording=recording, replay=bool(args.replay),record_pose_frames=args.record_pose_frames)
    stop = threading.Event()
    server, socket_thread = start_websocket(state, args.host, args.quest_port, stop)
    if args.replay:
        worker_args = (state, args.replay, stop)
        target = run_recording
    else:
        host = f"[{parsed.hostname}]" if ":" in parsed.hostname else parsed.hostname
        worker_args = (state, args.pi_websocket or f"ws://{host}:8765/", stop)
        target = run_upstream
    worker = threading.Thread(target=target, args=worker_args, daemon=True, name="pi-input")
    worker.start()
    pose_worker=None
    if args.pose_model:
        from .pose import run_pose
        pose_worker=threading.Thread(target=run_pose,args=(state,args.pi_http,args.pose_model,stop),daemon=True,name='camera-pose')
        pose_worker.start()
    try:
        create_app(state, args.pi_http, args.quest_port, args.config).run(host=args.host, port=args.port, threaded=True, use_reloader=False)
    finally:
        stop.set()
        server.shutdown()
        worker.join(timeout=7)
        socket_thread.join(timeout=2)
        if pose_worker:
            pose_worker.join(timeout=3)
        if recording:
            recording.close()


if __name__ == "__main__":
    main()
