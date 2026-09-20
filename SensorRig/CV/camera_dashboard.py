#!/usr/bin/env python3
"""Camera-first person detection. No camera or worker is opened on import."""
import argparse
import base64
import logging
import math
from pathlib import Path
import signal
import threading
import time
import uuid

import cv2
import numpy as np
from flask import Flask, Response, jsonify, render_template, request
from urllib.parse import urlsplit

from detector.person_detector import PersonDetector, CONFIG_PATH

HERE = Path(__file__).resolve().parent


def camera_position(box, width, height, hfov=62.0, person_height=1.65):
    """Bearing is usable on partial bodies; height-based depth is not.

    A full, standing body is still an assumption, so even an unclipped
    estimate is tagged monocular_estimate and never called measured range.
    """
    x1, y1, x2, y2 = box
    focal = width / (2 * math.tan(math.radians(hfov / 2)))
    slope = ((x1 + x2) / 2 - width / 2) / focal
    result = {"bearing_deg": round(math.degrees(math.atan(slope)), 2),
              "range_source": "unavailable", "x_m": None, "y_m": None}
    if y1 > 3 and y2 < height - 3 and x1 > 3 and x2 < width - 3 and y2 > y1:
        # Similar triangles estimate forward depth (not radial range).
        forward = person_height * focal / (y2 - y1)
        result.update(x_m=round(forward * slope, 3), y_m=round(forward, 3),
                      range_source="monocular_estimate")
    return result


def draw_detections(frame, people, raw):
    image = frame.copy()
    def label(text, x, y, color):
        (width, height), baseline = cv2.getTextSize(text, cv2.FONT_HERSHEY_SIMPLEX, .48, 1)
        x = max(3, min(x, image.shape[1] - width - 5))
        y = max(height + 7, min(y, image.shape[0] - baseline - 4))
        cv2.rectangle(image, (x - 3, y - height - 5), (x + width + 3, y + baseline + 2),
                      (20, 25, 22), -1)
        cv2.putText(image, text, (x, y), cv2.FONT_HERSHEY_SIMPLEX, .48, color, 1, cv2.LINE_AA)

    observed_boxes = {tuple(p["box"]) for p in people if p["observed"]}
    for d in raw:
        if tuple(d["box"]) in observed_boxes:
            continue
        x1, y1, x2, y2 = map(int, d["box"])
        cv2.rectangle(image, (x1, y1), (x2, y2), (50, 180, 230), 1)
        label(f"candidate {d['score']:.0%}", x1, y1 - 7, (50, 180, 230))
    for p in people:
        x1, y1, x2, y2 = map(int, p["box"])
        color = (110, 230, 100) if p["observed"] else (170, 170, 170)
        text = f"PERSON {p['id']} | {p['score']:.0%}" if p["observed"] else f"TRACK {p['id']} | briefly lost"
        cv2.rectangle(image, (x1, y1), (x2, y2), color, 2 if p["observed"] else 1)
        label(text, x1, y1 - 8, color)
    return image


