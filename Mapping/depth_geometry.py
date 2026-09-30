"""Robust depth alignment and independent geometric checks. No neural runtime."""
import numpy as np
import cv2


def sample_image(array, xy, width, height):
    """COLMAP pixel centers start at .5; resize coordinates preserve centers."""
    xy = np.asarray(xy, dtype=np.float64)
    coords = xy*np.array([array.shape[1]/width, array.shape[0]/height])-.5
    finite = np.isfinite(coords).all(axis=1)
    coords[~finite] = -1e6
    sampled = cv2.remap(np.asarray(array,dtype=np.float32),
                        coords[:,0].astype(np.float32).reshape(-1,1),
                        coords[:,1].astype(np.float32).reshape(-1,1),
                        cv2.INTER_LINEAR, borderMode=cv2.BORDER_CONSTANT,
                        borderValue=float('nan')).reshape(-1)
    sampled[~finite] = np.nan
    return sampled


def robust_affine(x, y, positive=True):
    """Deterministic RANSAC initialization then Huber IRLS; x -> y."""
    x,y = np.asarray(x,dtype=float),np.asarray(y,dtype=float)
    valid=np.isfinite(x)&np.isfinite(y)
    x,y=x[valid],y[valid]
    if len(x)<12 or np.ptp(x)<1e-7:
        raise ValueError('Too few varied alignment observations')
    origin=float(np.median(x)); scale=float(np.std(x))
    t=(x-origin)/scale
    design=np.column_stack([t,np.ones(len(t))])
    rng=np.random.default_rng(42)
    best=None
    for pair in rng.integers(0,len(t),size=(200,2)):
        a,b=pair
        if abs(t[a]-t[b])<.05: continue
        slope=(y[a]-y[b])/(t[a]-t[b]); shift=y[a]-slope*t[a]
        if positive and slope<=0: continue
        loss=np.median(np.abs(y-(slope*t+shift)))
        if best is None or loss<best[0]: best=(loss,np.array([slope,shift]))
    if best is None: raise ValueError('Predicted depth ordering does not support alignment')
    params=best[1]
    for _ in range(12):
        residual=y-design@params
        sigma=max(1e-8,1.4826*np.median(np.abs(residual-np.median(residual))))
        weights=np.minimum(1,1.345*sigma/np.maximum(np.abs(residual),1e-12))
        params=np.linalg.lstsq(design*np.sqrt(weights[:,None]), y*np.sqrt(weights),rcond=None)[0]
    slope=float(params[0]/scale);shift=float(params[1]-slope*origin)
    if positive and slope<=0:raise ValueError('Aligned inverse-depth slope must be positive')
    return slope,shift


def inverse_to_depth(prediction,slope,shift):
    inv=np.asarray(prediction)*slope+shift
    return np.divide(1.,inv,out=np.full_like(inv,np.nan,dtype=float),where=inv>1e-8)


def relative_errors(predicted,reference):
    predicted,reference=np.asarray(predicted),np.asarray(reference)
    valid=np.isfinite(predicted)&np.isfinite(reference)&(predicted>0)&(reference>0)
    if not valid.any():return dict(count=0,median=None,p90=None,within_10_percent=0.)
    errors=np.abs(predicted[valid]-reference[valid])/reference[valid]
    return dict(count=int(valid.sum()),median=float(np.median(errors)),
                p90=float(np.percentile(errors,90)),within_10_percent=float(np.mean(errors<.1)))


def voxel_average(xyz,rgb,support,size):
    if not len(xyz): return np.empty((0,3)),np.empty((0,3)),np.empty(0)
    keys=np.floor(np.asarray(xyz)/size).astype(np.int64)
    _,inverse=np.unique(keys,axis=0,return_inverse=True)
    count=np.bincount(inverse)
    out_xyz=np.column_stack([np.bincount(inverse,weights=xyz[:,i])/count for i in range(3)])
    out_rgb=np.column_stack([np.bincount(inverse,weights=rgb[:,i])/count for i in range(3)])
    out_support=np.bincount(inverse,weights=support)/count
    return out_xyz,np.clip(np.rint(out_rgb),0,255).astype(np.uint8),out_support
