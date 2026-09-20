"""Optional laptop pose worker over timestamped Pi inference frames.

Imports ML only when enabled. Latest-frame HTTP polling bounds the queue to one;
pose failure cannot stall or refresh the camera/radar relay.
"""
import base64
import json
import math
import time
from dataclasses import replace
from urllib.request import urlopen
from .fusion import number
from SensorRig.CV.tracking_math import assignment

CONNECTIONS=[(11,12),(11,23),(12,24),(23,24),(11,13),(13,15),(12,14),(14,16),
             (23,25),(25,27),(27,31),(24,26),(26,28),(28,32),(0,7),(0,8)]


def floor_position(points,frame,config):
    if config.rig_motion_mode!='stationary' or config.camera_height_m is None:
        return None
    w,h=frame['width'],frame['height']
    fx=config.camera_fx_px or w/(2*math.tan(math.radians(frame['hfov']/2)))
    fy=config.camera_fy_px or fx
    cx=config.camera_cx_px or w/2;cy=config.camera_cy_px or h/2
    pitch=math.radians(config.camera_pitch_deg)
    positions=[]
    for i in (31,32):
        x,y,q=points[i]
        if q<.65 or not 3<x<w-3 or not 3<y<h-3:return None
        rx,ry=(x-cx)/fx,(y-cy)/fy
        down=ry*math.cos(pitch)+math.sin(pitch)
        if down<.05:return None
        t=config.camera_height_m/down
        right,forward=rx*t,(math.cos(pitch)-ry*math.sin(pitch))*t
        if not .2<forward<8:return None
        positions.append((right,forward))
    if math.dist(*positions)>1.2:return None
    return dict(right_m=sum(p[0] for p in positions)/2,forward_m=sum(p[1] for p in positions)/2,
                source='floor_estimate',intrinsics='calibrated' if config.camera_fx_px else 'hfov_assumption')


def validate_frame(frame):
    if not isinstance(frame,dict) or not isinstance(frame.get('source_session_id'),str):
        raise ValueError('Missing pose source')
    for name in ('generation','frame_id','width','height'):
        if type(frame.get(name)) is not int or not 0<=frame[name]<=2**31-1:
            raise ValueError('Invalid pose frame identity')
    if not 1<=frame['width']<=4096 or not 1<=frame['height']<=4096:
        raise ValueError('Pose image exceeds budget')
    for name in ('capture_ms','age_ms','hfov'):
        if not number(frame.get(name)) or frame[name]<0:
            raise ValueError('Invalid pose frame timing')
    if not 1<frame['hfov']<179 or not isinstance(frame.get('people'),list) or len(frame['people'])>128:
        raise ValueError('Invalid pose frame observations')


def extract_poses(result, frame, config):
    """Bind full-frame pose results to same-frame observed person boxes."""
    people=[p for p in frame['people'] if p.get('observed') is True and isinstance(p.get('box'),list)
            and len(p['box'])==4 and all(number(v) for v in p['box']) and type(p.get('id')) is int][:8]
    candidates=[]
    for image,world in zip(result.pose_landmarks,result.pose_world_landmarks):
        if len(image)!=33 or len(world)!=33:
            continue
        joints=[]
        pixels=[]
        # Controller mode applies the full camera orientation on Quest.
        pitch=math.radians(config.camera_pitch_deg) if config.rig_motion_mode=='stationary' else 0.
        for a,b in zip(image,world):
            values=[a.x,a.y,b.x,b.y,b.z,a.visibility or 0.,a.presence or 0.]
            if not all(number(v) and abs(v)<100 for v in values):
                break
            quality=min(a.visibility or 0.,a.presence or 0.)
            # Camera x right, y down, z away -> right/up/forward.
            up=-b.y*math.cos(pitch)-b.z*math.sin(pitch)
            forward=-b.y*math.sin(pitch)+b.z*math.cos(pitch)
            joints.append([b.x,up,forward,max(0.,min(1.,quality))])
            pixels.append([a.x*frame['width'],a.y*frame['height'],quality])
        if len(joints)!=33:
            continue
        reliable=[p for p in pixels if p[2]>=.5]
        if len(reliable)<8:
            continue
        box=[min(p[0] for p in reliable),min(p[1] for p in reliable),
             max(p[0] for p in reliable),max(p[1] for p in reliable)]
        facing=None
        if min(joints[i][3] for i in (11,12,23,24))>=.6:
            dx=joints[12][0]-joints[11][0]; dz=joints[12][2]-joints[11][2]
            if math.hypot(dx,dz)>.12:
                facing=math.degrees(math.atan2(-dz,dx))
        candidates.append(dict(joints=joints,image_points=pixels,box=box,facing_deg=facing,
            height_m=None,height_source='assumed',capture_ms=frame['capture_ms'],age_ms=frame['age_ms'],
            floor_position=floor_position(pixels,frame,config),
            frame_id=frame['frame_id'],generation=frame['generation'],coordinate_frame='camera_relative_right_up_forward'))
    costs=[]
    for p in people:
        a=p['box']; row=[]
        for c in candidates:
            b=c['box']; ix=max(0,min(a[2],b[2])-max(a[0],b[0])); iy=max(0,min(a[3],b[3])-max(a[1],b[1]))
            overlap=ix*iy/max(1,(b[2]-b[0])*(b[3]-b[1]))
            hips=[c['image_points'][i] for i in (23,24)]
            inside=all(a[0]<=h[0]<=a[2] and a[1]<=h[1]<=a[3] for h in hips)
            row.append(1-overlap if inside and overlap>.5 else math.inf)
        costs.append(row)
    matches=assignment(costs,1.)
    output=[]
    for i,j in matches.items():
        ranked=sorted(c for c in costs[i] if math.isfinite(c))
        reverse=sorted(row[j] for row in costs if math.isfinite(row[j]))
        if any(len(x)>1 and x[1]-x[0]<.08 for x in (ranked,reverse)):
            continue
        c=dict(candidates[j],camera_id=people[i]['id'])
        floor=c['floor_position']; box=people[i]['box']
        joints=c['joints']
        upright=all(joints[k][3]>.65 for k in (11,12,23,24,25,26,27,28))
        for hip,knee,ankle in ((23,25,27),(24,26,28)):
            a=[joints[hip][n]-joints[knee][n] for n in range(3)]
            b=[joints[ankle][n]-joints[knee][n] for n in range(3)]
            denom=math.sqrt(sum(x*x for x in a)*sum(x*x for x in b))
            upright &= denom>.001 and sum(x*y for x,y in zip(a,b))/max(denom,.001)<-.92
        if floor and upright and box[1]>3 and abs(config.camera_pitch_deg)<1:
            fy=config.camera_fy_px or frame['width']/(2*math.tan(math.radians(frame['hfov']/2)))
            stature=config.camera_height_m-(box[1]-(config.camera_cy_px or frame['height']/2))*floor['forward_m']/fy
            if .8<stature<2.4:c.update(height_m=stature,height_source='floor_calibrated')
        output.append(c)
    return output


