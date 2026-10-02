"""Conservative offline DA3 window stitching on CPU/Metal-produced predictions.

This is a bounded experiment, not upstream DA3-Streaming or a SLAM estimator.
It has no loop closure, global optimization, inertial fusion or metric scale.
"""
import argparse
import json
from pathlib import Path

import numpy as np

from Mapping.da3_inspect import centers, diagnostics, similarity, jpeg_data
from Mapping.reference_benchmark import trajectory_metrics, write_json


def load_trial(path):
    path=Path(path)
    report=json.loads((path/'summary.json').read_text())
    if report['state']!='complete':raise ValueError('Incomplete window')
    arrays=dict(np.load(path/'prediction.npz',allow_pickle=False))
    if any(not np.isfinite(v).all() for v in arrays.values()):raise ValueError('Nonfinite prediction')
    if (arrays['depth']<=0).any():raise ValueError('Nonpositive depth')
    return dict(path=path,report=report,arrays=arrays)


def apply_similarity(points, transform):
    scale,rotation,translation=transform
    return scale*np.asarray(points)@rotation.T+translation


def compose(outer, inner):
    a,r,t=outer;b,q,u=inner
    return a*b,r@q,a*r@u+t


def robust_similarity(source, target, tolerance):
    source,target=np.asarray(source,float),np.asarray(target,float)
    if source.shape!=target.shape or source.ndim!=2 or source.shape[1]!=3 or len(source)<12:
        raise ValueError('Need matching Nx3 overlap points')
    if not np.isfinite(source).all() or not np.isfinite(target).all() or not np.isfinite(tolerance) or tolerance<=0:
        raise ValueError('Invalid overlap geometry or tolerance')
    for points in (source,target):
        singular=np.linalg.svd(points-points.mean(0),compute_uv=False)
        if singular[0]<1e-8 or singular[1]<1e-4*singular[0]:raise ValueError('Degenerate overlap')
    rng=np.random.default_rng(42);best=np.zeros(len(source),bool)
    for _ in range(80):
        ids=rng.choice(len(source),3,replace=False)
        if np.linalg.norm(np.cross(source[ids[1]]-source[ids[0]],source[ids[2]]-source[ids[0]]))<1e-8:continue
        fit=similarity(source[ids],target[ids])
        if not np.isfinite(fit[0]) or fit[0]<=0:continue
        mask=np.linalg.norm(apply_similarity(source,fit)-target,axis=1)<tolerance
        if mask.sum()>best.sum():best=mask
    if best.sum()<12:raise ValueError('Insufficient consistent overlap')
    fit=None
    for _ in range(5):
        fit=similarity(source[best],target[best])
        mask=np.linalg.norm(apply_similarity(source,fit)-target,axis=1)<tolerance
        if mask.sum()<12:raise ValueError('Overlap refinement lost support')
        if np.array_equal(mask,best):break
        best=mask
    return similarity(source[best],target[best]), float(best.mean())


def world_points(data, i, ys, xs):
    pixels=np.stack([xs,ys,np.ones_like(xs)],axis=-1).reshape(-1,3)
    camera=(pixels@np.linalg.inv(data['intrinsics'][i]).T)*data['depth'][i,ys,xs].reshape(-1,1)
    ex=data['extrinsics'][i]
    return (camera-ex[:,3])@ex[:,:3]


