"""COLMAP-only process for research correspondences; avoids Torch/OpenMP conflicts."""
import argparse
import json
from pathlib import Path
import time


def run(session,output,progress_file=None):
    import pycolmap
    from Mapping.reconstruct import atomic_json,select_model
    from Mapping import camera_calibration
    from Mapping.geometry_quality import compare,assess
    calibration=camera_calibration.load(session)
    if calibration:
        camera_calibration.apply_database(output/'database.db',calibration)
        atomic_json(output/'calibration.json',calibration)
    if progress_file:atomic_json(progress_file,dict(state='running',revision=output.name,phase='Checking learned matches against camera geometry'))
    started=time.perf_counter()
    verification=pycolmap.TwoViewGeometryOptions();verification.ransac.random_seed=42
    pycolmap.verify_matches(output/'database.db',output/'pairs.txt',options=verification)
    verified=time.perf_counter()
    if progress_file:atomic_json(progress_file,dict(state='running',revision=output.name,phase='Optimizing the experimental 3D reconstruction'))
    options=pycolmap.IncrementalPipelineOptions();options.num_threads=4;options.random_seed=42
    options.max_runtime_seconds=180
    # Do not join weakly constrained views merely to increase coverage.
    options.structure_less_registration_fallback=False
    if calibration:camera_calibration.fix_intrinsics(options)
    models=pycolmap.incremental_mapping(output/'database.db',session/'images',output/'sparse',options=options)
    mapped=time.perf_counter()
    if progress_file:atomic_json(progress_file,dict(state='running',revision=output.name,phase='Checking whether a second build agrees with the map'))
    options.random_seed=43
    check=pycolmap.incremental_mapping(output/'database.db',session/'images',output/'stability-check',options=options)
    reference,trial=select_model(models),select_model(check)
    comparison=compare(reference,trial) if reference is not None and trial is not None else {'failure':'One build produced no usable model'}
    quality=assess(comparison,len(list((session/'images').glob('*.jpg'))))
    quality.update(seeds=[42,43],structure_less_registration_fallback=False)
    atomic_json(output/'quality.json',quality)
    finished=time.perf_counter()
    data={'timing_seconds':{'verification':verified-started,'mapping':mapped-verified,'repeatability_check':finished-mapped},
          'quality_status':quality['status'],
          'components':[{'component':k,'registered_images':m.num_reg_images(),'points':m.num_points3D(),
                         'mean_reprojection_error_px':m.compute_mean_reprojection_error(),
                         'intrinsics':[c.params.tolist() for c in m.cameras.values()]} for k,m in models.items()]}
    (output/'geometry.json').write_text(json.dumps(data,indent=2)+'\n')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--session',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--progress-file',type=Path)
    args=parser.parse_args();run(args.session,args.output,args.progress_file)
