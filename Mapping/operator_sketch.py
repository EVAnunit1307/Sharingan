"""Build small, explicitly inferred operator sketches from DA3 + semantic labels.

No unseen room polygon is completed. Floor support is required for top-down
geometry. Generic object regions retain uncertain semantic candidates. A separate
synthetic observation replay exercises the pose boundary; it is never radar data.
"""
import argparse
from collections import Counter, defaultdict
from dataclasses import replace
import json
from pathlib import Path

import cv2
import numpy as np

from Mapping.da3_inspect import centers, diagnostics, jpeg_data, compare_poses
from Mapping.pose_bridge import PoseSample, place_observation

COLORS={'floor':[104,157,130],'wall':[132,159,196],'object':[221,169,94],
        'door':[194,136,213],'window':[109,185,194]}


def category(label):
    label=label.strip().lower()
    if label in ('floor','wall','door'):return label
    if label in ('windowpane','window'):return 'window'
    if label in ('bed','chair','armchair','swivel chair','stool','seat','sofa','table',
                 'desk','coffee table','cabinet','wardrobe','chest of drawers','shelf',
                 'bookcase','cradle','toilet','sink','bathtub','box','bag','ottoman'):
        return 'object'
    return None


def fit_floor(points, views, camera_centres, depth_scale):
    """Heuristic plane support, explicitly not gravity or physical validation."""
    if len(points)<200 or len(np.unique(views))<2:
        return None,dict(available=False,reason='Too little confidently labelled floor across multiple views',points=len(points))
    rng=np.random.default_rng(42)
    ix=rng.choice(len(points),min(len(points),4000),replace=False);sample=points[ix]
    tolerance=.025*depth_scale;best=np.zeros(len(sample),bool)
    for _ in range(180):
        p=sample[rng.choice(len(sample),3,replace=False)]
        n=np.cross(p[1]-p[0],p[2]-p[0]);length=np.linalg.norm(n)
        if length<1e-8:continue
        n/=length;mask=np.abs((sample-p[0])@n)<tolerance
        if mask.sum()>best.sum():best=mask
    if best.sum()<200:return None,dict(available=False,reason='No sufficiently supported floor plane',points=len(points))
    origin=sample[best].mean(0);_,singular,vt=np.linalg.svd(sample[best]-origin,full_matrices=False)
    normal=vt[-1]
    if normal@(camera_centres.mean(0)-origin)<0:normal=-normal
    residual=np.abs((points-origin)@normal);inliers=residual<tolerance
    fraction=float(inliers.mean());support=len(np.unique(views[inliers]))
    ratio=float(singular[1]/max(singular[0],1e-8))
    report=dict(available=fraction>=.65 and support>=2 and ratio>=.08,
                inlier_fraction=fraction,support_views=support,points=len(points),
                median_residual_arbitrary=float(np.median(residual)),
                tolerance_arbitrary=float(tolerance),extent_ratio=ratio,
                source='semantic floor pixels + RANSAC; no IMU gravity',validated=False)
    report['support_quality']='limited' if support<3 or ratio<.2 else 'broader'
    report['reason']='Supported inferred floor patch' if report['available'] else 'Floor estimates disagree or cover too little area'
    return (dict(origin=origin,normal=normal,inliers=inliers) if report['available'] else None),report


def map_basis(plane, first_centre, first_rotation):
    up=plane['normal'];right=first_rotation[:,0]
    right=right-up*(right@up)
    if np.linalg.norm(right)<.1:raise ValueError('Camera orientation cannot define a stable floor heading')
    right/=np.linalg.norm(right);forward=np.cross(up,right)
    rotation=np.stack([right,forward,up])
    origin=first_centre-up*((first_centre-plane['origin'])@up)
    return rotation,origin