def overlap_transform(reference, moving):
    ar,br=reference['report'],moving['report'];a,b=reference['arrays'],moving['arrays']
    if ar['session_id']!=br['session_id'] or a['depth'].shape[1:]!=b['depth'].shape[1:]:
        raise ValueError('Windows must use the same recording and image geometry')
    common=sorted(set(ar['indices'])&set(br['indices']))
    if len(common)<4:raise ValueError('Need at least four shared images')
    fit_a=[];fit_b=[];check_a=[];check_b=[]
    h,w=a['depth'].shape[1:];ys,xs=np.mgrid[3:h:9,3:w:9]
    ia=[ar['indices'].index(i) for i in common];ib=[br['indices'].index(i) for i in common]
    for k,(i,j) in enumerate(zip(ia,ib)):
        name_a=Path(ar['frames'][i]['image']).name;name_b=Path(br['frames'][j]['image']).name
        if ar['source_sha256'][name_a]!=br['source_sha256'][name_b]:
            raise ValueError('Shared image hashes disagree')
        pa,pb=world_points(a,i,ys,xs),world_points(b,j,ys,xs)
        valid=(a['confidence'][i,ys,xs].ravel()>=np.percentile(a['confidence'][i],60))
        valid&=(b['confidence'][j,ys,xs].ravel()>=np.percentile(b['confidence'][j],60))
        valid&=(a['depth'][i,ys,xs].ravel()<np.percentile(a['depth'][i],98))
        valid&=(b['depth'][j,ys,xs].ravel()<np.percentile(b['depth'][j],98))
        (fit_a if k%2==0 else check_a).append(pa[valid])
        (fit_b if k%2==0 else check_b).append(pb[valid])
    fit_a,fit_b,check_a,check_b=map(np.concatenate,(fit_a,fit_b,check_a,check_b))
    if min(len(fit_a),len(check_a))<200:raise ValueError('Too few overlap samples')
    depth_scale=float(np.median(a['depth'][ia]))
    transform,inliers=robust_similarity(fit_b,fit_a,.025*depth_scale)
    errors=np.linalg.norm(apply_similarity(check_b,transform)-check_a,axis=1)/depth_scale
    ca,cb=centers(a['extrinsics'])[ia],centers(b['extrinsics'])[ib]
    baseline=float(np.sqrt(np.mean(np.sum((ca-ca.mean(0))**2,axis=1)))/depth_scale)
    positions=np.linalg.norm(apply_similarity(cb,transform)-ca,axis=1)/depth_scale
    rotations_a=a['extrinsics'][ia,:,:3].transpose(0,2,1)
    rotations_b=transform[1]@b['extrinsics'][ib,:,:3].transpose(0,2,1)
    angles=np.degrees(np.arccos(np.clip((np.trace(rotations_a.transpose(0,2,1)@rotations_b,axis1=1,axis2=2)-1)/2,-1,1)))
    check=dict(shared_views=len(common),fit_views=common[::2],validation_views=common[1::2],
        fit_points=len(fit_a),validation_points=len(check_a),fit_inlier_fraction=inliers,
        validation_median_fraction_of_depth=float(np.median(errors)),validation_p90_fraction_of_depth=float(np.percentile(errors,90)),
        camera_p90_fraction_of_depth=float(np.percentile(positions,90)),orientation_p90_degrees=float(np.percentile(angles,90)),
        overlap_baseline_fraction_of_depth=baseline,relative_scale=float(transform[0]),
        note='Alternating shared views fit/check the transform. These correlated image predictions are not independent ground truth.')
    reasons=[]
    if inliers<.6:reasons.append('too few consistent overlap points')
    if np.median(errors)>.03 or np.percentile(errors,90)>.10:reasons.append('held-out overlap surfaces disagree')
    if np.percentile(positions,90)>.03:reasons.append('shared camera positions disagree')
    if np.percentile(angles,90)>10:reasons.append('shared camera orientations disagree')
    if baseline<.01:reasons.append('insufficient translation to check pose consistency')
    if not .2<transform[0]<5:reasons.append('extreme scale change')
    check.update(accepted=not reasons,reasons=reasons)
    return transform,check


