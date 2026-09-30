"""Opt-in per-recording lens calibration; metric map scale stays unknown."""
import hashlib
import json
from pathlib import Path

import numpy as np


def load(session):
    path=Path(session)/'camera-calibration.json'
    if not path.is_file():return None
    data=json.loads(path.read_text())
    camera=json.loads((Path(session)/'manifest.json').read_text())['camera']
    if data.get('passes_basic_checks') is not True or data.get('camera_model')!='OPENCV':
        raise ValueError('Lens candidate has not passed basic calibration checks')
    if data.get('pixel_coordinates')!='opencv_integer_pixel_centers':
        raise ValueError('Unknown calibration pixel convention')
    geometry=data.get('processed_geometry',{})
    for key in ('width','height','rotation','source'):
        if key not in geometry or camera.get(key)!=geometry[key]:
            raise ValueError('Calibration and recording camera geometry differ: '+key)
    if (data.get('width'),data.get('height'))!=(camera['width'],camera['height']):
        raise ValueError('Calibration image dimensions differ')
    params=np.asarray(data.get('params',[]),dtype=float)
    if params.shape!=(8,) or not np.isfinite(params).all() or min(params[:2])<=0:
        raise ValueError('Invalid OPENCV lens parameters')
    data['source_sha256']=hashlib.sha256(path.read_bytes()).hexdigest()
    return data


def colmap_params(data):
    params=np.asarray(data['params'],dtype=float).copy()
    params[2:4]+=.5
    return params


def reader_options(data):
    if data is None:return dict(camera_model='SIMPLE_RADIAL')
    return dict(camera_model='OPENCV',camera_params=','.join(map(str,colmap_params(data))))


def fix_intrinsics(options):
    options.ba_refine_focal_length=False
    options.ba_refine_principal_point=False
    options.ba_refine_extra_params=False
    options.mapper.abs_pose_refine_focal_length=False
    options.mapper.abs_pose_refine_extra_params=False


def apply_database(database,data):
    import pycolmap
    with pycolmap.Database.open(database) as db:
        for camera in db.read_all_cameras():
            if (camera.width,camera.height)!=(data['width'],data['height']):
                raise ValueError('Feature database dimensions differ from calibration')
            db.update_camera(pycolmap.Camera(camera_id=camera.camera_id,model='OPENCV',
                width=camera.width,height=camera.height,params=colmap_params(data),has_prior_focal_length=True))
        # Essential matrices and verified poses depend on the previous intrinsics.
        db.clear_two_view_geometries()


def opencv_parameters(camera):
    """K uses COLMAP centers, matching the localizer's +0.5 image observations."""
    if camera.model_name=='SIMPLE_RADIAL':
        f,cx,cy,k=camera.params
        return np.array([[f,0,cx],[0,f,cy],[0,0,1]],float),np.array([k,0,0,0],float)
    if camera.model_name=='OPENCV':
        fx,fy,cx,cy,k1,k2,p1,p2=camera.params
        return np.array([[fx,0,cx],[0,fy,cy],[0,0,1]],float),np.array([k1,k2,p1,p2],float)
    raise ValueError('Unsupported camera model for live localization: '+camera.model_name)
