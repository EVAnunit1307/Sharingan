"""Repeat-build diagnostics. Passing is not proof of physical map accuracy."""
import numpy as np


def compare(reference, trial):
    import pycolmap
    a = {im.name: im for im in reference.images.values() if im.has_pose}
    b = {im.name: im for im in trial.images.values() if im.has_pose}
    names = sorted(a.keys() & b.keys())
    result = dict(common_views=len(names), reference_views=len(a), trial_views=len(b),
                  note='Repeatability after similarity alignment; not metric or physical accuracy')
    if len(names) < 4:
        return dict(result, failure='Fewer than four common views')
    target = np.array([a[n].projection_center() for n in names])
    source = np.array([b[n].projection_center() for n in names])
    if not np.isfinite(target).all() or not np.isfinite(source).all():
        return dict(result, failure='Non-finite camera positions')
    spread = np.sqrt(np.mean(np.sum((target-target.mean(axis=0))**2, axis=1)))
    if spread < 1e-12:
        return dict(result, failure='No reference camera baseline')
    transform = pycolmap.estimate_sim3d(source, target)
    if transform is None:
        return dict(result, failure='Degenerate similarity fit')
    Q = transform.rotation.matrix()
    aligned = transform.scale*(source@Q.T)+transform.translation
    error = np.linalg.norm(aligned-target, axis=1)
    angles = []
    for name in names:
        predicted = b[name].cam_from_world().rotation.matrix()@Q.T
        actual = a[name].cam_from_world().rotation.matrix()
        angles.append(float(np.degrees(np.arccos(np.clip((np.trace(predicted@actual.T)-1)/2, -1, 1)))))
    result.update(center_rmse_fraction_of_path_spread=float(np.sqrt(np.mean(error**2))/spread),
                  center_median_fraction_of_path_spread=float(np.median(error)/spread),
                  rotation_median_degrees=float(np.median(angles)),
                  rotation_p90_degrees=float(np.percentile(angles, 90)))
    return result


def assess(comparison, input_images):
    """Conservative engineering thresholds, not calibrated accuracy bounds."""
    limits = dict(minimum_common_fraction=.9, maximum_path_rmse_fraction=.05,
                  maximum_rotation_p90_degrees=5., minimum_coverage=.8)
    result = dict(schema_version=1, thresholds=limits, comparison=comparison,
                  physical_accuracy='unvalidated', scale='unknown')
    if comparison.get('failure'):
        return dict(result, status='inconclusive', warning='Could not verify repeatability: '+comparison['failure'])
    reference = comparison.get('reference_views', 0)
    trial = comparison.get('trial_views', 0)
    common = comparison.get('common_views', 0)
    rmse = comparison.get('center_rmse_fraction_of_path_spread', float('nan'))
    rotation = comparison.get('rotation_p90_degrees', float('nan'))
    if not reference or not trial or not input_images or not np.isfinite([rmse, rotation]).all():
        return dict(result, status='inconclusive', warning='Repeatability diagnostics are incomplete.')
    if (common/max(reference, trial) < limits['minimum_common_fraction'] or
            rmse > limits['maximum_path_rmse_fraction'] or rotation > limits['maximum_rotation_p90_degrees']):
        return dict(result, status='unstable', warning='Repeated builds disagree; map shape is not reliable yet.')
    if min(reference, trial)/input_images < limits['minimum_coverage']:
        return dict(result, status='repeatable_partial', warning='Repeatable partial reconstruction; much of the walk is missing. Physical shape remains unvalidated.')
    return dict(result, status='repeatable_unvalidated', warning='Repeat-build check passed; physical shape and scale still need measurement.')
