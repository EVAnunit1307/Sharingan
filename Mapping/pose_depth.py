"""Reviewed camera inputs for offline DA3 conditioning; no metric-scale claim."""
import hashlib
import json

import cv2
import numpy as np

from Mapping.camera_calibration import load
from Mapping.da3_inspect import centers, similarity


def world_to_camera(position, quaternion):
    p, q = np.asarray(position, float), np.asarray(quaternion, float)
    if p.shape != (3,) or q.shape != (4,) or not np.isfinite(np.r_[p, q]).all():
        raise ValueError('Invalid tracked camera pose')
    norm = np.linalg.norm(q)
    if abs(norm - 1) > .01:
        raise ValueError('Tracked quaternion is not unit length')
    x, y, z, w = q / norm
    camera_to_world = np.array([
        [1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w)],
        [2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w)],
        [2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y)]])
    out = np.eye(4)
    out[:3, :3] = camera_to_world.T
    out[:3, 3] = -camera_to_world.T @ p
    return out


def tracked_extrinsics(tracking, session_id, rows, indices, calibration_hash):
    if (tracking.get('session_id') != session_id or
            tracking.get('kind') != 'orb_tracking_replay' or
            tracking.get('calibration_sha256') != calibration_hash or
            len(tracking.get('frames', [])) != len(rows)):
        raise ValueError('Tracking provenance does not match this calibrated recording')
    selected = [tracking['frames'][i] for i in indices]
    map_ids = {f['map_id'] for f in selected}
    if len(map_ids) != 1 or None in map_ids:
        raise ValueError('Conditioning requires one retained map')
    for i, frame in zip(indices, selected):
        seconds = (rows[i]['sensor_timestamp_ns'] - rows[0]['sensor_timestamp_ns']) / 1e9
        if (frame['state'] != 2 or frame['position'] is None or frame['quaternion'] is None or
                frame['image'] != rows[i]['image'] or abs(frame['seconds'] - seconds) > 1e-6):
            raise ValueError('Conditioning requires matching, retained tracking poses')
    return np.asarray([world_to_camera(f['position'], f['quaternion']) for f in selected], np.float32)


def normalize_extrinsics(extrinsics):
    # Matches upstream API: first-camera coordinates and torch.median's lower
    # middle element for even batches. Camera translation has arbitrary scale.
    relative = extrinsics @ np.linalg.inv(extrinsics[0])
    distances = np.linalg.norm(centers(relative[:, :3]), axis=1)
    divisor = max(.1, float(np.sort(distances)[(len(distances)-1)//2]))
    relative[:, :3, 3] /= divisor
    return relative, divisor


def rectification_maps(k, distortion, size, center_principal=False):
    output_k = k.copy()
    if center_principal:
        output_k, _ = cv2.getOptimalNewCameraMatrix(k, distortion, size, 0, size,
                                                   centerPrincipalPoint=True)
        # DA3's pose encoder omits principal-point offsets; its decoder uses W/2,H/2.
        output_k[0, 2], output_k[1, 2] = size[0]/2, size[1]/2
        output_k[0, 0] *= 1.01
        output_k[1, 1] *= 1.01
    mx, my = cv2.initUndistortRectifyMap(k, distortion, None, output_k, size, cv2.CV_32FC1)
    valid = (mx >= 0) & (mx <= size[0]-1) & (my >= 0) & (my <= size[1]-1)
    if center_principal and not valid.all():
        raise ValueError('Centered rectification leaves unsupported border pixels')
    return output_k, mx, my, valid


def prepare(session, rows, indices, paths, tracking_path=None, center_principal=False):
    calibration = load(session)
    if calibration is None or calibration.get('status') != 'reviewed_for_recording':
        raise ValueError('Undistortion requires calibration reviewed for this recording')
    fx, fy, cx, cy, *distortion = calibration['params']
    k = np.array([[fx, 0, cx], [0, fy, cy], [0, 0, 1]], np.float32)
    size = calibration['width'], calibration['height']
    output_k, mx, my, valid = rectification_maps(k, np.asarray(distortion), size, center_principal)
    images = []
    for path in paths:
        im = cv2.imread(str(path))
        if im is None or im.shape[:2] != (size[1], size[0]):
            raise ValueError('Image dimensions do not match reviewed calibration')
        corrected = cv2.remap(im, mx, my, cv2.INTER_LINEAR, borderMode=cv2.BORDER_CONSTANT)
        images.append(cv2.cvtColor(corrected, cv2.COLOR_BGR2RGB))
    metadata = dict(calibration_sha256=calibration['source_sha256'],
        undistortion='OpenCV remap, centered crop K' if center_principal else 'OpenCV remap, original K',
        center_principal=center_principal, output_intrinsics=output_k.tolist(),
        principal_point_note='DA3 encodes focal length but not principal offset. Centering crops the field of view; extrinsics keep the same optical-axis orientation.' if center_principal else 'Original principal offset is retained. DA3 cannot encode this offset; this is a diagnostic baseline, not a corrected conditioning input.',
        valid_undistorted_fraction=float(valid.mean()),
        intrinsics_resize='Official DA3 InputProcessor scales K with the same resize as the pixels',
        undistorted_rgb_sha256=hashlib.sha256(np.stack(images).tobytes()).hexdigest())
    extrinsics = None
    if tracking_path is not None:
        tracking = json.loads(tracking_path.read_text())
        extrinsics = tracked_extrinsics(tracking, session.name, rows, indices, calibration['source_sha256'])
        metadata.update(tracking_sha256=hashlib.sha256(tracking_path.read_bytes()).hexdigest(),
                        tracking_revision=tracking['revision'],
                        pose_convention='ORB Twc qx,qy,qz,qw inverted to OpenCV Tcw')
    return images, extrinsics, np.repeat(output_k[None], len(images), axis=0), metadata


def anchor_depth(arrays, input_extrinsics, input_intrinsics):
    source, target = centers(input_extrinsics[:, :3]), centers(arrays['extrinsics'])
    if min(np.linalg.norm(source-source.mean(0)), np.linalg.norm(target-target.mean(0))) < 1e-6:
        raise ValueError('Stationary poses cannot establish a depth scale')
    # Same scale direction as the official API (input trajectory -> model
    # trajectory), but explicit all-view Umeyama, without RANSAC or hidden cuts.
    scale, rotation, translation = similarity(source, target)
    if not np.isfinite(scale) or scale <= 0:
        raise ValueError('Invalid trajectory scale')
    residual = np.linalg.norm(scale * source @ rotation.T + translation - target, axis=1)
    spread = np.sqrt(np.mean(np.sum((target-target.mean(0))**2, axis=1)))
    anchored = dict(arrays, depth=arrays['depth'] / scale,
        extrinsics=input_extrinsics[:, :3].copy(), intrinsics=input_intrinsics.copy())
    return anchored, dict(model_units_per_orb_unit=float(scale),
        model_camera_fit_rmse_fraction_of_spread=float(np.sqrt(np.mean(residual**2))/spread),
        method='All-view Umeyama input centers to predicted centers; divide model depth by scale',
        warning='Export cameras and K are supplied, not independently predicted. Camera agreement is imposed; it cannot validate the depth or ORB path. Units remain arbitrary.')
