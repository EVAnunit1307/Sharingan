"""Conservative image-to-existing-map pose estimates; does not create new geometry."""
from pathlib import Path
import cv2
import numpy as np

from Mapping.depth_fusion import load_model
from Mapping.edge_features import EdgeFeatures
from Mapping.camera_calibration import opencv_parameters


class MapLocalizer:
    def __init__(self,session,network):
        self.scene,self.model=load_model(session)
        self.extractor=EdgeFeatures(network,threads=2,top_k=1024)
        images=sorted((im for im in self.model.images.values() if im.has_pose),key=lambda im:im.name)
        self.camera=self.model.cameras[images[0].camera_id]
        self.K,self.dist=opencv_parameters(self.camera)
        descriptors=[];landmarks=[];ids=[]
        # Bound descriptor matching size. Prefer views spread across the recording.
        step=max(1,int(np.ceil(len(images)/32)))
        for image in images[::step]:
            frame=cv2.imread(str(Path(session)/'images'/image.name));features=self.extractor.extract(frame)
            slots={tuple(p):i for i,p in enumerate(features['keypoints'])}
            for observation in image.points2D:
                if not observation.has_point3D():continue
                point=self.model.points3D[observation.point3D_id]
                if point.error>2 or point.track.length()<3:continue
                index=slots.get(tuple(observation.xy-.5))
                if index is not None:
                    descriptors.append(features['descriptors'][index]);landmarks.append(point.xyz);ids.append(observation.point3D_id)
        self.descriptors=np.asarray(descriptors,dtype=np.float32);self.landmarks=np.asarray(landmarks);self.ids=np.asarray(ids)
        if len(descriptors)<100:raise ValueError('Insufficient descriptors linked to reliable 3D points')

    def locate(self,frame):
        if frame is None or frame.shape[:2]!=(self.camera.height,self.camera.width):
            return dict(state='lost',reason='Camera resolution does not match the map')
        features=self.extractor.extract(frame)
        if len(features['keypoints'])<40:return dict(state='lost',reason='Too few visual features')
        similarity=features['descriptors']@self.descriptors.T
        best=similarity.argmax(axis=1);scores=similarity[np.arange(len(best)),best]
        chosen=[];seen=set()
        for index in np.argsort(-scores):
            if scores[index]<.85:break
            pid=self.ids[best[index]]
            if pid not in seen:seen.add(pid);chosen.append(index)
        chosen=np.array(chosen,dtype=int)
        if len(chosen)<40:return dict(state='lost',reason='Not enough matches to this saved map',matches=len(chosen))
        world=self.landmarks[best[chosen]];xy=features['keypoints'][chosen].astype(float)+.5
        ok,rvec,tvec,inliers=cv2.solvePnPRansac(world,xy,self.K,self.dist,iterationsCount=300,reprojectionError=3.,confidence=.999,flags=cv2.SOLVEPNP_EPNP)
        if not ok or inliers is None or len(inliers)<40:
            return dict(state='lost',reason='Map matches do not establish a reliable pose',matches=len(chosen),inliers=0 if inliers is None else len(inliers))
        indices=inliers.ravel();rvec,tvec=cv2.solvePnPRefineLM(world[indices],xy[indices],self.K,self.dist,rvec,tvec)
        projected,_=cv2.projectPoints(world[indices],rvec,tvec,self.K,self.dist)
        residual=np.linalg.norm(projected[:,0]-xy[indices],axis=1)
        rotation=cv2.Rodrigues(rvec)[0];center=-rotation.T@tvec[:,0]
        span=np.ptp(xy[indices],axis=0)/[self.camera.width,self.camera.height]
        if np.median(residual)>2 or min(span)<.12 or not np.isfinite(center).all():
            return dict(state='lost',reason='Pose support is too narrow or inconsistent',inliers=len(indices))
        return dict(state='tracking',position=center.tolist(),rotation=rotation.tolist(),inliers=len(indices),
                    matches=len(chosen),median_reprojection_px=float(np.median(residual)),
                    note='Pose estimate in an existing map with unknown metric scale; not independent location accuracy or flight control')
