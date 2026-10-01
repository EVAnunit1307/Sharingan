"""Bounded clean-image recorder, fed by the existing camera owner. No motor I/O."""
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import queue
import re
import shutil
import threading
import time
import uuid
import zipfile

import cv2
from flask import Response, jsonify, request, send_file

SESSION_ID = re.compile(r"^[0-9]{8}T[0-9]{6}Z-[a-f0-9]{8}$")
MAX_SAMPLE_FPS = 24


def validate_fps(fps):
    if isinstance(fps, bool) or not isinstance(fps, (int, float)) or not math.isfinite(fps) or not 0 < fps <= MAX_SAMPLE_FPS:
        raise ValueError(f'Recording rate must be greater than 0 and at most {MAX_SAMPLE_FPS} fps')
    return float(fps)


def write_json(path, value):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value, indent=2, allow_nan=False) + '\n')
    temporary.replace(path)


class MappingCapture:
    def __init__(self, root, fps=3, max_seconds=120, autostart=False, min_free_bytes=512*1024*1024):
        fps = validate_fps(fps)
        if not 5 <= max_seconds <= 600:
            raise ValueError('Mapping capture requires 5–600 seconds')
        self.root = Path(root)
        self.fps, self.max_seconds = fps, max_seconds
        self.min_free_bytes = min_free_bytes
        self.autostart = autostart
        self.lock = threading.RLock()
        self.condition = threading.Condition(self.lock)
        self.jobs = queue.Queue(maxsize=4)
        self.shutdown = threading.Event()
        self.active = None
        self.last_session = None
        self.next_sample = None
        self.preview = None
        self.preview_sequence = 0
        self.preview_pending = None
        self.preview_fps = 12
        self.last_capture = None
        self.error = None
        self.selection = 'uniform'
        self.exposure_us = None
        self.requested_gain = 8.0
        self.settings_version = 0
        self.settings_changed_at = -1e9
        self.camera_metadata = {}
        self.applied_controls = {}
        self.worker = threading.Thread(target=self._run, name='mapping-images', daemon=True)
        self.worker.start()
        self.preview_worker = threading.Thread(target=self._preview_run, name='mapping-preview', daemon=True)
        self.preview_worker.start()

    def start(self):
        with self.lock:
            if self.active:
                raise ValueError('Finish the current recording before starting another')
            if time.monotonic()-self.settings_changed_at<1:
                raise ValueError('Allow one second for the camera settings to settle')
            self.root.mkdir(parents=True, exist_ok=True)
            if shutil.disk_usage(self.root).free < self.min_free_bytes:
                raise ValueError('Not enough free disk space for recording')
            identifier = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ-') + uuid.uuid4().hex[:8]
            path = self.root / identifier
            (path / 'images').mkdir(parents=True)
            manifest = dict(schema_version=1, session_id=identifier, source='drone_camera',
                            status='recording', created_at=datetime.now(timezone.utc).isoformat(),
                            frames_saved=0, dropped_frames=0, sample_fps=self.fps,
                            max_seconds=self.max_seconds, scale='unknown', units='arbitrary',
                            annotation='none', reconstruction='not_built', camera=None,
                            timing='SensorTimestamp when available; host monotonic retained separately')
            manifest.update(frame_selection=self.selection,requested_exposure_us=self.exposure_us,
                            requested_gain=self.requested_gain if self.exposure_us else None)
            write_json(path / 'manifest.json', manifest)
            self.active = dict(path=path, manifest=manifest, pending=0,
                               started=time.monotonic(), generation=None, stopping=False,
                               candidate=None,window_start=None,candidates_seen=0,last_saved_at=None)
            self.last_session = identifier
            self.next_sample = None
            self.error = None
            self.autostart = False
            return self.status()

    def _finish(self, session):
        if session['pending'] or not session['stopping']:
            return
        manifest = session['manifest']
        manifest['status'] = 'failed' if manifest.get('error') else 'complete'
        manifest['finished_at'] = datetime.now(timezone.utc).isoformat()
        try:
            write_json(session['path'] / 'manifest.json', manifest)
        except OSError as exc:
            self.error = f'Could not finalize recording: {exc}'
        if self.active is session:
            self.active = None
        self.condition.notify_all()

    def _request_stop(self, session, reason):
        self._flush_candidate(session)
        session['stopping'] = True
        session['manifest']['stop_reason'] = reason
        self._finish(session)

    def stop(self, reason='operator'):
        with self.condition:
            self.autostart = False
            session = self.active
            if session:
                self._request_stop(session, reason)
                self.condition.wait_for(lambda: self.active is not session, timeout=5)
            return self.status()

    def submit(self, frame, sequence, captured_at, generation, metadata=None, rotation=180, source='picamera2'):
        """Select/copy a few frames; encoding and disk writes never run on capture thread."""
        with self.lock:
            self.last_capture = captured_at
            self.camera_metadata = {key:(metadata or {}).get(key) for key in ('ExposureTime','AnalogueGain','DigitalGain','FrameDuration')}
            # One replaceable frame: a slow viewer never queues old camera images.
            self.preview_pending = (frame.copy(), sequence, captured_at)
            self.condition.notify_all()
            if self.autostart:
                try:
                    self.start()
                except (OSError, ValueError) as exc:
                    self.error = str(exc)
                    self.autostart = False
            session = self.active
            if session and not session['stopping']:
                if time.monotonic() - session['started'] >= self.max_seconds:
                    self._request_stop(session, 'time_limit')
                elif session['generation'] is not None and session['generation'] != generation:
                    self._request_stop(session, 'camera_reconnected')
                else:
                    session['generation'] = generation
            if session and session['stopping']:
                session = None
            if not session:
                return
            if self.selection == 'uniform':
                period = 1 / self.fps
                if self.next_sample is None:
                    self.next_sample = captured_at
                # Small arrival jitter should not discard alternate frames when
                # requested rate equals the sensor rate. Never duplicate a frame.
                if captured_at + min(.002, period * .05) < self.next_sample:
                    return
                # Keep a fixed sampling clock: capture jitter must not turn a
                # requested 12/24 fps into every third/second sensor frame.
                # Skip missed deadlines; never enqueue duplicates to catch up.
                self.next_sample += max(1, math.floor((captured_at - self.next_sample) / period) + 1) * period
            meta = metadata or {}
            row = dict(frame_id=sequence, host_capture_mono_ns=round(captured_at*1e9),
                       sensor_timestamp_ns=meta.get('SensorTimestamp'), exposure_us=meta.get('ExposureTime'),
                       analogue_gain=meta.get('AnalogueGain'), camera_generation=generation)
            geometry = dict(width=int(frame.shape[1]), height=int(frame.shape[0]),
                            rotation=rotation, source=source, intrinsics='not_calibrated')
            if session and session['manifest']['camera'] not in (None, geometry):
                self._request_stop(session, 'camera_geometry_changed')
                session = None
            if session:
                session['manifest']['camera'] = geometry
                if self.selection=='sharpest':
                    if session['window_start'] is None:session['window_start']=captured_at
                    if captured_at-session['window_start']>=1/self.fps:
                        self._flush_candidate(session)
                        session['window_start']=captured_at
                    gray=cv2.cvtColor(cv2.resize(frame,(320,240)),cv2.COLOR_BGR2GRAY)
                    score=float(cv2.Laplacian(gray,cv2.CV_32F).var())
                    session['candidates_seen']+=1
                    candidate=session['candidate']
                    if candidate is None or score>candidate[0]:
                        row['selection_sharpness']=round(score,3)
                        session['candidate']=(score,frame.copy(),row)
                else:
                    self._enqueue(frame.copy(),row,session)

    def _enqueue(self,frame,row,session):
        session['pending']+=1
        try:self.jobs.put_nowait((frame,row,session))
        except queue.Full:
            session['pending']-=1
            session['manifest']['dropped_frames']+=1

    def _flush_candidate(self,session):
        candidate=session.get('candidate')
        if candidate is None:return
        _,frame,row=candidate
        row['selection_candidates']=session['candidates_seen']
        row['selection_method']='sharpest_per_window'
        self._enqueue(frame,row,session)
        session['last_saved_at']=row['host_capture_mono_ns']/1e9
        session['candidate']=None;session['candidates_seen']=0

    def configure(self,selection='uniform',exposure_us=None,gain=8,fps=None):
        if selection not in ('uniform','sharpest'):raise ValueError('Choose uniform or sharpest frame selection')
        if exposure_us is not None and (not isinstance(exposure_us,(int,float)) or not 1000<=exposure_us<=40000):
            raise ValueError('Manual exposure must be 1,000–40,000 microseconds')
        if not isinstance(gain,(int,float)) or not 1<=gain<=16:raise ValueError('Gain must be between 1 and 16')
        if fps is not None: fps = validate_fps(fps)
        with self.lock:
            if self.active:raise ValueError('Stop recording before changing capture settings')
            if fps is not None: self.fps = fps
            self.next_sample = None
            self.selection=selection;self.exposure_us=int(exposure_us) if exposure_us is not None else None
            self.requested_gain=float(gain);self.settings_version+=1;self.settings_changed_at=time.monotonic()
            return self.status()

    def camera_controls(self):
        with self.lock:
            controls={'AeEnable':True} if self.exposure_us is None else {
                'AeEnable':False,'ExposureTime':self.exposure_us,'AnalogueGain':self.requested_gain}
            return self.settings_version,controls

    def _run(self):
        while not self.shutdown.is_set() or not self.jobs.empty():
            try:
                frame, row, session = self.jobs.get(timeout=.2)
            except queue.Empty:
                with self.lock:
                    if self.active and time.monotonic()-self.active['started'] >= self.max_seconds:
                        self._request_stop(self.active, 'time_limit')
                continue
            try:
                ok, encoded = cv2.imencode('.jpg', frame, [cv2.IMWRITE_JPEG_QUALITY, 95])
                if not ok:
                    raise ValueError('Image encoding failed')
                jpeg = encoded.tobytes()
                row['sharpness'] = round(float(cv2.Laplacian(cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY), cv2.CV_64F).var()), 2)
                if session and not session['manifest'].get('error'):
                    if shutil.disk_usage(session['path']).free < self.min_free_bytes:
                        raise OSError('Recording stopped: disk space reserve reached')
                    filename = f"{row['frame_id']:09d}.jpg"
                    destination = session['path'] / 'images' / filename
                    destination.write_bytes(jpeg)
                    row['image'] = 'images/' + filename
                    with (session['path'] / 'frames.jsonl').open('a') as records:
                        records.write(json.dumps(row, allow_nan=False) + '\n')
                    with self.lock:
                        session['manifest']['frames_saved'] += 1
                        write_json(session['path'] / 'manifest.json', session['manifest'])
            except Exception as exc:
                with self.lock:
                    self.error = str(exc)
                    if session:
                        session['manifest']['error'] = str(exc)
                        session['stopping'] = True
            finally:
                with self.condition:
                    if session:
                        session['pending'] -= 1
                        self._finish(session)
                self.jobs.task_done()

    def _preview_run(self):
        while not self.shutdown.is_set():
            with self.condition:
                self.condition.wait_for(lambda: self.preview_pending is not None or self.shutdown.is_set())
                if self.shutdown.is_set():
                    return
                frame, sequence, captured_at = self.preview_pending
                self.preview_pending = None
            started = time.monotonic()
            try:
                ok, encoded = cv2.imencode('.jpg', frame, [cv2.IMWRITE_JPEG_QUALITY, 75])
                if ok:
                    sharpness = round(float(cv2.Laplacian(cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY), cv2.CV_64F).var()), 2)
                    with self.condition:
                        self.preview = (captured_at, encoded.tobytes(), sharpness)
                        self.preview_sequence = sequence
                        self.condition.notify_all()
            except cv2.error:
                # A bad preview must not stop saving the mapping dataset.
                pass
            self.shutdown.wait(max(0, 1/self.preview_fps - (time.monotonic()-started)))

    def preview_stream(self):
        last_sequence = -1
        while not self.shutdown.is_set():
            with self.condition:
                self.condition.wait_for(lambda: self.shutdown.is_set() or
                                        (self.preview is not None and self.preview_sequence != last_sequence), timeout=1)
                if self.shutdown.is_set() or not self.preview or time.monotonic()-self.preview[0] >= 1:
                    return
                if self.preview_sequence == last_sequence:
                    continue
                when, jpeg, _ = self.preview
                last_sequence = self.preview_sequence
            headers = (f'--frame\r\nContent-Type: image/jpeg\r\nContent-Length: {len(jpeg)}\r\n'
                       f'X-Frame-Id: {last_sequence}\r\nX-Capture-Mono-Ns: {round(when*1e9)}\r\n'
                       f'X-Frame-Age-Ms: {(time.monotonic()-when)*1000:.1f}\r\n\r\n').encode()
            yield headers + jpeg + b'\r\n'

    def status(self):
        with self.lock:
            session = self.active
            return dict(enabled=True, state='saving' if session and session['stopping'] else
                        'recording' if session else 'ready', session_id=session['manifest']['session_id'] if session else self.last_session,
                        frames_saved=session['manifest']['frames_saved'] if session else 0,
                        dropped_frames=session['manifest']['dropped_frames'] if session else 0,
                        elapsed_seconds=round(time.monotonic()-session['started'], 1) if session else 0,
                        max_seconds=self.max_seconds, sample_fps=self.fps,
                        sample_fps_max=MAX_SAMPLE_FPS,
                        preview_fps_limit=self.preview_fps,
                        frame_selection=self.selection,requested_exposure_us=self.exposure_us,requested_gain=self.requested_gain,
                        camera_metadata=dict(self.camera_metadata),applied_controls=dict(self.applied_controls),
                        settings_settling=time.monotonic()-self.settings_changed_at<1,
                        preview_age_ms=round((time.monotonic()-self.preview[0])*1000, 1) if self.preview else None,
                        camera_live=self.last_capture is not None and time.monotonic()-self.last_capture < 1,
                        sharpness=self.preview[2] if self.preview else None, error=self.error)

    def sessions(self):
        result = []
        with self.lock:
            current = self.active['manifest']['session_id'] if self.active else None
            for path in sorted(self.root.glob('*/manifest.json'), reverse=True):
                if not SESSION_ID.fullmatch(path.parent.name):
                    continue
                try:
                    item = json.loads(path.read_text())
                    if item['session_id'] != current and item['status'] == 'recording':
                        item['status'] = 'interrupted'
                    result.append(item)
                except (ValueError, KeyError, OSError):
                    continue
        return result

    def archive(self, identifier):
        if not SESSION_ID.fullmatch(identifier):
            raise ValueError('Invalid session ID')
        with self.lock:
            if self.active and self.active['manifest']['session_id'] == identifier:
                raise ValueError('Stop and finish saving before downloading')
            path = self.root / identifier
            if not (path / 'manifest.json').is_file():
                raise ValueError('Session not found')
        # Finished sessions are immutable. Do not hold the capture lock during disk I/O.
        archive = path / 'capture.zip'
        temp = path / (uuid.uuid4().hex + '.zip.tmp')
        try:
            with zipfile.ZipFile(temp, 'w', compression=zipfile.ZIP_STORED) as output:
                for file in [path/'manifest.json', path/'frames.jsonl', *sorted((path/'images').glob('*.jpg'))]:
                    if file.is_file() and not file.is_symlink():
                        output.write(file, str(file.relative_to(path)))
            temp.replace(archive)
        finally:
            temp.unlink(missing_ok=True)
        return archive

    def close(self):
        self.stop('shutdown')
        self.shutdown.set()
        with self.condition:
            self.condition.notify_all()
        self.worker.join(timeout=8)
        self.preview_worker.join(timeout=2)


