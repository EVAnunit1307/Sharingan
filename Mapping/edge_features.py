"""Experimental XFeat ONNX front end with NumPy/OpenCV postprocessing, no Torch.

Network weights are upstream XFeat; remap interpolation is an approximation to
PyTorch grid_sample and must be benchmarked for matching quality before replacing
the established mapper. Input geometry is fixed by the exported ONNX model.
"""
import cv2
import numpy as np


def sample_grid(array,xy,width,height,mode):
    """Upstream grid_sample coordinates; bicubic coefficient -0.75, zero padding."""
    if array.ndim==2:array=array[...,None]
    coords=xy/np.array([width-1,height-1],np.float32)*np.array([array.shape[1],array.shape[0]],np.float32)-.5
    def fetch(x,y):
        valid=(x>=0)&(x<array.shape[1])&(y>=0)&(y<array.shape[0])
        values=array[np.clip(y,0,array.shape[0]-1),np.clip(x,0,array.shape[1]-1)]
        return values*valid[:,None]
    if mode=='nearest':
        indices=np.rint(coords).astype(int);return fetch(indices[:,0],indices[:,1])
    base=np.floor(coords).astype(int)
    offsets=(0,1) if mode=='bilinear' else (-1,0,1,2)
    def weight(distance):
        t=np.abs(distance)
        if mode=='bilinear':return np.maximum(1-t,0)
        return np.where(t<=1,1.25*t**3-2.25*t**2+1,
                        np.where(t<2,-.75*t**3+3.75*t**2-6*t+3,0))
    out=np.zeros((len(xy),array.shape[2]),dtype=np.float32)
    for dx in offsets:
        x=base[:,0]+dx;wx=weight(coords[:,0]-x)
        for dy in offsets:
            y=base[:,1]+dy;w=wx*weight(coords[:,1]-y)
            out+=fetch(x,y)*w[:,None]
    return out


class EdgeFeatures:
    def __init__(self,path,threads=2,top_k=1024):
        import onnxruntime as ort
        options=ort.SessionOptions();options.intra_op_num_threads=threads;options.inter_op_num_threads=1
        self.session=ort.InferenceSession(str(path),sess_options=options,providers=['CPUExecutionProvider'])
        self.top_k=top_k
        _,_,self.height,self.width=self.session.get_inputs()[0].shape

    def extract(self,bgr):
        if bgr.shape[:2]!=(self.height,self.width):raise ValueError('Frame geometry differs from exported network')
        tensor=np.ascontiguousarray(cv2.cvtColor(bgr,cv2.COLOR_BGR2RGB).transpose(2,0,1)[None],dtype=np.float32)/255.
        features,logits,reliability=self.session.run(None,{'image':tensor})
        features=features[0];features/=np.maximum(np.linalg.norm(features,axis=0,keepdims=True),1e-12)
        logits=logits[0];prob=np.exp(logits-logits.max(axis=0,keepdims=True));prob/=prob.sum(axis=0,keepdims=True)
        h,w=logits.shape[1:]
        heat=prob[:64].transpose(1,2,0).reshape(h,w,8,8).transpose(0,2,1,3).reshape(h*8,w*8)
        y,x=np.where((heat>.05)&(heat==cv2.dilate(heat,np.ones((5,5),np.uint8))))
        keep=(x>0)|(y>0);x,y=x[keep],y[keep]
        xy=np.column_stack([x,y]).astype(np.float32)
        scores=sample_grid(heat,xy,self.width,self.height,'nearest').reshape(-1)*sample_grid(reliability[0,0],xy,self.width,self.height,'bilinear').reshape(-1)
        idx=np.argsort(-scores)[:self.top_k];idx=idx[scores[idx]>0];xy=xy[idx];scores=scores[idx]
        descriptors=sample_grid(features.transpose(1,2,0).copy(),xy,self.width,self.height,'bicubic')
        descriptors/=np.maximum(np.linalg.norm(descriptors,axis=1,keepdims=True),1e-12)
        return dict(keypoints=xy,scores=scores,descriptors=descriptors)


def mutual_matches(first,second,threshold=.82):
    if not len(first['descriptors']) or not len(second['descriptors']):return np.empty((0,2),dtype=int)
    similarity=first['descriptors']@second['descriptors'].T
    best=similarity.argmax(axis=1);reverse=similarity.argmax(axis=0);a=np.arange(len(best))
    valid=(reverse[best]==a)&(similarity[a,best]>=threshold)
    return np.column_stack([a[valid],best[valid]])
