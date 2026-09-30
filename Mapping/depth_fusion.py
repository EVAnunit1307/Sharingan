"""Experimental relative-depth fusion against a fixed, uncalibrated COLMAP map.

Uses 80% of eligible point tracks for fitting and holds out the same 20% in every
view. Checks are consistency against reconstructed geometry, not physical truth.
"""
import argparse
from datetime import datetime, timezone
import json
import hashlib
from pathlib import Path
import time
import uuid

import cv2
import numpy as np

from Mapping.depth_geometry import (sample_image, robust_affine, inverse_to_depth,
                                    relative_errors, voxel_average)
from Mapping.reconstruct import atomic_json, select_model


def load_model(session):
    import pycolmap
    scene=json.loads((session/'scene.json').read_text())
    revision=session/'reconstructions'/scene['revision']
    folder=revision/('sparse_exhaustive' if scene['matching_strategy']=='sequential_then_exhaustive' else 'sparse')
    models={int(p.name):pycolmap.Reconstruction(p) for p in folder.iterdir() if p.is_dir() and p.name.isdigit()}
    model=select_model(models)
    if model is None:raise ValueError('No usable sparse reconstruction')
    return scene,model


def write_ply(path,xyz,rgb):
    with path.open('w') as out:
        out.write(f'ply\nformat ascii 1.0\ncomment MODEL INFERRED - NOT MEASURED\nelement vertex {len(xyz)}\n')
        out.write('property float x\nproperty float y\nproperty float z\nproperty uchar red\nproperty uchar green\nproperty uchar blue\nend_header\n')
        for point,color in zip(xyz,rgb):
            out.write(' '.join([*(f'{v:.6f}' for v in point),*(str(int(v)) for v in color)])+'\n')


