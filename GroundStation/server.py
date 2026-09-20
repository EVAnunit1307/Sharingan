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
    def __init__(self, config=None, clock=time.monotonic, recording=None, replay=False):
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
            return root


def run_upstream(state, uri, stop):
    from websockets.sync.client import connect
    from websockets.exceptions import WebSocketException
    while not stop.is_set():
        try:
            with connect(uri, open_timeout=5, close_timeout=1, max_size=1024 * 1024, max_queue=2) as socket:
                while not stop.is_set():
                    # A stalled connection cannot prevent shutdown or stale-data cleanup.
                    raw = socket.recv(timeout=1)
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
        directory = HERE / "static" if name == "ground.js" else SENSOR_UI / "static"
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

    @app.get("/handoff.json")
    def handoff():
        host = urlsplit(request.host_url).hostname
        if ":" in host:
            host = f"[{host}]"
        return jsonify(schema_version=1, quest_url=request.host_url + "quest",
                       websocket_url=f"ws://{host}:{websocket_port}/", native_mode="WallhackSensorPeople",
                       camera_radar_association="ground_station", headset_validation="requires_physical_check")

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
    args = parser.parse_args(argv)
    if args.pi_http:
        parsed = urlsplit(args.pi_http)
        if parsed.scheme not in ("http", "https") or not parsed.hostname:
            parser.error("--pi-http must be an HTTP(S) URL")
    config = FusionConfig(**json.loads(args.config.read_text(), parse_constant=reject_constant)) if args.config.exists() else FusionConfig()
    recording = args.record.open("x") if args.record else None
    state = RelayState(config, recording=recording, replay=bool(args.replay))
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
    try:
        create_app(state, args.pi_http, args.quest_port, args.config).run(host=args.host, port=args.port, threaded=True, use_reloader=False)
    finally:
        stop.set()
        server.shutdown()
        worker.join(timeout=7)
        socket_thread.join(timeout=2)
        if recording:
            recording.close()


if __name__ == "__main__":
    main()
