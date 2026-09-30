"""Managed laptop worker for optional localization in a saved map."""
import argparse
import atexit
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time
from urllib.request import urlopen

from Mapping.learned import ROOT,RESEARCH
from Mapping.reconstruct import atomic_json

NETWORK=RESEARCH/'models'/'xfeat-vga.onnx'


class LivePose:
    def __init__(self,pi_http,sessions,root=None):
        self.pi_http=pi_http;self.sessions=Path(sessions);self.root=Path(root) if root else RESEARCH/'live-pose'
        self.lock=threading.RLock();self.process=None;self.log=None
        atexit.register(self.stop)

    def status(self):
        with self.lock:running=self.process is not None and self.process.poll() is None
        if not running:return dict(state='stopped',available=NETWORK.is_file(),fresh=False)
        try:data=json.loads((self.root/'status.json').read_text())
        except (ValueError,OSError):data=dict(state='loading')
        age=time.time()-data.get('processed_at',0)+data.get('source_age_seconds',0)
        fresh=data.get('state')=='tracking' and 0<=age<.75
        data.update(available=True,fresh=fresh,age_seconds=round(age,3) if data.get('processed_at') else None)
        if not fresh:data.pop('position',None);data.pop('rotation',None)
        return data

    def start(self,session_id):
        from Mapping.server import identifier
        identifier(session_id)
        if not self.pi_http or not NETWORK.is_file():raise ValueError('Pi address or optional edge model is unavailable')
        session=self.sessions/session_id
        if not (session/'scene.json').is_file():raise ValueError('Build a sparse map first')
        with self.lock:
            self.stop();self.root.mkdir(parents=True,exist_ok=True)
            atomic_json(self.root/'status.json',dict(state='loading',session_id=session_id,reason='Loading landmarks from the saved map'))
            self.log=(self.root/'worker.log').open('a')
            environment=dict(os.environ,OPENBLAS_NUM_THREADS='1')
            self.process=subprocess.Popen([sys.executable,'-m','Mapping.pose_live','--worker','--session',str(session),
                                           '--pi-http',self.pi_http,'--output',str(self.root),'--parent-pid',str(os.getpid())],cwd=ROOT,env=environment,
                                           stdout=self.log,stderr=subprocess.STDOUT)

    def stop(self):
        with self.lock:
            if self.process is not None and self.process.poll() is None:
                self.process.terminate()
                try:self.process.wait(timeout=5)
                except subprocess.TimeoutExpired:self.process.kill();self.process.wait(timeout=2)
            self.process=None
            if self.log:self.log.close();self.log=None


def worker(session,pi_http,output,parent_pid=None):
    import cv2
    import numpy as np
    from Mapping.map_localizer import MapLocalizer
    cv2.setNumThreads(1);localizer=MapLocalizer(session,NETWORK)
    while True:
        if parent_pid is not None and os.getppid()!=parent_pid:return
        start=time.monotonic()
        try:
            current=json.loads((session/'scene.json').read_text())
            if current['revision']!=localizer.scene['revision']:raise ValueError('Map revision changed; restart localization')
            with urlopen(pi_http.rstrip('/')+'/mapping/preview.jpg',timeout=3) as response:
                frame=cv2.imdecode(np.frombuffer(response.read(4*1024*1024),dtype=np.uint8),cv2.IMREAD_COLOR)
                frame_id=response.headers.get('X-Frame-Id')
                reported_age=float(response.headers.get('X-Frame-Age-Ms',0))/1000
            result=localizer.locate(frame)
            elapsed=time.monotonic()-start
            result.update(session_id=session.name,source_revision=localizer.scene['revision'],frame_id=frame_id,
                          processed_at=time.time(),source_age_seconds=reported_age+elapsed,processing_ms=round(elapsed*1000,1))
        except Exception as exc:result=dict(state='lost',session_id=session.name,reason=str(exc),processed_at=time.time())
        atomic_json(output/'status.json',result)
        time.sleep(max(.05,.25-(time.monotonic()-start)))


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--worker',action='store_true')
    p.add_argument('--session',type=Path,required=True);p.add_argument('--pi-http',required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--parent-pid',type=int)
    a=p.parse_args();worker(a.session,a.pi_http,a.output,a.parent_pid)