def build(trial):
    report=json.loads((trial/'summary.json').read_text())
    if report['state']!='complete':raise ValueError('Incomplete DA3 result')
    data=dict(np.load(trial/'prediction.npz',allow_pickle=False))
    sem=dict(np.load(trial/'semantics.npz',allow_pickle=False))
    sem_report=json.loads((trial/'semantics.json').read_text());names=sem_report['id2label']
    depth,conf,ex,intr,images=[data[k] for k in ('depth','confidence','extrinsics','intrinsics','images')]
    labels,scores=sem['labels'],sem['scores']
    if labels.shape!=depth.shape or scores.shape!=depth.shape:raise ValueError('Semantic/image geometry mismatch')
    if any(not np.isfinite(v).all() for v in (depth,conf,ex,intr,scores)):raise ValueError('Nonfinite prediction')
    camera_centres=centers(ex);depth_scale=float(np.median(depth));threshold=np.percentile(conf,40)
    ids={int(k):category(v) for k,v in names.items()};floor_id=next(int(k) for k,v in names.items() if v.strip()=='floor')
    clouds=[];all_views=[];all_labels=[];all_categories=[];evidence=[];thumbnails=[];overlays=[]
    h,w=depth.shape[1:];ys,xs=np.mgrid[0:h:3,0:w:3]
    pixels=np.stack([xs,ys,np.ones_like(xs)],axis=-1).reshape(-1,3)
    for i in range(len(depth)):
        d=depth[i,ys,xs].ravel();lab=labels[i,ys,xs].ravel()
        valid=(conf[i,ys,xs].ravel()>=threshold)&(scores[i,ys,xs].ravel()>=.60)&(d>0)&(d<np.percentile(depth,99))
        cam=(pixels@np.linalg.inv(intr[i]).T)*d[:,None]
        world=(cam-ex[i,:,3])@ex[i,:,:3]
        valid &= np.linalg.norm(world-camera_centres[0],axis=1)<5*depth_scale
        clouds.append(world[valid]);all_views.extend([i]*int(valid.sum()));all_labels.extend(lab[valid].tolist())
        all_categories.extend([ids.get(int(k)) for k in lab[valid]])
        overlay=images[i].copy();categories=[]
        for label_id,cat in ids.items():
            if cat is None:continue
            mask=(labels[i]==label_id)&(scores[i]>=.60)
            if mask.sum()<.005*h*w:continue
            overlay[mask]=(overlay[mask]*.5+np.array(COLORS[cat])*.5).astype(np.uint8)
            categories.append(dict(label=names[str(label_id)].strip(),category=cat,pixel_fraction=float(mask.mean())))
        evidence.append(categories);thumbnails.append(jpeg_data(images[i]));overlays.append(jpeg_data(overlay))
    points=np.concatenate(clouds);views=np.array(all_views);lab=np.array(all_labels);cats=np.array(all_categories,dtype=object)
    floor,plane_report=fit_floor(points[lab==floor_id],views[lab==floor_id],camera_centres,depth_scale)
    checks=diagnostics(data)
    error=checks['median_of_pair_medians_pixels']
    placement_ok=(floor is not None and error is not None and error<=.02*w and
                  checks['pairs_with_at_least_8_matches']>=min(2,len(images)-1))
    frame_id=f'{report["session_id"]}/{trial.name}'
    result=dict(schema_version=1,kind='inferred_operator_sketch',id=trial.name,
                scene=trial.name.split('-')[0],map_id=frame_id,units='arbitrary',metric_scale_available=False,
                observation_window_seconds=report['requested_window_seconds'],selected_span_seconds=report['selected_span_seconds'],
                selected_frames=len(images),source_indices=report['indices'],source_session=report['session_id'],
                inference_seconds=report['inference_seconds'],pose_source='da3_recorded_batch',imu_used=False,
                gravity_source='inferred_floor_plane' if floor else 'unavailable',floor=plane_report,
                placement_check_passed=placement_ok,
                placement_check_note='Experimental screening: supported floor, at least two matched image pairs, median reprojection no more than 2% of image width. Not an accuracy certification.',
                diagnostics=checks,images=thumbnails,overlays=overlays,image_labels=evidence,
                colors=COLORS,cells=[],landmarks=[],poses=[],simulation=[],simulation_reference=None,
                clock_id=f'{report["session_id"]}/camera_sensor',
                source_frames=report['frames'],
                warning='AI-inferred visible surfaces only; unobserved cells are unknown. No measured distances, complete room outline, live tracking or real contacts.',
                imu_status='Pending hardware. Future visual-inertial poses require synchronized time, calibrated units and camera/IMU mounting; raw IMU is not a position estimate.')
    if floor:
        rotation,origin=map_basis(floor,camera_centres[0],ex[0,:,:3].T)
        mapped=(points-origin)@rotation.T;cell_size=depth_scale*.035
        accepted=(mapped[:,2]>=-.08*depth_scale)&(mapped[:,2]<3*depth_scale)
        buckets=defaultdict(set);votes=defaultdict(Counter)
        for p,v,label,cat,keep in zip(mapped,views,lab,cats,accepted):
            if cat is None or not keep:continue
            cell=tuple(np.floor(p[:2]/cell_size).astype(int));key=(*cell,cat)
            buckets[key].add(int(v));votes[key][names[str(label)].strip()]+=1
        cells=[]
        for (x,y,cat),support in buckets.items():
            if len(support)>=2:cells.append(dict(x=int(x),y=int(y),category=cat,views=len(support)))
        result.update(cells=cells,cell_size_arbitrary=cell_size,
                      map_from_da3=dict(rotation=rotation.tolist(),origin=origin.tolist()))
        # Connected seen cells summarize patches; these are not full object footprints.
        for cat in ('door','window','object'):
            selected=[c for c in cells if c['category']==cat]
            if len(selected)<3:continue
            grid=np.array([[c['x'],c['y']] for c in selected]);low=grid.min(0);shape=grid.max(0)-low+1
            if shape.max()>1024:continue
            mask=np.zeros((shape[1]+2,shape[0]+2),np.uint8);shifted=grid-low+1;mask[shifted[:,1],shifted[:,0]]=1
            count,components=cv2.connectedComponents(mask,connectivity=8)
            for k in range(1,count):
                ys2,xs2=np.where(components==k)
                if len(xs2)<3:continue
                coordinates=np.column_stack([xs2,ys2])+low-1
                keys=[(int(x),int(y),cat) for x,y in coordinates];support=set().union(*(buckets[key] for key in keys))
                counts=sum((votes[key] for key in keys),Counter());candidates=[x for x,_ in counts.most_common(3)]
                result['landmarks'].append(dict(id=f'{cat}-{k}',category=cat,
                    label='Object region' if cat=='object' else f'{cat.title()}-like surface',
                    position=((coordinates.mean(0)+.5)*cell_size).tolist(),seen_cells=len(coordinates),
                    support_views=sorted(support),semantic_candidates=candidates,
                    evidence='Predicted labels and depth; visible surface patch, not verified object identity or footprint'))
        result['landmarks']=sorted(result['landmarks'],key=lambda x:-x['seen_cells'])[:12]
        mapped_centres=(camera_centres-origin)@rotation.T
        for i,(position,e,row) in enumerate(zip(mapped_centres,ex,report['frames'])):
            # DA3 predicts rotations numerically; project to SO(3) before pose use.
            r=rotation@e[:,:3].T;u,_,vt=np.linalg.svd(r);u[:,-1]*=np.linalg.det(u@vt);r=u@vt
            result['poses'].append(dict(position=position.tolist(),rotation_map_from_camera=r.tolist(),
                                        timestamp_ns=row['sensor_timestamp_ns'],source='da3_recorded_batch',imu_used=False))
        floor_points=mapped[(lab==floor_id)&accepted]
        target=np.array([*np.median(floor_points[:,:2],axis=0),0.])
        result['simulation_reference']=target.tolist()
        for entry in result['poses']:
            pose=PoseSample(frame_id,result['clock_id'],entry['timestamp_ns'],'arbitrary','da3_recorded_batch',
                            np.array(entry['position']),np.array(entry['rotation_map_from_camera']),tracking=placement_ok)
            synthetic=pose.rotation_map_from_camera.T@(target-pose.position)
            placed=place_observation(synthetic,pose=pose,timestamp_ns=pose.timestamp_ns,map_id=frame_id,
                                     clock_id=pose.clock_id,units='arbitrary',camera_from_sensor=np.eye(4),max_age_ns=0)
            withheld=place_observation(synthetic,pose=replace(pose,tracking=False),timestamp_ns=pose.timestamp_ns,
                map_id=frame_id,clock_id=pose.clock_id,units='arbitrary',camera_from_sensor=np.eye(4))
            if withheld is not None:raise AssertionError('Lost pose must withhold map placement')
            result['simulation'].append(dict(source='simulation',classification='simulated_contact',
                sensor_point=synthetic.tolist(),map_point=placed.tolist() if placed is not None else None,timestamp_ns=pose.timestamp_ns,
                mount='Synthetic identity camera mount; not a real radar calibration',units='arbitrary'))
        placed_points=[x['map_point'] for x in result['simulation'] if x['map_point'] is not None]
        result['simulation_max_roundtrip_error_arbitrary']=float(max(np.linalg.norm(np.array(x)-target) for x in placed_points)) if placed_points else None
    small={k:v for k,v in result.items() if k not in ('images','overlays')}
    # A small versioned extension, kept separate from existing live sensor packets.
    # Unstable diagnostic geometry is not published as an operator map.
    categories=list(COLORS)
    packet=dict(schema='wallhack.operator_sketch.v1',map_id=frame_id,source='recorded_ai_trial',live=False,
                state='provisional' if placement_ok else 'withheld',units='arbitrary',imu_used=False,
                timestamp_ns=report['frames'][-1]['sensor_timestamp_ns'],clock_id=result['clock_id'],
                unlisted_cells='unknown',cell_size=result.get('cell_size_arbitrary'),categories=categories,
                surface_cells=[[c['x'],c['y'],categories.index(c['category']),c['views']] for c in result['cells']] if placement_ok else [],
                landmarks=result['landmarks'] if placement_ok else [],
                inference_warning='Visible surface estimates; no verified floor plan, metric scale or free-space classification')
    encoded=json.dumps(packet,separators=(',',':'),allow_nan=False)+'\n'
    (trial/'operator-packet.json').write_text(encoded)
    result['operator_packet_bytes']=len(encoded.encode())
    small['operator_packet_bytes']=result['operator_packet_bytes']
    with (trial/'pose-replay.jsonl').open('w') as stream:
        for i,row in enumerate(report['frames']):
            pose=result['poses'][i] if placement_ok else None
            stream.write(json.dumps(dict(schema='wallhack.pose.v1',map_id=frame_id,clock_id=result['clock_id'],
                timestamp_ns=row['sensor_timestamp_ns'],source='recorded_da3',live=False,imu_used=False,
                units='arbitrary',tracking='estimated' if pose else 'unavailable',
                position=pose['position'] if pose else None,
                rotation_map_from_camera=pose['rotation_map_from_camera'] if pose else None),separators=(',',':'))+'\n')
    (trial/'sketch.json').write_text(json.dumps(small,indent=2,allow_nan=False)+'\n')
    (trial/'diagnostics.json').write_text(json.dumps(checks,indent=2)+'\n')
    return result,dict(report=report,arrays=data)


