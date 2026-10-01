"""Small, explicit pose boundary for operator-map observations.

Today: recorded visual estimates and synthetic observations in arbitrary units.
Later: a calibrated visual-inertial estimator can publish the same pose shape.
This module neither integrates raw IMU data nor invents missing poses/scale.
"""
from dataclasses import dataclass
import numpy as np


@dataclass(frozen=True)
class PoseSample:
    map_id: str
    clock_id: str
    timestamp_ns: int
    units: str
    source: str
    position: np.ndarray
    rotation_map_from_camera: np.ndarray
    tracking: bool = True
    imu_used: bool = False


def place_observation(point_sensor, *, pose, timestamp_ns, map_id, clock_id,
                      units, camera_from_sensor, max_age_ns=100_000_000):
    """Place an observation only with compatible, fresh pose and explicit mount.

    No arbitrary-to-metre conversion or guessed timestamp synchronization.
    Callers must obtain a pose at measurement time (interpolation is a later
    estimator responsibility). Lost/stale/mismatched observations are withheld.
    """
    if pose is None or not pose.tracking:return None
    if (map_id,clock_id,units)!=(pose.map_id,pose.clock_id,pose.units):return None
    # Nanoseconds must stay integral: NaN can otherwise bypass comparisons,
    # and floats lose timestamp precision. Cast NumPy integers before subtraction
    # so a difference cannot wrap around at the fixed-width integer boundary.
    timing=(timestamp_ns,pose.timestamp_ns,max_age_ns)
    if any(isinstance(t,(bool,np.bool_)) or not isinstance(t,(int,np.integer)) or t<0 for t in timing):
        return None
    if abs(int(timestamp_ns)-int(pose.timestamp_ns))>int(max_age_ns):return None
    rotation=np.asarray(pose.rotation_map_from_camera,float)
    position=np.asarray(pose.position,float);mount=np.asarray(camera_from_sensor,float)
    point=np.asarray(point_sensor,float)
    if rotation.shape!=(3,3) or position.shape!=(3,) or point.shape!=(3,) or mount.shape!=(4,4):
        raise ValueError('Invalid pose, point or mount shape')
    if not all(np.isfinite(x).all() for x in (rotation,position,mount,point)):
        raise ValueError('Non-finite observation geometry')
    for r in (rotation,mount[:3,:3]):
        if not np.allclose(r.T@r,np.eye(3),atol=1e-4) or not np.isclose(np.linalg.det(r),1,atol=1e-4):
            raise ValueError('Expected a proper rigid rotation')
    if not np.allclose(mount[3],[0,0,0,1]):raise ValueError('Invalid homogeneous mount transform')
    camera=mount[:3,:3]@point+mount[:3,3]
    return rotation@camera+position