def local_screen(data):
    check=diagnostics(data);error=check['median_of_pair_medians_pixels']
    needed=max(2,len(data['images'])//2)
    accepted=error is not None and error<=.02*check['image_width'] and check['pairs_with_at_least_8_matches']>=needed
    return dict(accepted=accepted,diagnostics=check,
                rule='Median pair reprojection <=2% image width; >=8 evaluated matches in at least half the adjacent pairs (minimum two). Heuristic only.')


def replay(paths, output, reference_session=None):
    output=Path(output)
    if output.exists():raise ValueError('Choose a new replay directory')
    output.mkdir(parents=True)
    previous=None;transform=(1.,np.eye(3),np.zeros(3));poses={};clouds=[];steps=[];thumbnails=[]
    base_time=None;seed_count=0;blocked=False;source_session=None;last_start=-1
    for path in paths:
        trial=load_trial(path);report,data=trial['report'],trial['arrays']
        if source_session is None:source_session=report['session_id']
        if report['session_id']!=source_session or report['indices'][0]<=last_start:
            raise ValueError('Replay requires ordered windows from one recording')
        last_start=report['indices'][0]
        if base_time is None:base_time=report['frames'][0]['sensor_timestamp_ns']
        local=local_screen(data)
        step=dict(name=Path(path).name,indices=report['indices'],local=local,
                  observation_end_seconds=(report['frames'][-1]['sensor_timestamp_ns']-base_time)/1e9,
                  state='blocked' if blocked else 'withheld',reasons=[],alignment=None)
        thumbnails.append(jpeg_data(data['images'][-1]))
        if blocked:step['reasons']=['An earlier window failed; no relocalization is implemented']
        elif not local['accepted']:
            step['reasons']=['Local image-consistency screen failed']
            # Before initialization, keep looking for a supported seed. Once a
            # map exists, never silently restart with a different origin/scale.
            blocked=previous is not None
        elif previous is not None:
            try:
                relative,step['alignment']=overlap_transform(previous,trial)
                if step['alignment']['accepted']:
                    transform=compose(transform,relative);step['state']='merged'
                else:step['reasons']=step['alignment']['reasons'];blocked=True
            except ValueError as exc:step['reasons']=[str(exc)];blocked=True
        else:step['state']='seed';seed_count=len(report['indices'])
        if step['state'] in ('seed','merged'):
            c=apply_similarity(centers(data['extrinsics']),transform)
            h,w=data['depth'].shape[1:];ys,xs=np.mgrid[0:h:6,0:w:6]
            for i,index in enumerate(report['indices']):
                if index in poses:continue
                poses[index]=dict(index=index,position=c[i].tolist(),step=len(steps),
                                 seconds=(report['frames'][i]['sensor_timestamp_ns']-base_time)/1e9)
                p=apply_similarity(world_points(data,i,ys,xs),transform)
                valid=(data['confidence'][i,ys,xs].ravel()>=np.percentile(data['confidence'][i],40))
                valid&=(data['depth'][i,ys,xs].ravel()<np.percentile(data['depth'][i],98))
                color=data['images'][i,ys,xs].reshape(-1,3)
                clouds.append(np.column_stack([p[valid],color[valid],np.full(valid.sum(),len(steps))]))
            previous=trial
        step['retained_poses']=len(poses);steps.append(step)
    if not steps:raise ValueError('No replay windows')
    points=np.concatenate(clouds) if clouds else np.empty((0,7))
    if len(points)>60000:points=points[np.linspace(0,len(points)-1,60000,dtype=int)]
    rows=[[round(float(v),5) for v in p[:3]]+[int(v) for v in p[3:]] for p in points]
    result=dict(kind='experimental_window_replay',source_session=source_session,units='arbitrary',imu_used=False,live=False,
        steps=steps,poses=list(poses.values()),points=rows,images=thumbnails,seed_poses=seed_count,
        accepted_windows=sum(s['state'] in ('seed','merged') for s in steps),reference=None,
        warning='Offline stitching with heuristic rejection. No loop closure, global optimization, floor orientation, metric scale or real contact placement.')
    if reference_session and poses:
        ref=json.loads((Path(reference_session)/'reference.json').read_text())
        if Path(reference_session).name!=source_session:raise ValueError('Reference recording mismatch')
        entries=[p for p in poses.values() if ref['frames'][p['index']]['position'] is not None]
        p=np.array([x['position'] for x in entries]);g=np.array([ref['frames'][x['index']]['position'] for x in entries])
        try:
            evaluation=trajectory_metrics(p,g)
            seed_step=min(x['step'] for x in poses.values())
            seed_matched=sum(x['step']==seed_step for x in entries)
            try:evaluation['first_window_alignment']=trajectory_metrics(p,g,seed_matched)
            except ValueError as exc:evaluation['first_window_alignment']=dict(unavailable=str(exc))
            evaluation['indices']=[x['index'] for x in entries]
            # Reference displayed in the same arbitrary frame as the inferred cloud.
            r=np.array(evaluation['alignment_rotation']);t=np.array(evaluation['alignment_translation']);s=evaluation['fitted_scale']
            evaluation['reference_in_map']=((g-t)@r/s).tolist()
            result['reference']=evaluation
        except ValueError as exc:result['reference']=dict(unavailable=str(exc))
    write_json(output/'replay.json',result)
    with (output/'inferred-points.ply').open('w') as file:
        file.write(f'ply\nformat ascii 1.0\nelement vertex {len(rows)}\nproperty float x\nproperty float y\nproperty float z\nproperty uchar red\nproperty uchar green\nproperty uchar blue\nend_header\n')
        for p in rows:file.write(' '.join(map(str,p[:6]))+'\n')
    return result


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--trials',nargs='+',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--reference-session',type=Path)
    args=parser.parse_args()
    result=replay(args.trials,args.output,args.reference_session)
    print(json.dumps({k:v for k,v in result.items() if k not in ('points','images','poses')},indent=2))