def main(args):
    results=[];raw=[]
    for trial in args.trials:
        result,data=build(trial);results.append(result);raw.append(data)
        print(trial.name,'floor:',result['floor'],'cells:',len(result['cells']),'landmarks:',len(result['landmarks']),flush=True)
    comparisons=[]
    for i,r in enumerate(results):
        longer=[j for j,s in enumerate(results) if s['scene']==r['scene'] and s['observation_window_seconds']>r['observation_window_seconds']]
        if not longer:continue
        j=min(longer,key=lambda j:results[j]['observation_window_seconds'])
        try:check=compare_poses(raw[i],raw[j])
        except ValueError as exc:check=dict(unavailable=str(exc))
        comparisons.append(dict(first=r['id'],second=results[j]['id'],check=check))
    payload=dict(trials=results,comparisons=comparisons,created_from='saved camera recordings; no live Pi, radar or IMU')
    args.output.parent.mkdir(parents=True,exist_ok=True)
    (args.output.parent/'quick-scan-summary.json').write_text(json.dumps(dict(
        trials=[{k:r[k] for k in ('id','selected_frames','selected_span_seconds','inference_seconds','floor','diagnostics')} for r in results],
        comparisons=comparisons),indent=2,allow_nan=False)+'\n')
    template=(Path(__file__).parent/'static'/'operator-sketch.html').read_text()
    args.output.write_text(template.replace('/*SKETCH_DATA*/null',json.dumps(payload,separators=(',',':'),allow_nan=False).replace('<','\\u003c')))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--trials',type=Path,nargs='+',required=True)
    parser.add_argument('--output',type=Path,required=True)
    main(parser.parse_args())
