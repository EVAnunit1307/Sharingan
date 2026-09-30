"""Isolated repeatability experiments. Never publish or overwrite an active map."""
import argparse
import json
from pathlib import Path
import sqlite3
import time

import numpy as np

from Mapping.camera_calibration import fix_intrinsics,load,reader_options
from Mapping.reconstruct import atomic_json,select_model
from Mapping.geometry_quality import compare


def stats(model):
    images=sorted((im for im in model.images.values() if im.has_pose),key=lambda im:im.name)
    centers=np.array([im.projection_center() for im in images]);steps=np.linalg.norm(np.diff(centers,axis=0),axis=1)
    positive=steps[steps>1e-9];typical=float(np.median(positive)) if len(positive) else 0
    tracks=np.array([p.track.length() for p in model.points3D.values()]);errors=np.array([p.error for p in model.points3D.values()])
    return dict(registered_images=model.num_reg_images(),points=model.num_points3D(),
                mean_reprojection_px=model.compute_mean_reprojection_error(),median_point_reprojection_px=float(np.median(errors)),
                median_track_length=float(np.median(tracks)),short_tracks_fraction=float(np.mean(tracks<=3)),
                max_step_over_median=float(steps.max()/typical) if typical else None,
                camera_path=[dict(image=im.name,position=im.projection_center().tolist()) for im in images],
                note='Temporal steps include gaps if some images did not register; no physical units')


def copy_database(source,target):
    with sqlite3.connect(f'file:{source}?mode=ro',uri=True) as src,sqlite3.connect(target) as dst:src.backup(dst)


def build_sift(session,output):
    import pycolmap
    output.mkdir(parents=True,exist_ok=False);database=output/'database.db'
    names=sorted(p.name for p in (session/'images').glob('*.jpg'));calibration=load(session)
    extraction=pycolmap.FeatureExtractionOptions();extraction.num_threads=4;extraction.use_gpu=False
    started=time.perf_counter()
    pycolmap.extract_features(database,session/'images',image_names=names,camera_mode=pycolmap.CameraMode.SINGLE,
                             reader_options=reader_options(calibration),extraction_options=extraction,device=pycolmap.Device.cpu)
    extracted=time.perf_counter()
    pairs={(i,j) for i in range(len(names)) for j in range(i+1,min(i+16,len(names)))}
    pairs|={(min(i,j),max(i,j)) for i in range(0,len(names),25) for j in range(len(names)) if i!=j}
    (output/'pairs.txt').write_text(''.join(names[i]+' '+names[j]+'\n' for i,j in sorted(pairs)))
    matching=pycolmap.FeatureMatchingOptions();matching.num_threads=4;matching.use_gpu=False
    pairing=pycolmap.ImportedPairingOptions();pairing.match_list_path=str(output/'pairs.txt')
    verification=pycolmap.TwoViewGeometryOptions();verification.ransac.random_seed=42
    pycolmap.match_image_pairs(database,matching_options=matching,pairing_options=pairing,
                              verification_options=verification,device=pycolmap.Device.cpu)
    atomic_json(output/'features.json',dict(feature='SIFT',candidate_pairs=len(pairs),calibration=calibration,
                extraction_seconds=extracted-started,matching_verification_seconds=time.perf_counter()-extracted))
    return database


def run(session,source,output,mode,seeds):
    import pycolmap
    output.mkdir(parents=True,exist_ok=False);database=output/'database.db';copy_database(source,database)
    calibrated=load(session) is not None
    report=dict(source_database=str(source),session_id=session.name,mode=mode,calibrated=calibrated,
                pycolmap_version=pycolmap.__version__,trials=[],published=False)
    previous=None
    for seed in seeds:
        folder=output/f'seed-{seed}'
        options=pycolmap.IncrementalPipelineOptions();options.num_threads=4;options.random_seed=seed;options.max_runtime_seconds=180
        if calibrated:fix_intrinsics(options)
        if mode in ('no_fallback','strict'):
            options.structure_less_registration_fallback=False
        if mode=='strict':
            options.mapper.abs_pose_min_num_inliers=60
            options.mapper.abs_pose_max_error=3
            options.mapper.filter_max_reproj_error=2
        if mode=='global':
            options=pycolmap.GlobalPipelineOptions();options.num_threads=4;options.random_seed=seed
            options.mapper.global_positioning.use_gpu=False
            options.mapper.bundle_adjustment.ceres.use_gpu=False
            options.mapper.bundle_adjustment.ceres.solver_options.num_threads=4
            options.mapper.bundle_adjustment.ceres.solver_options.max_solver_time_in_seconds=60
            if calibrated:
                options.mapper.bundle_adjustment.refine_focal_length=False
                options.mapper.bundle_adjustment.refine_principal_point=False
                options.mapper.bundle_adjustment.refine_extra_params=False
        atomic_json(output/f'options-{seed}.json',json.loads(json.dumps(options.todict(),default=str)))
        started=time.perf_counter()
        if mode=='global':models=pycolmap.global_mapping(database,session/'images',folder,options=options)
        else:models=pycolmap.incremental_mapping(database,session/'images',folder,options=options)
        result=dict(seed=seed,components=len(models),mapping_seconds=time.perf_counter()-started)
        model=select_model(models)
        if model:
            result.update(stats(model))
            if previous is not None:result['repeatability']=compare(previous,model)
            model.export_PLY(output/f'seed-{seed}.ply');previous=model
        else:result['failure']='No usable reconstruction'
        report['trials'].append(result);atomic_json(output/'report.json',report)
        print(json.dumps({k:v for k,v in result.items() if k!='camera_path'}),flush=True)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--session',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True);p.add_argument('--source',type=Path)
    p.add_argument('--sift',action='store_true');p.add_argument('--mode',choices=('baseline','no_fallback','strict','global'),default='no_fallback')
    p.add_argument('--seeds',type=int,nargs='+',default=[42,43]);a=p.parse_args()
    if a.sift:a.source=build_sift(a.session,a.output.parent/(a.output.name+'-features'))
    if a.source is None:p.error('--source or --sift is required')
    run(a.session,a.source,a.output,a.mode,a.seeds)
