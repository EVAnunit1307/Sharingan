"""Optional laptop live-depth preview; a separate Core ML process, latest result only."""
import argparse
import atexit
import json
import os
from pathlib import Path
import subprocess
import threading
import time
from urllib.request import urlopen

from Mapping.depth_build import PACKAGE,available
from Mapping.learned import ROOT,RESEARCH,RESEARCH_PYTHON
from Mapping.reconstruct import atomic_json


class LiveDepth:
    def __init__(self,pi_http,root=None):
        self.pi_http=pi_http
        self.root=Path(root) if root else RESEARCH/'live-depth'
        self.process=None;self.log=None;self.lock=threading.Lock()
        atexit.register(self.stop)

    def status(self):
        with self.lock:
            running=self.process is not None and self.process.poll() is None
        if not running:return dict(state='stopped',available=available(),fresh=False)
        try:data=json.loads((self.root/'status.json').read_text())
        except (OSError,ValueError):data=dict(state='loading')
        age=time.time()-data.get('processed_at',0)
        data.update(available=available(),fresh=data.get('state')=='live' and 0<=age<3,
                    result_age_seconds=round(age,2) if data.get('processed_at') else None)
        return data

    def start(self):
        if not self.pi_http:raise ValueError('No Pi configured')
        if not available():raise ValueError('Depth environment is not installed')
        with self.lock:
            if self.process is not None and self.process.poll() is None:return
            self.root.mkdir(parents=True,exist_ok=True)
            atomic_json(self.root/'status.json',dict(state='loading',phase='Loading the depth model on this Mac'))
            if self.log:self.log.close()
            self.log=(self.root/'worker.log').open('a')
            self.process=subprocess.Popen([str(RESEARCH_PYTHON),'-m','Mapping.depth_live','--worker',
                                           '--pi-http',self.pi_http,'--output',str(self.root),'--parent-pid',str(os.getpid())],
                                           cwd=ROOT,stdout=self.log,stderr=subprocess.STDOUT)

    def stop(self):
        with self.lock:
            if self.process is not None and self.process.poll() is None:
                self.process.terminate()
                try:self.process.wait(timeout=5)
                except subprocess.TimeoutExpired:self.process.kill();self.process.wait(timeout=2)
            self.process=None
            if self.log:self.log.close();self.log=None


def worker(pi_http,output,parent_pid=None):
    import io
    import cv2
    import coremltools as ct
    import numpy as np
    from PIL import Image
    output.mkdir(parents=True,exist_ok=True)
    model=ct.models.MLModel(str(PACKAGE),compute_units=ct.ComputeUnit.ALL)
    inp=model.get_spec().description.input[0]
    size=(inp.type.imageType.width,inp.type.imageType.height)
    count=0;latencies=[]
    while True:
        if parent_pid is not None and os.getppid()!=parent_pid:return
        start=time.monotonic()
        try:
            with urlopen(pi_http.rstrip('/')+'/mapping/preview.jpg',timeout=4) as response:
                jpeg=response.read(4*1024*1024)
                frame_id=response.headers.get('X-Frame-Id')
                capture_age=response.headers.get('X-Frame-Age-Ms')
            source=Image.open(io.BytesIO(jpeg)).convert('RGB').resize(size)
            before=time.monotonic()
            prediction=np.asarray(model.predict({inp.name:source})['depth'],dtype=np.float32).squeeze()
            inference_ms=(time.monotonic()-before)*1000
            low,high=np.percentile(prediction,[2,98])
            shade=np.clip((prediction-low)/max(high-low,1e-6)*255,0,255).astype(np.uint8)
            panel=np.concatenate([cv2.cvtColor(np.asarray(source),cv2.COLOR_RGB2BGR),cv2.cvtColor(shade,cv2.COLOR_GRAY2BGR)],axis=1)
            title=np.full((62,panel.shape[1],3),24,dtype=np.uint8)
            cv2.putText(title,'NORMAL CAMERA  |  AI DEPTH ESTIMATE - NOT THERMAL',(10,24),cv2.FONT_HERSHEY_SIMPLEX,.55,(245,245,245),1,cv2.LINE_AA)
            cv2.putText(title,'Dark: farther   Bright: nearer   |   No metres or temperature; not a 3D map',(10,47),cv2.FONT_HERSHEY_SIMPLEX,.45,(195,195,195),1,cv2.LINE_AA)
            ok,encoded=cv2.imencode('.jpg',np.concatenate([title,panel]),[cv2.IMWRITE_JPEG_QUALITY,85])
            if not ok:raise ValueError('Could not encode depth preview')
            temporary=output/'preview.tmp';temporary.write_bytes(encoded.tobytes());temporary.replace(output/'preview.jpg')
            elapsed=(time.monotonic()-start)*1000;latencies.append(elapsed);latencies=latencies[-60:];count+=1
            atomic_json(output/'status.json',dict(state='live',phase='Estimated depth on the Mac',processed_at=time.time(),
                        frame_id=frame_id,source_reported_age_ms=float(capture_age) if capture_age else None,
                        inference_ms=round(inference_ms,1),fetch_infer_render_ms=round(elapsed,1),
                        median_pipeline_ms=round(float(np.median(latencies)),1),p95_pipeline_ms=round(float(np.percentile(latencies,95)),1),
                        frames_processed=count,rate_limit_fps=3,
                        meaning='Relative depth estimate; not thermal, metric distance, persistent mapping or live rig pose'))
        except Exception as exc:
            atomic_json(output/'status.json',dict(state='unavailable',phase=str(exc),fresh=False))
        time.sleep(max(.05,1/3-(time.monotonic()-start)))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--worker',action='store_true')
    parser.add_argument('--pi-http',required=True);parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--parent-pid',type=int)
    args=parser.parse_args();worker(args.pi_http,args.output,args.parent_pid)
