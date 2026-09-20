"""Persistent identities and conservative motion estimates over validated fusion.

The v1 observations stay available; tracks are an additive presentation contract.
Memory may outlive a sample for association, but never makes it renderable.
"""
import copy
import math
from SensorRig.CV.tracking_math import MotionFilter


class PeopleTracker:
    def __init__(self):
        self.tracks = {}
        self.aliases = {}
        self.next_id = 1
        self.poses = {}
        self.decisions = []
        self.reference = None

    def reset(self, reference=None):
        self.tracks.clear()
        self.aliases.clear()
        self.poses.clear()
        self.reference = reference
        # Never reuse an ID within the same relay process.

    def attach_pose(self, key, pose, at):
        old = self.poses.get(key)
        if old and pose['capture_ms'] <= old['capture_ms']:
            return False
        self.poses[key] = dict(copy.deepcopy(pose), received_at=at)
        return True

    def _track(self, keys, x, y, at, sigma, now):
        known = sorted({self.aliases[k] for k in keys if k in self.aliases})
        if known:
            # Preserve the older identity when an initially independent pair joins.
            track_id = known[0]
            for obsolete in known[1:]:
                self.tracks.pop(obsolete, None)
                for k,v in list(self.aliases.items()):
                    if v == obsolete:
                        self.aliases[k] = track_id
        else:
            track_id = self.next_id
            self.next_id += 1
            self.tracks[track_id] = dict(filter=MotionFilter(x,y,at,sigma), tokens=set(),
                                        last=now, height=1.65, facing=0., facing_at=-1.e9)
        for k in keys:
            self.aliases[k] = track_id
        return track_id, self.tracks[track_id]

    def snapshot(self, people, radar, camera, now, reference, config, yaw=0.):
        if reference != self.reference:
            self.reset(reference)
        for tid,t in list(self.tracks.items()):
            if now-t['last'] > 1.0:
                del self.tracks[tid]
        self.aliases = {k:v for k,v in self.aliases.items() if v in self.tracks}
        self.poses = {k:p for k,p in self.poses.items() if now-p['received_at'] < 1.0}
        self.decisions = []
        output = []
        observations = []
        for p in people:
            keys = [('C',p['camera_generation'],p['camera_id'])]
            matched = p['position_source']=='radar_matched'
            if matched:
                keys.append(('R',p['radar_generation'],p['radar_id']))
            source = 'radar_matched' if matched else 'camera_estimate'
            age = p['radar_age_ms'] if matched else p['camera_age_ms']
            token = (keys[-1],p['radar_frame_id'] if matched else p['camera_frame_id'])
            observations.append((p,keys,source,age,token,.20 if matched else 1.0))
        for r in radar:
            keys = [('R',r['generation'],r['id'])]
            observations.append((r,keys,'radar_only',r['age_ms'],(keys[0],r['frame_id']),.20))

        for p,keys,source,age,token,sigma in observations:
            if len(output) >= config.max_people:
                break
            observed_at = now-age/1000
            tid,t = self._track(keys,p['right_m'],p['forward_m'],observed_at,sigma,now)
            if any(o['id']==tid for o in output):
                continue
            if token not in t['tokens']:
                t['tokens'].add(token)
                if len(t['tokens'])>64:
                    t['tokens']={token}
                # A first radar range replaces an uncertain monocular range once.
                # It must not imply a metre-per-frame walking velocity.
                if config.rig_motion_mode=='left_controller':
                    # Filtering a rotating local frame adds lag and mistakes rig
                    # motion for walking. Quest filters positions after applying
                    # its capture-time controller transform.
                    t['filter']=MotionFilter(p['right_m'],p['forward_m'],observed_at,sigma)
                    t.pop('rejected_token',None)
                elif source!='camera_estimate' and t.get('source')=='camera_estimate':
                    t['filter']=MotionFilter(p['right_m'],p['forward_m'],observed_at,sigma)
                elif observed_at > t['filter'].at:
                    if not t['filter'].update(p['right_m'],p['forward_m'],observed_at,sigma):
                        t['rejected_token']=token
                t['source']=source
            if t.get('rejected_token')==token:
                self.decisions.append(dict(id=tid,reason='position innovation outside motion gate',source=source))
                continue
            t['last']=now
            state=t['filter'].state()
            # A stale radar filter must not masquerade as a fresh camera estimate.
            if source=='camera_estimate' and now-t['filter'].at>.75:
                t['filter']=MotionFilter(p['right_m'],p['forward_m'],observed_at,sigma)
                state=t['filter'].state()
            speed=math.hypot(state['velocity_right_mps'],state['velocity_forward_mps'])
            if speed>.18 and now-t['facing_at']>1.0:
                t['facing']=math.degrees(math.atan2(state['velocity_right_mps'],state['velocity_forward_mps']))
            pose=next((self.poses[k] for k in keys if k in self.poses),None)
            # Recover a recently seen camera pose through the persistent radar alias.
            if pose is None:
                pose=next((self.poses[k] for k,v in self.aliases.items() if v==tid and k in self.poses),None)
            pose_age=(pose['age_ms']+(now-pose['received_at'])*1000) if pose else None
            pose_valid=pose is not None and pose_age<=350
            if pose_valid:
                if pose.get('facing_deg') is not None:
                    t['facing']=pose['facing_deg']+yaw
                    t['facing_at']=now
                if (pose.get('height_m') is not None and pose.get('height_source')=='floor_calibrated'
                        and t.get('height_frame')!=pose['capture_ms']):
                    t['height']=.9*t['height']+.1*pose['height_m']
                    t['height_source']='camera_floor_estimate'
                    t['height_frame']=pose['capture_ms']
            camera_visible=source!='radar_only'
            bearing=math.degrees(math.atan2(state['right_m']-config.camera_offset_right_m,
                                          state['forward_m']-config.camera_offset_forward_m))-yaw
            hfov=camera.metadata['hfov'] if camera else 0
            in_fov=bool(camera and camera.age(now)<=750 and abs(bearing)<hfov/2-5)
            support='camera_observed' if camera_visible else 'previously_seen' if any(k[0]=='C' and v==tid for k,v in self.aliases.items()) else 'unverified'
            # FOV alone cannot establish an unobstructed view. Report the miss,
            # but do not suppress a through-wall/off-camera independent return.
            visibility='camera_observed' if camera_visible else 'in_fov_occlusion_unknown' if in_fov else 'outside_or_camera_unavailable'
            if source=='radar_only' and in_fov:
                self.decisions.append(dict(id=tid,reason='camera absent; occlusion unknown, radar retained'))
            valid_for=(750 if source=='camera_estimate' else 500)-age
            row=dict(id=tid,**state,position_source=source,age_ms=age,valid_for_ms=max(0,valid_for),
                sample_key='/'.join(map(str,token[0]))+'/'+str(token[1]),
                camera_id=p.get('camera_id'),camera_generation=p.get('camera_generation'),
                radar_id=p.get('radar_id') if source=='radar_matched' else p.get('id') if source=='radar_only' else None,
                radar_generation=p.get('radar_generation') if source=='radar_matched' else p.get('generation'),
                height_m=t['height'],height_source=t.get('height_source','assumed'),
                facing_deg=t['facing'],facing_source='camera' if pose_valid and pose.get('facing_deg') is not None else 'estimated_motion',
                pose_source='camera' if pose_valid else 'estimated_motion',pose_age_ms=pose_age,pose_yaw_deg=yaw,
                person_evidence=support,camera_visibility=visibility,track_quality='observed',
                pose=copy.deepcopy(pose) if pose_valid else None)
            if row['pose']:
                row['pose'].pop('received_at',None)
                row['pose']['age_ms']=pose_age
            output.append(row)
        return output
