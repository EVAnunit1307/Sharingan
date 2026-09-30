"""Synthetic non-planar textured scene for an explicit, hardware-free reconstruction smoke test.

python -m Mapping.tests.synthetic_walk --output Saved/MappingSmoke
Never serves this dataset as a live room or a physical validation result.
"""
import argparse
import json
from pathlib import Path
import cv2
import numpy as np


def generate(output):
    output.mkdir(parents=True,exist_ok=False)
    images=output/'images';images.mkdir()
    rng=np.random.default_rng(70)
    panels=[]
    for y in np.linspace(-1.8,1.8,7):
        for x in np.linspace(-3.6,3.6,13):
            z=rng.uniform(4,7)
            texture=rng.integers(25,235,(48,48,3),dtype=np.uint8)
            texture=cv2.resize(texture,(96,96),interpolation=cv2.INTER_NEAREST)
            for _ in range(8):
                p=tuple(int(v) for v in rng.integers(4,92,2));color=tuple(int(v) for v in rng.integers(0,255,3))
                cv2.circle(texture,p,int(rng.integers(3,10)),color,-1)
            panels.append((z,x,y,texture))
    frames=[]
    for i,xpos in enumerate(np.linspace(-1.4,1.4,16)):
        center=np.array([xpos,.1*np.sin(i*.3),.15*np.cos(i*.4)])
        image=np.full((480,640,3),20,np.uint8)
        for z,x,y,texture in sorted(panels,reverse=True,key=lambda p:p[0]):
            corners=np.array([[x-.27,y-.27,z],[x+.27,y-.27,z],[x+.27,y+.27,z],[x-.27,y+.27,z]])-center
            projected=(corners[:,:2]/corners[:,2,None]*500+np.array([320,240])).astype(np.float32)
            transform=cv2.getPerspectiveTransform(np.array([[0,0],[95,0],[95,95],[0,95]],np.float32),projected)
            warped=cv2.warpPerspective(texture,transform,(640,480))
            mask=cv2.warpPerspective(np.full((96,96),255,np.uint8),transform,(640,480))
            image[mask>0]=warped[mask>0]
        name=f'{i+1:09d}.jpg';cv2.imwrite(str(images/name),image)
        frames.append(dict(image='images/'+name,frame_id=i+1,camera_generation=1,
                           host_capture_mono_ns=i*333333333,sensor_timestamp_ns=None,synthetic=True))
    (output/'frames.jsonl').write_text(''.join(json.dumps(row)+'\n' for row in frames))
    (output/'manifest.json').write_text(json.dumps(dict(schema_version=1,session_id=output.name,status='complete',
                                                       frames_saved=len(frames),source='synthetic_test_only',scale='unknown')))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True)
    generate(parser.parse_args().output)