def register_mapping_capture(app, capture):
    @app.get('/mapping/status')
    def mapping_status():
        return jsonify(capture.status() if capture else dict(enabled=False, error='Start the Pi with --mapping'))

    @app.get('/mapping/sessions')
    def mapping_sessions():
        return jsonify(sessions=capture.sessions() if capture else [])

    @app.post('/mapping/<action>')
    def mapping_action(action):
        if request.headers.get('Origin') not in (None, request.host_url.rstrip('/')):
            return jsonify(error='Use the same-origin mapping dashboard'), 403
        if not capture:
            return jsonify(error='Start the Pi with --mapping'), 503
        try:
            if action == 'start':
                return jsonify(capture.start())
            if action == 'stop':
                return jsonify(capture.stop())
            if action == 'configure':
                options=request.get_json(silent=True)
                if not isinstance(options,dict):raise ValueError('Expected capture settings')
                return jsonify(capture.configure(options.get('selection','uniform'),options.get('exposure_us'),options.get('gain',8),options.get('fps')))
            return jsonify(error='Unknown action'), 404
        except (ValueError, OSError) as exc:
            return jsonify(error=str(exc)), 409

    @app.get('/mapping/preview.jpg')
    def mapping_preview():
        if capture:
            with capture.lock:
                preview = capture.preview
                sequence = capture.preview_sequence
            if preview and time.monotonic()-preview[0] < 1:
                return Response(preview[1], mimetype='image/jpeg', headers={
                    'X-Frame-Id': str(sequence), 'X-Frame-Age-Ms': f'{(time.monotonic()-preview[0])*1000:.1f}'})
        return jsonify(error='No fresh camera image'), 503

    @app.get('/mapping/stream')
    def mapping_stream():
        if not capture:
            return jsonify(error='Mapping capture is disabled'), 503
        return Response(capture.preview_stream(), content_type='multipart/x-mixed-replace; boundary=frame',
                        headers={'Cache-Control': 'no-store', 'X-Accel-Buffering': 'no'})

    @app.get('/mapping/sessions/<identifier>/archive')
    def mapping_archive(identifier):
        if not capture:
            return jsonify(error='Mapping capture is disabled'), 503
        try:
            return send_file(capture.archive(identifier), as_attachment=True, download_name=identifier+'.zip')
        except (ValueError, OSError) as exc:
            return jsonify(error=str(exc)), 409
