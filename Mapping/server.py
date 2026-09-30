"""Laptop map dashboard: python -m Mapping.server --pi-http http://larp-pi.local:8766"""
import argparse
from http.client import HTTPException
import importlib.util
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
from urllib.error import HTTPError
from urllib.parse import urlsplit
from urllib.request import Request, urlopen
import zipfile

from flask import Flask, Response, jsonify, request, send_from_directory

HERE = Path(__file__).resolve().parent
SESSION_ID = re.compile(r'^[0-9]{8}T[0-9]{6}Z-[a-f0-9]{8}$')
MAX_ARCHIVE = 1024*1024*1024


def identifier(value):
    if not isinstance(value,str) or not SESSION_ID.fullmatch(value):
        raise ValueError('Invalid recording ID')
    return value


def import_archive(archive, root, session_id):
    """Import only our capture format; never extract arbitrary ZIP paths or overwrite a session."""
    identifier(session_id)
    root.mkdir(parents=True, exist_ok=True)
    destination = root/session_id
    if destination.exists():
        return destination
    with tempfile.TemporaryDirectory(prefix='.import-', dir=root) as temporary:
        staging = Path(temporary)/session_id
        staging.mkdir()
        with zipfile.ZipFile(archive) as bundle:
            files = bundle.infolist()
            if len(files) > 7000 or sum(f.file_size for f in files) > MAX_ARCHIVE:
                raise ValueError('Recording archive exceeds import limits')
            seen = set()
            for member in files:
                name = member.filename
                allowed = name in ('manifest.json', 'frames.jsonl') or re.fullmatch(r'images/[0-9]{9}\.jpg', name)
                if not allowed or name in seen or member.file_size > 16*1024*1024:
                    raise ValueError('Recording archive has an unexpected or oversized entry')
                seen.add(name)
                path = staging/PurePosixPath(name)
                path.parent.mkdir(exist_ok=True)
                with bundle.open(member) as source, path.open('xb') as target:
                    shutil.copyfileobj(source, target)
        manifest = json.loads((staging/'manifest.json').read_text())
        if manifest.get('schema_version') != 1 or manifest.get('session_id') != session_id:
            raise ValueError('Recording manifest does not match the selected session')
        if manifest.get('status') == 'recording':
            manifest['status'] = 'interrupted'
            (staging/'manifest.json').write_text(json.dumps(manifest))
        rows = [json.loads(line) for line in (staging/'frames.jsonl').read_text().splitlines() if line.strip()]
        images = [row.get('image') for row in rows]
        if len(set(images)) != len(images) or set(images) != {name for name in seen if name.startswith('images/')}:
            raise ValueError('Image files and frame metadata do not match')
        if len({row.get('camera_generation') for row in rows}) > 1:
            raise ValueError('Recording mixes camera generations; record a new walk')
        staging.rename(destination)
    return destination


class MapJobs:
    def __init__(self, root, pi_http):
        self.root = Path(root).resolve()
        self.pi_http = pi_http.rstrip('/') if pi_http else None
        self.lock = threading.Lock()
        self.job = None
        self.thread = None

    def status(self):
        with self.lock:
            state = dict(self.job) if self.job else dict(state='idle')
        if state.get('session_id') and state['state'] == 'running':
            path = self.root/state['session_id']/'reconstruction.json'
            try:
                state.update(json.loads(path.read_text()))
            except (OSError, ValueError):
                pass
        return state

    def start(self, session_id, backend='standard'):
        identifier(session_id)
        if backend not in ('standard','xfeat','depth'):
            raise ValueError('Unknown mapping backend')
        if backend=='xfeat':
            from Mapping.learned import available
            if not available():raise ValueError('Optional stronger matcher is not installed')
        if backend=='depth':
            from Mapping.depth_build import available
            if not available():raise ValueError('Optional depth model is not installed')
            if not (self.root/session_id/'scene.json').is_file():raise ValueError('Build a sparse map first')
        if not importlib.util.find_spec('pycolmap'):
            raise ValueError('Install laptop mapping dependencies: python -m pip install -r Mapping/requirements.txt')
        with self.lock:
            if self.thread and self.thread.is_alive():
                raise ValueError('A reconstruction is already running')
            self.job = dict(session_id=session_id, backend=backend, state='downloading', phase='Copying the recording from the Pi')
            self.thread = threading.Thread(target=self._run, args=(session_id,backend), daemon=True, name='map-build')
            self.thread.start()

    def _run(self, session_id, backend='standard'):
        try:
            self.root.mkdir(parents=True, exist_ok=True)
            session = self.root/session_id
            if not session.exists():
                if not self.pi_http:
                    raise ValueError('No Pi configured and this recording is not saved on the laptop')
                with tempfile.TemporaryFile() as archive:
                    with urlopen(self.pi_http+'/mapping/sessions/'+session_id+'/archive', timeout=60) as response:
                        size = 0
                        while True:
                            block = response.read(1024*1024)
                            if not block:
                                break
                            size += len(block)
                            if size > MAX_ARCHIVE:
                                raise ValueError('Recording is larger than the 1 GiB import limit')
                            archive.write(block)
                    archive.seek(0)
                    session = import_archive(archive, self.root, session_id)
            from Mapping.reconstruct import atomic_json
            atomic_json(session/'reconstruction.json', dict(state='running', phase='Starting reconstruction'))
            with self.lock:
                self.job.update(state='running')
            with (session/'reconstruction.log').open('a') as output:
                command=([sys.executable,'-m','Mapping.depth_build','--session',str(session)] if backend=='depth' else
                         [sys.executable,'-m','Mapping.reconstruct','--session',str(session),'--backend',backend])
                process = subprocess.run(command,
                                         cwd=HERE.parent, stdout=output, stderr=subprocess.STDOUT)
            result = json.loads((session/'reconstruction.json').read_text())
            if process.returncode and result.get('state') != 'failed':
                result = dict(state='failed', phase='Reconstruction exited; inspect reconstruction.log')
            with self.lock:
                self.job.update(result)
        except Exception as exc:
            with self.lock:
                self.job.update(state='failed', phase=str(exc))

    def sessions(self):
        result = []
        for path in sorted(self.root.glob('*/manifest.json'), reverse=True):
            if not SESSION_ID.fullmatch(path.parent.name):
                continue
            try:
                item = json.loads(path.read_text())
                item['has_model'] = (path.parent/'scene.json').is_file()
                item['has_depth'] = (path.parent/'depth.json').is_file()
                annotation=path.parent/'annotations.json'
                if annotation.is_file():item['display_label']=json.loads(annotation.read_text()).get('display_label')
                result.append(item)
            except (OSError, ValueError):
                continue
        return result


