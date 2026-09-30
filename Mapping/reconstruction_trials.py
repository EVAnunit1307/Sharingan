"""Reproducible geometry-speed/keyframe trials from existing verified matches.

Keeps raw captures and active models intact. Timings exclude extraction/matching
and are only comparable to each other, not to end-to-end dashboard builds.
"""
import argparse
import json
from pathlib import Path
import sqlite3
import time

import numpy as np

from Mapping.reconstruct import atomic_json, select_model
from Mapping.depth_fusion import load_model


def choose_overlap_frames(database,rows,max_gap=.75,motion_px=25,min_matches=60):
    with sqlite3.connect(f'file:{database}?mode=ro',uri=True) as db:
        images={name:image_id for image_id,name in db.execute('SELECT image_id,name FROM images')}
        keypoints={image_id:np.frombuffer(data,dtype=np.float32).reshape(count,cols)[:,:2]
                   for image_id,count,cols,data in db.execute('SELECT image_id,rows,cols,data FROM keypoints')}
        matches={pair:np.frombuffer(data,dtype=np.uint32).reshape(count,cols)
                 for pair,count,cols,data in db.execute('SELECT pair_id,rows,cols,data FROM two_view_geometries') if count>0}
    kept=[0];decisions=[]
    for index in range(1,len(rows)):
        previous=kept[-1]
        a=images[Path(rows[previous]['image']).name];b=images[Path(rows[index]['image']).name]
        first,second=sorted([a,b]);pair=first*2147483647+second
        pairs=matches.get(pair,np.empty((0,2),dtype=int))
        motion=float(np.median(np.linalg.norm(keypoints[first][pairs[:,0]]-keypoints[second][pairs[:,1]],axis=1))) if len(pairs) else None
        gap=(rows[index]['host_capture_mono_ns']-rows[previous]['host_capture_mono_ns'])/1e9
        reason='endpoint' if index==len(rows)-1 else 'overlap_lost' if len(pairs)<min_matches else 'motion' if motion>=motion_px else 'max_gap' if gap>=max_gap else None
        if reason:
            if reason=='overlap_lost' and index-1>previous:kept.append(index-1)
            kept.append(index)
        decisions.append(dict(index=index,kept=bool(reason),reason=reason,inliers=len(pairs),motion_px=motion,gap_seconds=gap))
    return kept,decisions


def compare_centers(reference,trial):
    common=sorted(set(im.name for im in reference.images.values() if im.has_pose)&set(im.name for im in trial.images.values() if im.has_pose))
    if len(common)<4:return dict(common_views=len(common))
    def positions(model):return {im.name:im.projection_center() for im in model.images.values() if im.has_pose}
    a,b=positions(reference),positions(trial)
    target=np.array([a[name] for name in common]);source=np.array([b[name] for name in common])
    tc,sc=target.mean(axis=0),source.mean(axis=0);x,y=source-sc,target-tc
    u,s,v=np.linalg.svd(x.T@y);d=np.ones(3);d[-1]=np.linalg.det(u@v)
    rotation=u@np.diag(d)@v;scale=float(np.sum(s*d)/np.sum(x*x))
    distances=np.linalg.norm(scale*x@rotation-y,axis=1)
    spread=float(np.sqrt(np.mean(np.sum(y*y,axis=1))))
    return dict(common_views=len(common),similarity_aligned_center_rmse=float(np.sqrt(np.mean(distances**2))),
                center_rmse_fraction_of_path_spread=float(np.sqrt(np.mean(distances**2))/spread),
                note='Agreement with prior uncalibrated reconstruction, not ground truth')


def run(session,output):
    import pycolmap
    scene,reference=load_model(session)
    source=session/'reconstructions'/scene['revision']/'database.db'
    rows=[json.loads(line) for line in (session/'frames.jsonl').read_text().splitlines() if line.strip()]
    kept,decisions=choose_overlap_frames(source,rows)
    output.mkdir(parents=True,exist_ok=False)
    atomic_json(output/'selection.json',dict(selected_indices=kept,decisions=decisions))
    results=[]
    for label,selection,fast in [('full_reference',list(range(len(rows))),False),('overlap_keyframes',kept,False),('fast_refinement',list(range(len(rows))),True)]:
        trial=output/label;trial.mkdir()
        with sqlite3.connect(source) as original,sqlite3.connect(trial/'database.db') as copied:original.backup(copied)
        options=pycolmap.IncrementalPipelineOptions();options.num_threads=4;options.random_seed=42
        options.image_names=[Path(rows[index]['image']).name for index in selection]
        options.max_runtime_seconds=180
        if fast:
            options.ba_global_frames_ratio=1.3;options.ba_global_points_ratio=1.3
            options.ba_local_max_num_iterations=15;options.ba_global_max_num_iterations=30
            options.ba_local_max_refinements=1;options.ba_global_max_refinements=2
        start=time.perf_counter()
        models=pycolmap.incremental_mapping(trial/'database.db',session/'images',trial/'sparse',options=options)
        elapsed=time.perf_counter()-start;model=select_model(models)
        result=dict(name=label,input_frames=len(selection),mapping_seconds=elapsed,components=len(models),random_seed=42)
        if model:
            model.export_PLY(trial/'points.ply')
            result.update(registered_images=model.num_reg_images(),points=model.num_points3D(),
                          reprojection_error_px=model.compute_mean_reprojection_error(),
                          camera_params=[c.params.tolist() for c in model.cameras.values()],
                          reference_comparison=compare_centers(reference,model))
        else:result['failed']='No usable model'
        results.append(result);atomic_json(output/'results.json',dict(source_revision=scene['revision'],trials=results,timing_scope='Geometry only; existing features/matches reused'))
        print(json.dumps(result),flush=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--session',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();run(args.session,args.output)