class CameraPipeline:
    def __init__(self, detector, source="picamera2", rotation=180, detect_fps=10,
                 width=640, height=480, hfov=62, stale_seconds=.75):
        self.detector, self.source = detector, source
        self.rotation, self.detect_fps = rotation, detect_fps
        self.width, self.height, self.hfov = width, height, hfov
        self.stale_seconds = stale_seconds
        self.condition = threading.Condition()
        self.stop_event = threading.Event()
        self.latest = None
        self.result = None
        self.sequence = 0
        self.error = None
        self.capture_fps = 0.0
        self.workers = []
        self.radar = None
        self.bridge = None
        self._camera = None
        self._source_epoch = 0
        self.source_session_id = uuid.uuid4().hex
        blank = np.full((height, width, 3), (24, 22, 18), dtype=np.uint8)
        cv2.putText(blank, "Waiting for fresh camera frames", (25, height // 2),
                    cv2.FONT_HERSHEY_SIMPLEX, .65, (210, 220, 230), 1, cv2.LINE_AA)
        self.blank_jpeg = cv2.imencode('.jpg', blank)[1].tobytes()

    def start(self):
        for target in (self.capture_loop, self.detect_loop):
            worker = threading.Thread(target=target, daemon=True, name=target.__name__)
            worker.start()
            self.workers.append(worker)

    def stop(self):
        self.stop_event.set()
        with self.condition:
            self.condition.notify_all()
        for worker in self.workers:
            worker.join(timeout=3)

    def publish_frame(self, frame, captured_at, epoch=0):
        with self.condition:
            self.sequence += 1
            self.latest = (self.sequence, captured_at, time.time(), frame, epoch)
            self.condition.notify_all()

    def capture_loop(self):
        while not self.stop_event.is_set():
            camera = None
            try:
                if self.source == "picamera2":
                    from picamera2 import Picamera2
                    camera = Picamera2()
                    # Picamera2 RGB888 is B,G,R in memory, already OpenCV order.
                    # queue=False prevents handing us a previously queued frame.
                    camera.configure(camera.create_video_configuration(
                        main={"size": (self.width, self.height), "format": "RGB888"},
                        controls={"FrameRate": 24}, buffer_count=4, queue=False))
                    camera.start()
                    self.stop_event.wait(.8)
                else:
                    source = int(self.source) if self.source.isdecimal() else self.source
                    camera = cv2.VideoCapture(source)
                    camera.set(cv2.CAP_PROP_BUFFERSIZE, 1)
                    if not camera.isOpened():
                        raise RuntimeError(f"Cannot open camera source {self.source}")
                self._camera = camera
                self._source_epoch += 1
                last = time.monotonic()
                while not self.stop_event.is_set():
                    if self.source == "picamera2":
                        frame = camera.capture_array("main")
                    else:
                        ok, frame = camera.read()
                        if not ok:
                            raise RuntimeError("Camera stopped delivering frames")
                    now = time.monotonic()
                    if self.rotation == 180:
                        frame = cv2.rotate(frame, cv2.ROTATE_180)
                    self.capture_fps = .9 * self.capture_fps + .1 / max(now - last, .001)
                    last = now
                    self.publish_frame(frame, now, self._source_epoch)
            except Exception as exc:
                logging.exception("Camera capture failed")
                with self.condition:
                    self.error = str(exc)
                    self.latest = self.result = None
                    self.condition.notify_all()
                self.stop_event.wait(2)
            finally:
                if camera is not None:
                    if self.source == "picamera2":
                        camera.stop()
                        camera.close()
                    else:
                        camera.release()
                self._camera = None

    def detect_loop(self):
        last_seq, last_epoch, next_at = 0, -1, 0
        last_finished = None
        rate = 0.0
        while not self.stop_event.is_set():
            if self.stop_event.wait(max(0, next_at - time.monotonic())):
                return
            with self.condition:
                self.condition.wait_for(lambda: self.stop_event.is_set() or
                                        (self.latest is not None and self.latest[0] != last_seq), timeout=.5)
                if self.stop_event.is_set():
                    return
                item = self.latest
            if item is None or item[0] == last_seq:
                continue
            seq, captured, wall_time, frame, epoch = item
            last_seq = seq
            start = time.monotonic()
            next_at = start + 1 / self.detect_fps
            if start - captured > self.stale_seconds:
                continue
            try:
                if epoch != last_epoch:
                    self.detector.reset()
                    last_epoch = epoch
                people = self.detector.detect(frame, timestamp=captured)
                infer_ms = (time.monotonic() - start) * 1000
                h, w = frame.shape[:2]
                for p in people:
                    p.update(camera_position(p["box"], w, h, self.hfov))
                rendered = draw_detections(frame, people, self.detector.last_raw)
                ok, jpeg = cv2.imencode('.jpg', rendered, [cv2.IMWRITE_JPEG_QUALITY, 85])
                raw_ok, raw_jpeg = cv2.imencode('.jpg', frame, [cv2.IMWRITE_JPEG_QUALITY, 85])
                if not ok or not raw_ok:
                    raise RuntimeError("JPEG encoding failed")
                finished = time.monotonic()
                if last_finished:
                    rate = .8 * rate + .2 / max(finished - last_finished, .001)
                last_finished = finished
                result = dict(frame_id=seq, captured_at=captured, timestamp=wall_time,
                              generation=epoch,
                              frame_width=w, frame_height=h, people=people,
                              raw=self.detector.last_raw, infer_ms=round(infer_ms, 1),
                              infer_fps=round(rate, 1),
                              pipeline_ms=round((finished - captured) * 1000, 1),
                              jpeg=jpeg.tobytes(), inference_jpeg=raw_jpeg.tobytes())
                with self.condition:
                    # A camera reconnect/error during inference must not restore
                    # an obsolete result from the previous connection.
                    if self.latest is not None and self.latest[4] == epoch:
                        self.result = result
                        self.error = None
                        self.condition.notify_all()
            except Exception as exc:
                logging.exception("Person inference failed")
                with self.condition:
                    self.error = str(exc)
                    self.result = None
                self.stop_event.wait(.5)

    def snapshot(self):
        now = time.monotonic()
        with self.condition:
            r = dict(self.result) if self.result else {}
            error = self.error
        age = now - r.get("captured_at", -1e9)
        fresh = bool(r) and age <= self.stale_seconds and not error
        people = r.get("people", []) if fresh else []
        return {"status": "live" if fresh else "waiting" if not r and not error else "stale",
                "source_session_id": self.source_session_id,
                "camera_generation": r.get("generation", self._source_epoch),
                "camera_capture_mono_ms": r.get("captured_at", 0) * 1000,
                "camera_hfov_deg": self.hfov,
                "camera_connected": fresh, "error": error,
                "model": self.detector.cfg["model"], "source": self.source,
                "rotation": self.rotation, "frame_id": r.get("frame_id", 0),
                "timestamp": r.get("timestamp"), "frame_age_ms": round(age * 1000, 1) if r else None,
                "frame_width": r.get("frame_width", self.width),
                "frame_height": r.get("frame_height", self.height),
                "infer_ms": r.get("infer_ms", 0), "pipeline_ms": r.get("pipeline_ms", 0),
                "infer_fps": r.get("infer_fps", 0) if fresh else 0,
                "capture_fps": round(self.capture_fps, 1) if fresh else 0,
                "people": people, "person_count": sum(p["observed"] for p in people),
                "coasting_count": sum(not p["observed"] for p in people),
                "raw": r.get("raw", []) if fresh else [],
                "radar": self.radar.snapshot() if self.radar else {"status": "disabled", "targets": []},
                "quest": self.bridge.status() if self.bridge else {"status": "disabled", "clients": 0}}

    def jpeg(self):
        with self.condition:
            r = self.result
            if r and not self.error and time.monotonic() - r["captured_at"] <= self.stale_seconds:
                return r["frame_id"], r["jpeg"]
        return 0, self.blank_jpeg


def create_app(pipeline):
    app = Flask(__name__, template_folder=str(HERE), static_folder=str(HERE / 'static'), static_url_path='/assets')
    from radar_web import register_radar_routes
    register_radar_routes(app, pipeline.radar)

    @app.after_request
    def no_cache(response):
        response.headers["Cache-Control"] = "no-store, max-age=0"
        return response

    @app.get('/')
    def index():
        return render_template('dashboard.html', quest_mode=False)

    @app.get('/quest')
    def quest_view():
        return render_template('dashboard.html', quest_mode=True)

    @app.get('/handoff.json')
    def handoff():
        state = pipeline.snapshot()
        host = urlsplit(request.host_url).hostname
        host = f'[{host}]' if ':' in host else host
        bridge = state['quest']
        return jsonify(
            schema_version=1,
            dashboard_url=request.host_url,
            quest_url=request.host_url+'quest',
            telemetry_url=request.host_url+'detections',
            video_url=request.host_url+'stream',
            websocket_url=f"ws://{host}:{bridge['port']}/" if bridge['status'] == 'listening' else None,
            sensors={'camera': state['status'], 'radar': state['radar']['status']},
            bridge=bridge,
            radar_reference_origin=state['radar'].get('reference_origin'),
            radar_mount=state['radar'].get('config'),
            coordinates={'units': 'metres', 'right': '+X', 'forward': '+Y',
                         'unreal_local_cm': {'X': '100 * forward_m', 'Y': '100 * right_m', 'Z': 'unobserved'}},
            stale_after_ms={'camera': 750, 'radar': 500, 'rig_pose': 1000},
            camera_radar_association='not_calibrated',
            through_wall_performance='not_verified',
            native_radar_rendering='not_integrated',
            headset_validation='pending',
        )

    @app.get('/detections')
    def detections():
        return jsonify(pipeline.snapshot())

    @app.get('/healthz')
    def health():
        state = pipeline.snapshot()
        return jsonify(state), 200 if state['camera_connected'] else 503

    @app.get('/snapshot.jpg')
    def snapshot_image():
        seq, jpeg = pipeline.jpeg()
        return Response(jpeg, mimetype='image/jpeg', headers={"X-Frame-Id": str(seq)})

    @app.get('/pose/frame')
    def pose_frame():
        # Image and detections are one immutable inference result, never two
        # independently timed HTTP reads. JPEG excludes dashboard annotations.
        with pipeline.condition:
            r = pipeline.result
            if not r or pipeline.error or time.monotonic()-r['captured_at']>.75:
                return jsonify(error='No fresh inference frame'), 503
            if 'inference_jpeg' not in r:
                return jsonify(error='Raw inference image unavailable'), 503
            return jsonify(source_session_id=pipeline.source_session_id,
                generation=r['generation'],frame_id=r['frame_id'],
                capture_ms=r['captured_at']*1000,age_ms=(time.monotonic()-r['captured_at'])*1000,
                width=r['frame_width'],height=r['frame_height'],hfov=pipeline.hfov,
                people=r['people'],jpeg_base64=base64.b64encode(r['inference_jpeg']).decode('ascii'))

    @app.get('/stream')
    def stream():
        def generate():
            last_seq = -1
            while not pipeline.stop_event.is_set():
                seq, jpeg = pipeline.jpeg()
                if seq != last_seq:
                    yield (b'--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ' +
                           str(len(jpeg)).encode() + b'\r\nX-Frame-Id: ' + str(seq).encode() +
                           b'\r\n\r\n' + jpeg + b'\r\n')
                    last_seq = seq
                with pipeline.condition:
                    pipeline.condition.wait(timeout=.1)
        return Response(generate(), mimetype='multipart/x-mixed-replace; boundary=frame')

    @app.get('/radar.json')
    def compatibility():
        state = pipeline.snapshot()
        radar = state['radar']
        return jsonify(targets=radar['targets'], radar_connected=radar['status'] == 'live',
                       person_count=state['person_count'], cam_ts=state['timestamp'],
                       cam_positions=[{"x": round(p['x_m'] * 1000), "y": round(p['y_m'] * 1000)}
                                      for p in state['people'] if p['observed'] and p['x_m'] is not None],
                       now=time.time())
    return app


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', default='picamera2', help='picamera2, USB index, or raw stream URL')
    parser.add_argument('--rotation', type=int, choices=(0, 180), default=180)
    parser.add_argument('--port', type=int, default=8766)
    parser.add_argument('--host', default='0.0.0.0')
    parser.add_argument('--detect-fps', type=float, default=10)
    parser.add_argument('--threads', type=int, default=2)
    parser.add_argument('--config', default=CONFIG_PATH)
    parser.add_argument('--hfov', type=float, default=62)
    parser.add_argument('--radar', action='store_true', help='Enable LD2450 after camera validation')
    parser.add_argument('--radar-port', default='/dev/serial0')
    parser.add_argument('--radar-invert-x', action='store_true', default=None)
    parser.add_argument('--radar-config', default=str(HERE / 'radar_config.json'))
    parser.add_argument('--quest-port', type=int, default=0, help='Optional WebSocket handoff, usually 8765')
    parser.add_argument('--stationary-rig', action='store_true', help='Explicitly use a fixed sensor-local frame for Quest')
    args = parser.parse_args(argv)
    if not (0 < args.detect_fps <= 30 and 0 < args.hfov < 180 and args.threads > 0):
        parser.error('Invalid FPS, horizontal FOV, or thread count')
    cv2.setNumThreads(1)
    detector = PersonDetector(args.config, threads=args.threads)
    pipeline = CameraPipeline(detector, source=args.source, rotation=args.rotation,
                              detect_fps=args.detect_fps, hfov=args.hfov)
    if args.radar:
        from radar_service import RadarService
        pipeline.radar = RadarService(args.radar_port, invert_x=args.radar_invert_x, config_path=args.radar_config)
        pipeline.radar.start()
    if args.quest_port:
        from quest_bridge import QuestBridge
        pipeline.bridge = QuestBridge(pipeline, args.host, args.quest_port, args.stationary_rig)
        pipeline.bridge.start()
    pipeline.start()
    def shutdown(*_):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, shutdown)
    print(f'Camera dashboard: http://{args.host}:{args.port}/', flush=True)
    try:
        create_app(pipeline).run(host=args.host, port=args.port, threaded=True, use_reloader=False)
    finally:
        pipeline.stop()
        if pipeline.radar:
            pipeline.radar.stop()
        if pipeline.bridge:
            pipeline.bridge.stop()


if __name__ == '__main__':
    main()