def run(session,cache,output,publish=False,stride=6,relative_tolerance=.08,minimum_neighbors=2,cycle_pixels=2.5):
    if stride<3:raise ValueError('Use a grid stride of at least 3 to bound experiment size')
    started=time.perf_counter()
    scene,model=load_model(session)
    output.mkdir(parents=True,exist_ok=False)
    records=[]; usable={}; errors_all=[]; errors_baseline=[]
    for image in sorted(model.images.values(),key=lambda im:im.name):
        if not image.has_pose: continue
        camera=model.cameras[image.camera_id]
        cache_file=cache/(Path(image.name).stem+'.npz')
        record=dict(image=image.name,accepted=False)
        records.append(record)
        if not cache_file.is_file():record['reason']='No cached prediction';continue
        with np.load(cache_file,allow_pickle=False) as data:
            prediction=np.asarray(data['prediction'],dtype=np.float32)
            if str(data['image_sha256'])!=hashlib.sha256((session/'images'/image.name).read_bytes()).hexdigest():
                record['reason']='Cached prediction belongs to a different source image';continue
        transform=image.cam_from_world().matrix();rotation=transform[:,:3];translation=transform[:,3]
        ids=[];xy=[];xyz=[]
        for p in image.points2D:
            if not p.has_point3D():continue
            point=model.points3D[p.point3D_id]
            if point.error>2.5 or point.track.length()<3:continue
            ids.append(p.point3D_id);xy.append(p.xy);xyz.append(point.xyz)
        if len(ids)<60:record['reason']='Fewer than 60 reliable point observations';continue
        ids,xy,xyz=np.asarray(ids),np.asarray(xy),np.asarray(xyz)
        z=(xyz@rotation.T+translation)[:,2]
        predicted=sample_image(prediction,xy,camera.width,camera.height)
        finite=np.isfinite(predicted)&np.isfinite(z)&(z>0)
        train=finite&(ids%5!=0);test=finite&(ids%5==0)
        record.update(fit_points=int(train.sum()),held_out_points=int(test.sum()))
        if train.sum()<40 or test.sum()<10:record['reason']='Insufficient fit or held-out points';continue
        try:slope,shift=robust_affine(predicted[train],1/z[train])
        except ValueError as exc:record['reason']=str(exc);continue
        aligned=inverse_to_depth(predicted,slope,shift)
        fit=relative_errors(aligned[train],z[train])
        held=relative_errors(aligned[test],z[test])
        baseline=relative_errors(np.full(test.sum(),np.median(z[train])),z[test])
        record.update(slope=slope,shift=shift,fit_error=fit,held_out_error=held,
                      constant_depth_baseline=baseline)
        valid_test=test&np.isfinite(aligned)&(aligned>0)
        errors_all.extend((np.abs(aligned[valid_test]-z[valid_test])/z[valid_test]).tolist())
        errors_baseline.extend((np.abs(np.median(z[train])-z[test])/z[test]).tolist())
        # Gate using training quality only; keep held-out observations diagnostic.
        if fit['median'] is None or fit['median']>.2 or fit['p90']>.65:
            record['reason']='Training geometry and predicted depth disagree';continue
        record['accepted']=True
        usable[image.image_id]=dict(image=image,camera=camera,R=rotation,t=translation,
                                    center=image.projection_center(),prediction=prediction,
                                    depth=inverse_to_depth(prediction,slope,shift),
                                    ids=set(ids[train].tolist()),z_bounds=np.percentile(z[train],[2,98]),
                                    record=record)
    aligned_at=time.perf_counter()
    if len(usable)<3:
        report=dict(source_revision=scene['revision'],frames=records,aligned_frames=len(usable),
                    failure='Fewer than three alignable frames; no inferred cloud published')
        atomic_json(output/'report.json',report)
        raise ValueError(report['failure'])
    points=[];colors=[];supports=[];total_candidates=0;counts={str(t):0 for t in (.03,.05,.08,.12)}
    for number,(image_id,view) in enumerate(usable.items()):
        camera=view['camera'];image=view['image']
        yy,xx=np.mgrid[stride//2:camera.height:stride,stride//2:camera.width:stride]
        uv=np.column_stack([xx.ravel()+.5,yy.ravel()+.5])
        z=sample_image(view['depth'],uv,camera.width,camera.height)
        # Remove model discontinuities instead of bridging chair legs/background.
        depth=view['depth'].astype(np.float32)
        grad=np.maximum(np.abs(cv2.Sobel(depth,cv2.CV_32F,1,0,ksize=3))/8,
                        np.abs(cv2.Sobel(depth,cv2.CV_32F,0,1,ksize=3))/8)
        gradient=sample_image(grad,uv,camera.width,camera.height)
        low,high=view['z_bounds']
        valid=np.isfinite(z)&(z>low*.6)&(z<high*1.5)&np.isfinite(gradient)&(gradient/np.maximum(z,1e-8)<.06)
        uv,z=uv[valid],z[valid]
        rays=camera.cam_from_img(uv)
        camera_xyz=np.column_stack([rays,np.ones(len(rays))])*z[:,None]
        world=(camera_xyz-view['t'])@view['R']
        finite=np.isfinite(world).all(axis=1);world,uv,z=world[finite],uv[finite],z[finite]
        total_candidates+=len(world)
        ranked=sorted(((len(view['ids']&other['ids']),other_id) for other_id,other in usable.items()
                       if other_id!=image_id),reverse=True)
        neighbors=[usable[other_id] for overlap,other_id in ranked[:8] if overlap>=20]
        consistency=[]
        for other in neighbors:
            projected=world@other['R'].T+other['t']
            neighbor_uv=other['camera'].img_from_cam(projected)
            neighbor_depth=sample_image(other['depth'],neighbor_uv,other['camera'].width,other['camera'].height)
            with np.errstate(invalid='ignore',divide='ignore'):
                rel=np.abs(neighbor_depth-projected[:,2])/projected[:,2]
                other_rays=other['camera'].cam_from_img(neighbor_uv)
                back_camera=np.column_stack([other_rays,np.ones(len(world))])*neighbor_depth[:,None]
                back_world=(back_camera-other['t'])@other['R']
                back_uv=camera.img_from_cam(back_world@view['R'].T+view['t'])
                cycle=np.linalg.norm(back_uv-uv,axis=1)
                a=world-view['center'];b=world-other['center']
                cosine=np.sum(a*b,axis=1)/(np.linalg.norm(a,axis=1)*np.linalg.norm(b,axis=1))
                angle=np.degrees(np.arccos(np.clip(cosine,-1,1)))
            good=np.isfinite(rel)&np.isfinite(cycle)&(projected[:,2]>0)&(cycle<cycle_pixels)&(angle>=1.)
            consistency.append(np.where(good,rel,np.inf))
        support=np.zeros(len(world),dtype=int)
        if consistency:
            comparisons=np.stack(consistency)
            for threshold in (.03,.05,.08,.12):counts[str(threshold)]+=int(((comparisons<threshold).sum(axis=0)>=minimum_neighbors).sum())
            support=(comparisons<relative_tolerance).sum(axis=0)
        accepted=support>=minimum_neighbors
        rgb=cv2.cvtColor(cv2.imread(str(session/'images'/image.name)),cv2.COLOR_BGR2RGB)
        pix=np.clip(np.floor(uv).astype(int),[0,0],[camera.width-1,camera.height-1])
        points.append(world[accepted]);colors.append(rgb[pix[accepted,1],pix[accepted,0]])
        supports.append(support[accepted]+1)
        view['record'].update(surface_candidates=len(world),surface_samples_retained=int(accepted.sum()),neighbors_checked=len(neighbors))
        if number%20==0:print(f'Cross-view checks {number+1}/{len(usable)}',flush=True)
    xyz=np.concatenate(points);rgb=np.concatenate(colors);support=np.concatenate(supports)
    sparse=np.array([p.xyz for p in model.points3D.values()])
    extent=float(np.linalg.norm(np.percentile(sparse,95,axis=0)-np.percentile(sparse,5,axis=0)))
    voxel_size=max(extent/350,1e-6)
    xyz,rgb,support=voxel_average(xyz,rgb,support,voxel_size)
    write_ply(output/'inferred.ply',xyz,rgb)
    np.savez_compressed(output/'inferred.npz',xyz=xyz,rgb=rgb,support=support)
    def stats(values):
        return dict(count=len(values),median=float(np.median(values)),p90=float(np.percentile(values,90)),
                    within_10_percent=float(np.mean(np.asarray(values)<.1))) if values else dict(count=0)
    report=dict(schema_version=1,session_id=session.name,source_revision=scene['revision'],
                revision=output.name,model='Depth Anything V2 Small / Core ML F16',
                aligned_frames=len(usable),registered_frames=model.num_reg_images(),frames=records,
                alignment='Robust affine prediction to inverse COLMAP camera-Z; positive slope',
                holdout='Point track ID modulo 5 = 0, excluded from fitting in every frame; observations correlated across views',
                held_out_relative_error=stats(errors_all),constant_depth_baseline=stats(errors_baseline),
                surface_candidates=total_candidates,threshold_sweep_retained_samples=counts,
                fused_points=len(xyz),voxel_size_arbitrary_units=voxel_size,
                acceptance=f'{relative_tolerance:.0%} depth agreement + {cycle_pixels}px cycle reprojection + >=1 degree baseline in >={minimum_neighbors} other views; depth edges excluded',
                timing_seconds=dict(alignment=aligned_at-started,fusion=time.perf_counter()-aligned_at,total=time.perf_counter()-started),
                validation='Internal consistency with a sparse reconstruction of unknown metric scale; NOT physical accuracy. Occluded space remains unknown.')
    atomic_json(output/'report.json',report)
    step=max(1,int(np.ceil(len(xyz)/45000)))
    display=np.column_stack([xyz[::step],rgb[::step]]).tolist()
    layer=dict(schema_version=1,session_id=session.name,source_revision=scene['revision'],revision=output.name,
               kind='model_inferred_surfaces',experimental=True,units='arbitrary',scale='unknown',
               points=display,points_total=len(xyz),aligned_frames=len(usable),
               held_out_relative_error=report['held_out_relative_error'],
               note='AI-inferred surfaces, checked across views; not measured geometry, temperature or collision clearance')
    atomic_json(output/'layer.json',layer)
    if publish:
        current=json.loads((session/'scene.json').read_text())
        if current['revision']!=scene['revision']:raise ValueError('Sparse map changed during fusion; result retained but not published')
        atomic_json(session/'depth.json',layer)
    print(json.dumps({k:v for k,v in report.items() if k!='frames'},indent=2),flush=True)
    return report


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('session','cache','output'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--publish',action='store_true');parser.add_argument('--stride',type=int,default=6)
    parser.add_argument('--relative-tolerance',type=float,default=.08)
    parser.add_argument('--minimum-neighbors',type=int,default=2);parser.add_argument('--cycle-pixels',type=float,default=2.5)
    args=parser.parse_args()
    run(args.session,args.cache,args.output,args.publish,args.stride,args.relative_tolerance,args.minimum_neighbors,args.cycle_pixels)