def register_routes(app, pi_http, root):
    # The Pi HTTP server listens on IPv4. mDNS also advertises an IPv6 address,
    # whose failed connection added ~1 s to each request on the measured hotspot.
    # Resolve at startup, keeping the .local hostname as the portable config.
    if pi_http:
        address = urlsplit(pi_http)
        if address.scheme == 'http' and (address.hostname or '').endswith('.local') and not address.username:
            try:
                ipv4 = socket.gethostbyname(address.hostname)
                pi_http = address._replace(netloc=ipv4 + (f':{address.port}' if address.port else '')).geturl()
            except OSError:
                pass  # The Pi may be offline at startup; retain hostname retry.
    jobs = MapJobs(root, pi_http)
    app.extensions['mapping_jobs'] = jobs
    from Mapping.depth_live import LiveDepth
    live_depth=LiveDepth(pi_http)
    app.extensions['live_depth']=live_depth
    from Mapping.pose_live import LivePose
    live_pose=LivePose(pi_http,jobs.root)
    app.extensions['live_pose']=live_pose

    def origin_allowed():
        return request.headers.get('Origin') in (None, request.host_url.rstrip('/'))

    @app.get('/map')
    def map_view():
        return send_from_directory(HERE/'static', 'index.html')

    @app.get('/map-assets/<path:name>')
    def map_assets(name):
        return send_from_directory(HERE/'static', name)

    @app.route('/map-api/pi/<action>', methods=['GET', 'POST'])
    def pi_proxy(action):
        allowed = {'status':'GET', 'sessions':'GET', 'preview.jpg':'GET', 'start':'POST', 'stop':'POST','configure':'POST'}
        if allowed.get(action) != request.method:
            return jsonify(error='Unsupported mapping action'), 405
        if request.method == 'POST' and not origin_allowed():
            return jsonify(error='Use this laptop dashboard'), 403
        if not pi_http:
            return jsonify(error='No Pi configured; saved maps are still available'), 503
        payload=(request.get_json(silent=True) or {}) if action=='configure' else {}
        upstream = Request(pi_http.rstrip('/')+'/mapping/'+action, data=json.dumps(payload).encode() if request.method=='POST' else None,
                           headers={'Content-Type':'application/json'}, method=request.method)
        try:
            with urlopen(upstream, timeout=8) as response:
                data = response.read(8*1024*1024)
                headers = {key: response.headers[key] for key in ('X-Frame-Id', 'X-Frame-Age-Ms') if key in response.headers}
                return Response(data, content_type=response.headers.get('Content-Type', 'application/json'), headers=headers)
        except HTTPError as exc:
            return jsonify(error=f'Pi returned {exc.code}. Check it is running with --mapping-only or --mapping'), 502
        except OSError:
            return jsonify(error='Pi unavailable. Check power, Wi-Fi and the mapping camera command'), 503

    @app.get('/map-api/pi/stream')
    def pi_stream():
        if not pi_http:
            return jsonify(error='No Pi configured'), 503
        try:
            upstream = urlopen(pi_http.rstrip('/')+'/mapping/stream', timeout=5)
        except (OSError, HTTPException):
            return jsonify(error='Camera stream unavailable'), 503

        def generate():
            try:
                # read1 forwards available bytes immediately; read(size) can wait
                # for several camera frames to fill its buffer.
                while chunk := upstream.read1(64*1024):
                    yield chunk
            except (OSError, HTTPException):
                return
            finally:
                upstream.close()

        response = Response(generate(), content_type=upstream.headers.get('Content-Type', 'multipart/x-mixed-replace; boundary=frame'),
                            headers={'Cache-Control': 'no-store', 'X-Accel-Buffering': 'no'})
        response.call_on_close(upstream.close)
        return response

    @app.get('/map-api/local')
    def local():
        from Mapping.learned import available
        from Mapping.depth_build import available as depth_available
        return jsonify(sessions=jobs.sessions(), job=jobs.status(), backend_ready=bool(importlib.util.find_spec('pycolmap')),
                       learned_backend_ready=available(),depth_backend_ready=depth_available(),
                       storage=str(jobs.root), pi_http=pi_http)

    @app.route('/map-api/live-depth/<action>',methods=['GET','POST'])
    def depth_live(action):
        if request.method=='GET' and action=='status':return jsonify(live_depth.status())
        if request.method=='GET' and action=='preview.jpg':
            if not live_depth.status()['fresh']:return jsonify(error='Live depth is unavailable or stale'),503
            return send_from_directory(live_depth.root,'preview.jpg',max_age=0)
        if request.method!='POST' or action not in ('start','stop'):return jsonify(error='Unsupported depth action'),405
        if not origin_allowed():return jsonify(error='Use this laptop dashboard'),403
        try:
            getattr(live_depth,action)()
            return jsonify(live_depth.status())
        except ValueError as exc:return jsonify(error=str(exc)),409

    @app.route('/map-api/live-pose/<action>',methods=['GET','POST'])
    def pose_live(action):
        if request.method=='GET' and action=='status':return jsonify(live_pose.status())
        if request.method!='POST' or action not in ('start','stop'):return jsonify(error='Unsupported pose action'),405
        if not origin_allowed():return jsonify(error='Use this laptop dashboard'),403
        try:
            if action=='stop':live_pose.stop()
            else:
                payload=request.get_json(silent=True) or {}
                if not isinstance(payload,dict):raise ValueError('Expected a map selection')
                live_pose.start(payload.get('session_id',''))
            return jsonify(live_pose.status())
        except ValueError as exc:return jsonify(error=str(exc)),409

    @app.post('/map-api/build/<session_id>')
    def build(session_id):
        if not origin_allowed():
            return jsonify(error='Use this laptop dashboard'), 403
        try:
            payload=request.get_json(silent=True) or {}
            if not isinstance(payload,dict):raise ValueError('Expected a build options object')
            jobs.start(session_id,payload.get('backend','standard'))
            return jsonify(accepted=True), 202
        except ValueError as exc:
            return jsonify(error=str(exc)), 409

    @app.get('/map-api/model/<session_id>')
    def model(session_id):
        try:
            identifier(session_id)
        except ValueError as exc:
            return jsonify(error=str(exc)), 400
        return send_from_directory(jobs.root/session_id, 'scene.json')

    @app.get('/map-api/depth/<session_id>')
    def depth_model(session_id):
        try:
            identifier(session_id)
            scene=json.loads((jobs.root/session_id/'scene.json').read_text())
            layer=json.loads((jobs.root/session_id/'depth.json').read_text())
            if layer['source_revision']!=scene['revision']:
                return jsonify(error='Sparse map changed; rebuild the inferred layer'),409
            return jsonify(layer)
        except ValueError as exc:
            return jsonify(error=str(exc)),400
        except FileNotFoundError:
            return jsonify(error='No inferred surface layer for this recording'),404

    return jobs


def create_app(pi_http=None, root=Path('Saved/Mapping')):
    if pi_http:
        url = urlsplit(pi_http)
        if url.scheme not in ('http', 'https') or not url.hostname:
            raise ValueError('Pi address must be an HTTP(S) URL')
    app = Flask(__name__)
    register_routes(app, pi_http, root)

    @app.get('/')
    def index():
        from flask import redirect
        return redirect('/map')

    @app.after_request
    def no_cache(response):
        response.headers['Cache-Control'] = 'no-store'
        return response
    return app


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pi-http', default='http://larp-pi.local:8766')
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=8766)
    parser.add_argument('--root', type=Path, default=HERE.parent/'Saved'/'Mapping')
    args = parser.parse_args()
    print(f'Open http://localhost:{args.port}/map — laptop storage: {args.root}', flush=True)
    create_app(args.pi_http, args.root).run(host=args.host, port=args.port, threaded=True, use_reloader=False)


if __name__ == '__main__':
    main()
