"""Held-out image localization within an existing map; not live SLAM or ground truth."""
import argparse
import json
from pathlib import Path
import time
from urllib.request import urlopen

import cv2
import numpy as np

from Mapping.depth_fusion import load_model
from Mapping.edge_features import EdgeFeatures


def run(session,network,output,live_url=None):
    cv2.setNumThreads(1)
    scene,model=load_model(session)
    images=sorted((image for image in model.images.values() if image.has_pose),key=lambda image:image.name)
    queries=[image for i,image in enumerate(images) if i%5==0]
    references=[image for i,image in enumerate(images) if i%5!=0 and i%3==0]
    extractor=EdgeFeatures(network,threads=2,top_k=1024)
    descriptors=[];landmarks=[];point_ids=[]
    started=time.perf_counter()
    for image in references:
        frame=cv2.imread(str(session/'images'/image.name));features=extractor.extract(frame)
        slots={tuple(p):i for i,p in enumerate(features['keypoints'])}
        for observation in image.points2D:
            if not observation.has_point3D():continue
            point=model.points3D[observation.point3D_id]
            if point.error>2 or point.track.length()<3:continue
            index=slots.get(tuple(observation.xy-.5))
            if index is not None:
                descriptors.append(features['descriptors'][index]);landmarks.append(point.xyz);point_ids.append(observation.point3D_id)
    descriptors=np.asarray(descriptors,dtype=np.float32);landmarks=np.asarray(landmarks);point_ids=np.asarray(point_ids)
    index_seconds=time.perf_counter()-started
    results=[]
    for image in ([None]*30 if live_url else queries):
        begin=time.perf_counter()
        if live_url:
            with urlopen(live_url.rstrip('/')+'/mapping/preview.jpg',timeout=5) as response:
                frame_id=response.headers.get('X-Frame-Id')
                frame=cv2.imdecode(np.frombuffer(response.read(),dtype=np.uint8),cv2.IMREAD_COLOR)
            camera=model.cameras[images[0].camera_id]
        else:
            frame=cv2.imread(str(session/'images'/image.name));camera=model.cameras[image.camera_id]
        features=extractor.extract(frame)
        matches=features['descriptors']@descriptors.T
        best=matches.argmax(axis=1);scores=matches[np.arange(len(best)),best]
        # One observation per 3D point; multiple reference descriptors can represent the same landmark.
        chosen=[];seen=set()
        for index in np.argsort(-scores):
            if scores[index]<.85:break
            pid=point_ids[best[index]]
            if pid not in seen:seen.add(pid);chosen.append(index)
        chosen=np.array(chosen,dtype=int);world=landmarks[best[chosen]];xy=features['keypoints'][chosen].astype(float)+.5
        if camera.model_name!='SIMPLE_RADIAL':raise ValueError('This trial currently expects the existing SIMPLE_RADIAL map')
        f,cx,cy,k=camera.params;K=np.array([[f,0,cx],[0,f,cy],[0,0,1]],float);dist=np.array([k,0,0,0],float)
        result=dict(image=image.name if image else 'live-'+str(frame_id),matches=len(chosen),localized=False)
        if len(chosen)>=12:
            ok,rvec,tvec,inliers=cv2.solvePnPRansac(world,xy,K,dist,iterationsCount=300,reprojectionError=3.,confidence=.999,flags=cv2.SOLVEPNP_EPNP)
            if ok and inliers is not None and len(inliers)>=30:
                indices=inliers.ravel();rvec,tvec=cv2.solvePnPRefineLM(world[indices],xy[indices],K,dist,rvec,tvec)
                projected,_=cv2.projectPoints(world[indices],rvec,tvec,K,dist)
                residual=np.linalg.norm(projected[:,0]-xy[indices],axis=1)
                rotation=cv2.Rodrigues(rvec)[0];center=-rotation.T@tvec[:,0]
                result.update(localized=True,inliers=len(indices),median_reprojection_px=float(np.median(residual)),
                              camera_center_map_units=center.tolist())
                if image:
                    ref_rotation=image.cam_from_world().matrix()[:,:3]
                    angle=float(np.degrees(np.arccos(np.clip((np.trace(rotation@ref_rotation.T)-1)/2,-1,1))))
                    result.update(camera_center_error_map_units=float(np.linalg.norm(center-image.projection_center())),rotation_error_degrees=angle)
        result['processing_ms']=(time.perf_counter()-begin)*1000;results.append(result)
        if live_url:print(json.dumps(result),flush=True);time.sleep(.2)
    good=[result for result in results if result['localized']]
    report=dict(source_revision=scene['revision'],reference_images=len(references),reference_descriptors=len(descriptors),
                unique_landmarks=len(set(point_ids.tolist())),query_images=len(results),localized_images=len(good),
                index_build_seconds=index_seconds,median_processing_ms=float(np.median([r['processing_ms'] for r in results])),
                median_inliers=float(np.median([r['inliers'] for r in good])) if good else None,
                median_rotation_error_degrees=float(np.median([r['rotation_error_degrees'] for r in good])) if good and not live_url else None,
                median_center_error_map_units=float(np.median([r['camera_center_error_map_units'] for r in good])) if good and not live_url else None,
                validation='Live Pi images matched to the saved chair map. No reference trajectory or independent pose accuracy; no new mapping or control.' if live_url else 'Query images excluded from descriptor references, but their observations participated in the original SfM map. Tests localization consistency, NOT independent pose accuracy or unseen-room SLAM.',
                results=results)
    output.parent.mkdir(parents=True,exist_ok=True);output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:v for k,v in report.items() if k!='results'},indent=2))


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('session','network','output'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--pi-http')
    a=p.parse_args();run(a.session,a.network,a.output,a.pi_http)