class PoseEstimator:
    def __init__(self, model, num_poses=8):
        import mediapipe as mp
        self.mp=mp
        self.landmarker=mp.tasks.vision.PoseLandmarker.create_from_options(
            mp.tasks.vision.PoseLandmarkerOptions(base_options=mp.tasks.BaseOptions(model_asset_path=str(model)),
                running_mode=mp.tasks.vision.RunningMode.VIDEO,num_poses=num_poses,
                min_pose_detection_confidence=.5,min_pose_presence_confidence=.5,min_tracking_confidence=.5))
        self.timestamp=-1

    def infer(self, image, timestamp_ms):
        self.timestamp=max(self.timestamp+1,int(timestamp_ms))
        return self.landmarker.detect_for_video(self.mp.Image(image_format=self.mp.ImageFormat.SRGB,data=image),self.timestamp)

    def close(self):
        self.landmarker.close()


def run_pose(state, pi_http, model, stop):
    import cv2
    import numpy as np
    estimator=None
    last=None
    from .rig_motion import RigMotionMonitor
    motion=RigMotionMonitor()
    try:
        estimator=PoseEstimator(model)
        while not stop.is_set():
            started=time.monotonic()
            try:
                with urlopen(pi_http.rstrip('/')+'/pose/frame',timeout=2) as response:
                    raw=response.read(2*1024*1024+1)
                if len(raw)>2*1024*1024:
                    raise ValueError('Pose response exceeds budget')
                frame=json.loads(raw)
                validate_frame(frame)
                token=(frame['source_session_id'],frame['generation'],frame['frame_id'])
                if token==last or frame['age_ms']>350:
                    stop.wait(.04)
                    continue
                if last and token[:2]!=last[:2]:
                    estimator.close(); estimator=PoseEstimator(model)
                last=token
                image=cv2.imdecode(np.frombuffer(base64.b64decode(frame['jpeg_base64'],validate=True),np.uint8),cv2.IMREAD_COLOR)
                if image is None or image.shape[:2]!=(frame['height'],frame['width']):
                    raise ValueError('Pose frame image/metadata mismatch')
                with state.lock:
                    config=state.engine.config
                    registration=state.registration_generation
                alarm=motion.update(cv2.cvtColor(image,cv2.COLOR_BGR2GRAY),
                    [p['box'] for p in frame['people'] if p.get('observed') and len(p.get('box',[]))==4],
                    (token[:2],registration))
                with state.lock:
                    state.rig_motion=alarm
                if alarm['motion_alarm']:
                    config=replace(config,rig_motion_mode='untracked')
                result=estimator.infer(cv2.cvtColor(image,cv2.COLOR_BGR2RGB),frame['capture_ms'])
                elapsed=(time.monotonic()-started)*1000
                frame['age_ms']+=elapsed
                poses=extract_poses(result,frame,config)
                accepted=state.accept_poses(frame,poses,elapsed)
                state.publish_pose_image(image,poses,frame,accepted)
            except Exception as error:
                with state.lock:
                    state.pose_status=dict(status='unavailable',error=str(error)[:300])
                stop.wait(.5)
            stop.wait(max(0,.08-(time.monotonic()-started)))
    except Exception as error:
        with state.lock:
            state.pose_status=dict(status='unavailable',error=str(error)[:300])
    finally:
        if estimator:
            estimator.close()
