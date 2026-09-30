"""Create data plots of sparse and inferred clouds; not screenshots or ground truth."""
import argparse
import json
from pathlib import Path

import cv2
import numpy as np

from Mapping.depth_fusion import load_model


def run(session,fusion,output):
    _,model=load_model(session)
    points=list(model.points3D.values());sparse=np.array([p.xyz for p in points]);sparse_rgb=np.array([p.color for p in points])
    with np.load(fusion/'inferred.npz') as data:dense=data['xyz'];dense_rgb=data['rgb']
    panels=[]
    images=sorted((im for im in model.images.values() if im.has_pose),key=lambda im:im.name)
    for idx in (0,min(12,len(images)-1),min(24,len(images)-1)):
        image=images[idx];camera=model.cameras[image.camera_id];pose=image.cam_from_world().matrix()
        source=cv2.imread(str(session/'images'/image.name))
        row=[source]
        for xyz,rgb in ((sparse,sparse_rgb),(dense,dense_rgb)):
            cam=xyz@pose[:,:3].T+pose[:,3];uv=camera.img_from_cam(cam)
            valid=np.isfinite(uv).all(axis=1)&(cam[:,2]>0)
            safe=np.where(np.isfinite(uv),uv,-1)
            xy=np.floor(safe).astype(int)
            valid&=(xy[:,0]>=0)&(xy[:,0]<camera.width)&(xy[:,1]>=0)&(xy[:,1]<camera.height)
            order=np.where(valid)[0][np.argsort(cam[valid,2])[::-1]]
            canvas=np.full_like(source,24)
            for index in order:
                cv2.circle(canvas,tuple(xy[index]),1,tuple(int(v) for v in rgb[index,::-1]),-1)
            row.append(canvas)
        panel=np.concatenate(row,axis=1);header=np.full((64,panel.shape[1],3),20,np.uint8)
        for j,label in enumerate(('Normal camera '+image.name,'Triangulated points','AI-inferred points - NOT measured')):
            cv2.putText(header,label,(j*camera.width+10,24),cv2.FONT_HERSHEY_SIMPLEX,.55,(230,230,230),1,cv2.LINE_AA)
        cv2.putText(header,'Reprojected into an original view for inspection; this view is NOT an independent accuracy test.',(10,50),cv2.FONT_HERSHEY_SIMPLEX,.5,(180,180,180),1,cv2.LINE_AA)
        panels.append(np.concatenate([header,panel]))
    output.parent.mkdir(parents=True,exist_ok=True);cv2.imwrite(str(output),np.concatenate(panels))
    print(output)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('session','fusion','output'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();run(a.session,a.fusion,a.output)
