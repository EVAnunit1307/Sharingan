"""Reconstruct a recorded walk on the laptop CPU. Output has unknown metric scale."""
import argparse
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import sys
import uuid


def atomic_json(path, data):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(data, allow_nan=False) + '\n')
    temporary.replace(path)


def select_model(reconstructions):
    # Registration alone is insufficient: the real first walk returned a
    # 21-camera component with just one point alongside a smaller viable model.
    candidates = [r for r in reconstructions.values() if r.num_reg_images() >= 3 and r.num_points3D() >= 20]
    return max(candidates, key=lambda r: (r.num_reg_images(), r.num_points3D()), default=None)


def reconstruct(session):
    import pycolmap
    from Mapping import camera_calibration
    calibration=camera_calibration.load(session)

    manifest = json.loads((session/'manifest.json').read_text())
    names = sorted(p.name for p in (session/'images').glob('*.jpg'))
    if len(names) < 12:
        raise ValueError('Record at least 12 images while translating the camera; 60–120 seconds is recommended')
    if manifest.get('status') not in ('complete', 'interrupted'):
        raise ValueError('Finish the recording before reconstruction')
    revision = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ-') + uuid.uuid4().hex[:8]
    output = session/'reconstructions'/revision
    output.mkdir(parents=True, exist_ok=False)
    database, sparse = output/'database.db', output/'sparse'
    sparse.mkdir()
    if calibration:atomic_json(output/'calibration.json',calibration)

    def progress(phase):
        atomic_json(session/'reconstruction.json', dict(state='running', phase=phase, revision=revision))

    progress('Extracting image features on the laptop CPU')
    extraction = pycolmap.FeatureExtractionOptions()
    extraction.num_threads = 4
    extraction.use_gpu = False
    pycolmap.extract_features(database, session/'images', image_names=names,
                             camera_mode=pycolmap.CameraMode.SINGLE,
                             reader_options=camera_calibration.reader_options(calibration),
                             extraction_options=extraction, device=pycolmap.Device.cpu)
    progress('Matching overlapping images')
    matching = pycolmap.FeatureMatchingOptions()
    matching.num_threads = 4
    matching.use_gpu = False
    pairing = pycolmap.SequentialPairingOptions()
    pairing.overlap = 15
    pairing.loop_detection = False  # No hidden vocabulary/model download.
    pycolmap.match_sequential(database, matching_options=matching, pairing_options=pairing,
                             device=pycolmap.Device.cpu)
    progress('Estimating camera positions and sparse 3D structure')
    options = pycolmap.IncrementalPipelineOptions()
    options.num_threads = 4
    if calibration:camera_calibration.fix_intrinsics(options)
    reconstructions = pycolmap.incremental_mapping(database, session/'images', sparse, options=options)
    model = select_model(reconstructions)
    strategy = 'sequential'
    # Bound the quadratic retry to short baseline walks. This connects revisited
    # views outside the sequential window without lowering geometry thresholds.
    if len(names) <= 400 and (model is None or model.num_reg_images() < len(names)*.5):
        progress('Matching revisited views across the whole walk')
        pycolmap.match_exhaustive(database, matching_options=matching, device=pycolmap.Device.cpu)
        progress('Reconstructing with the broader image matches')
        retry_sparse = output/'sparse_exhaustive'
        retry_sparse.mkdir()
        retry = pycolmap.incremental_mapping(database, session/'images', retry_sparse, options=options)
        retry_model = select_model(retry)
        if retry_model is not None and (model is None or
                (retry_model.num_reg_images(), retry_model.num_points3D()) > (model.num_reg_images(), model.num_points3D())):
            reconstructions, model, strategy = retry, retry_model, 'sequential_then_exhaustive'
    return export_reconstruction(session, output, reconstructions, strategy)


def export_reconstruction(session, output, reconstructions, strategy='sequential', experimental=False):
    import numpy as np
    import pycolmap

    manifest = json.loads((session/'manifest.json').read_text())
    names = sorted(p.name for p in (session/'images').glob('*.jpg'))
    revision = output.name
    model = select_model(reconstructions)
    if model is None:
        raise ValueError('Too little connected geometry for a useful baseline; inspect reconstruction.log and record another walk')
    # Coverage is a diagnostic label, not a physical accuracy acceptance test.
    partial = model.num_reg_images() < .8*len(names)
    model.export_PLY(output/'points.ply')
    cameras = [dict(image=image.name, position=image.projection_center().tolist())
               for image in sorted(model.images.values(), key=lambda i: i.name) if image.has_pose]
    points = []
    stride = max(1, math.ceil(model.num_points3D()/25000))
    for index, point in enumerate(model.points3D.values()):
        if index % stride == 0 and np.isfinite(point.xyz).all():
            points.append([*[round(float(v), 6) for v in point.xyz], *[int(v) for v in point.color]])
    scene = dict(schema_version=1, map_id=manifest['session_id'], revision=revision,
                 source='synthetic_test_only' if manifest.get('source') == 'synthetic_test_only' else 'drone_camera_colmap',
                 scale='unknown', units='arbitrary',
                 kind='sparse_reconstruction', coordinate_frame='colmap_world_unaligned',
                 created_at=datetime.now(timezone.utc).isoformat(),
                 registered_images=model.num_reg_images(), input_images=len(names),
                 coverage='partial' if partial else 'broad_unvalidated', matching_strategy=strategy,
                 experimental=experimental,
                 points_total=model.num_points3D(), mean_reprojection_error_px=model.compute_mean_reprojection_error(),
                 components=len(reconstructions), points=points, cameras=cameras,
                 note='Largest connected component. No measured metres, gravity alignment, wall mesh or live drone pose.')
    if experimental:
        scene.update(feature_method='XFeat',validation='Uncalibrated; physical accuracy and solver stability need independent checks.')
    if (output/'calibration.json').is_file():
        calibration=json.loads((output/'calibration.json').read_text())
        scene.update(lens_calibration=dict(source_session=calibration['source_session'],
                     source_sha256=calibration['source_sha256'],fixed_intrinsics=True,
                     held_out_median_px=calibration.get('held_out_median_px')),
                     validation='Checkerboard lens estimate applied; map scale, shape and solver stability remain unvalidated.')
    if (output/'quality.json').is_file():
        scene['geometry_quality']=json.loads((output/'quality.json').read_text())
        scene['quality_warning']=scene['geometry_quality']['warning']
    atomic_json(output/'scene.json', scene)
    atomic_json(session/'scene.json', scene)
    atomic_json(session/'reconstruction.json', dict(state='complete',
                                                    phase='Experimental XFeat reconstruction — inspect before relying on its shape' if experimental else
                                                          'Partial reconstruction — most of the walk is missing' if partial else 'Ready to inspect',
                                                    experimental=experimental,
                                                    coverage=scene['coverage'], matching_strategy=strategy, revision=revision,
                                                    registered_images=model.num_reg_images(), input_images=len(names),
                                                    points=model.num_points3D(), components=len(reconstructions),
                                                    pycolmap_version=pycolmap.__version__, output=str(output.relative_to(session))))
    return scene


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--session', required=True, type=Path)
    parser.add_argument('--backend',choices=('standard','xfeat'),default='standard')
    args = parser.parse_args()
    try:
        if args.backend=='xfeat':
            from Mapping.learned import reconstruct as learned_reconstruct
            learned_reconstruct(args.session)
        else:
            reconstruct(args.session)
    except Exception as exc:
        atomic_json(args.session/'reconstruction.json', dict(state='failed', phase=str(exc)))
        print(str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
