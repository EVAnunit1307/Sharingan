"""RGB-only TUM replay input and separate, post-inference reference evaluation.

Reference depth/poses never enter the inference input. A fitted similarity scale
is an evaluation aid, not a recovered metric scale or independent localization.
"""
import argparse
from decimal import Decimal
import hashlib
import json
from pathlib import Path
import shutil
import tarfile

import cv2
import numpy as np

from Mapping.da3_inspect import centers, similarity

SOURCE = 'https://cvg.cit.tum.de/data/datasets/rgbd-dataset'


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2, allow_nan=False)+'\n')


def read_rows(path):
    rows = [line.split() for line in Path(path).read_text().splitlines()
            if line.strip() and not line.lstrip().startswith('#')]
    times = np.array([int(Decimal(r[0])*1_000_000_000) for r in rows], dtype=np.int64)
    if len(times)<2 or not np.all(np.diff(times)>0):
        raise ValueError('Reference timestamps must strictly increase')
    return times, rows


def nearest_index(times, timestamp, tolerance_ns=20_000_000):
    i = int(np.searchsorted(times, timestamp))
    choices = [j for j in (i-1, i) if 0 <= j < len(times)]
    if not choices: return None
    j = min(choices, key=lambda j: abs(int(times[j])-int(timestamp)))
    return j if abs(int(times[j])-int(timestamp)) <= tolerance_ns else None


def quaternion_rotation(q):
    q = np.asarray(q, float)
    if q.shape != (4,) or not np.isfinite(q).all() or abs(np.linalg.norm(q)-1)>.05:
        raise ValueError('Expected an approximately unit quaternion in x,y,z,w order')
    x,y,z,w = q/np.linalg.norm(q)
    return np.array([[1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w)],
                     [2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w)],
                     [2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y)]])


def extract_archive(archive, output):
    output = Path(output)
    if output.exists(): raise ValueError('Choose a new extraction directory')
    with tarfile.open(archive, 'r:gz') as tar:
        members = tar.getmembers()
        if len(members)>10000 or sum(m.size for m in members)>3*1024**3:
            raise ValueError('Benchmark archive exceeds the bounded import size')
        for m in members:
            p = Path(m.name)
            if p.is_absolute() or '..' in p.parts or not (m.isfile() or m.isdir()):
                raise ValueError('Unexpected benchmark archive member')
        output.mkdir(parents=True)
        tar.extractall(output, members=members, filter='data')
    with Path(archive).open('rb') as stream:
        digest=hashlib.file_digest(stream,'sha256').hexdigest()
    write_json(output/'archive-source.json', dict(archive=str(Path(archive).resolve()),
               sha256=digest,
               bytes=Path(archive).stat().st_size, source=SOURCE, license='CC BY 4.0'))


def prepare(dataset, output, stride=10):
    dataset, output = Path(dataset).resolve(), Path(output).resolve()
    if output.exists(): raise ValueError('Choose a new RGB input directory')
    if not isinstance(stride,int) or stride<1: raise ValueError('Use a positive stride')
    rgb_times,rgb = read_rows(dataset/'rgb.txt')
    depth_times,depth = read_rows(dataset/'depth.txt')
    gt_times,gt = read_rows(dataset/'groundtruth.txt')
    (output/'images').mkdir(parents=True)
    (output/'reference-depth').mkdir()
    frames, reference = [], []
    def file(name):
        path = (dataset/name).resolve()
        if not path.is_relative_to(dataset) or not path.is_file():
            raise ValueError('Invalid benchmark image path')
        return path
    for i in range(0,len(rgb),stride):
        stamp = int(rgb_times[i]); n = len(frames)
        image = f'images/{n:09}.png'
        shutil.copyfile(file(rgb[i][1]), output/image)
        frames.append(dict(image=image,frame_id=n,sensor_timestamp_ns=stamp,
                           camera_generation=1,source='TUM recorded RGB timestamp'))
        g = nearest_index(gt_times, stamp)
        d = nearest_index(depth_times, stamp)
        entry = dict(index=n,source_rgb=rgb[i][1],timestamp_ns=stamp,position=None,
                     rotation_world_from_camera=None,depth=None)
        if g is not None:
            numbers = np.array(gt[g][1:],float)
            if len(numbers)!=7 or not np.isfinite(numbers).all(): raise ValueError('Invalid reference pose')
            entry.update(position=numbers[:3].tolist(),rotation_world_from_camera=quaternion_rotation(numbers[3:]).tolist(),
                         pose_time_difference_ns=int(gt_times[g])-stamp)
        if d is not None:
            name = f'reference-depth/{n:09}.png'
            shutil.copyfile(file(depth[d][1]), output/name)
            entry.update(depth=name,depth_time_difference_ns=int(depth_times[d])-stamp)
        reference.append(entry)
    (output/'frames.jsonl').write_text(''.join(json.dumps(f)+'\n' for f in frames))
    write_json(output/'reference.json', dict(dataset=dataset.name,source=SOURCE,license='CC BY 4.0',
        depth_divisor=5000,association_tolerance_ns=20_000_000,
        association='Nearest timestamps; no interpolation. Unmatched reference samples withheld.',
        rgb_only_inference=True,frames=reference))
    write_json(output/'manifest.json', dict(session_id=output.name,source='public TUM benchmark',
        frames_saved=len(frames),stride=stride,imu_used=False,
        source_metadata_sha256={name:hashlib.sha256((dataset/name).read_bytes()).hexdigest()
                                for name in ('rgb.txt','depth.txt','groundtruth.txt')}))
    return dict(images=len(frames),span_seconds=(frames[-1]['sensor_timestamp_ns']-frames[0]['sensor_timestamp_ns'])/1e9)


def trajectory_metrics(predicted, reference, fit_count=None):
    """Fit predicted -> reference Sim(3); report explicitly aligned errors."""
    predicted,reference = np.asarray(predicted,float),np.asarray(reference,float)
    if predicted.shape != reference.shape or predicted.ndim!=2 or predicted.shape[1]!=3:
        raise ValueError('Expected matching Nx3 camera paths')
    count = len(predicted) if fit_count is None else fit_count
    if not 3<=count<=len(predicted) or not np.isfinite(predicted).all() or not np.isfinite(reference).all():
        raise ValueError('Need at least three finite poses for alignment')
    for points in (predicted[:count], reference[:count]):
        singular=np.linalg.svd(points-points.mean(0),compute_uv=False)
        if singular[0]<1e-6 or singular[1]<1e-4*singular[0]:
            raise ValueError('Stationary or collinear path cannot establish a stable full similarity')
    scale,rotation,translation = similarity(predicted[:count],reference[:count])
    if not np.isfinite(scale) or scale<=0:raise ValueError('Invalid evaluation alignment')
    aligned=scale*predicted@rotation.T+translation
    errors=np.linalg.norm(aligned-reference,axis=1)
    result=dict(fit_poses=count,evaluated_poses=len(predicted),fitted_scale=scale,
        ate_rmse_metres=float(np.sqrt(np.mean(errors**2))),p90_error_metres=float(np.percentile(errors,90)),
        alignment='Similarity fitted using reference positions; scale is not estimated independently',
        alignment_rotation=rotation.tolist(),alignment_translation=translation.tolist(),
        aligned_positions=aligned.tolist(),reference_positions=reference.tolist())
    if count<len(predicted):result['heldout_rmse_metres']=float(np.sqrt(np.mean(errors[count:]**2)))
    return result


def evaluate(trial, session):
    trial,session = Path(trial),Path(session)
    report=json.loads((trial/'summary.json').read_text())
    if report['state']!='complete' or report['session_id']!=session.name:
        raise ValueError('Trial/session mismatch or incomplete inference')
    data=dict(np.load(trial/'prediction.npz',allow_pickle=False))
    ref=json.loads((session/'reference.json').read_text())
    selected=[ref['frames'][i] for i in report['indices']]
    good=[i for i,r in enumerate(selected) if r['position'] is not None]
    c=centers(data['extrinsics'])
    check=trajectory_metrics(c[good], [selected[i]['position'] for i in good])
    rotation=np.array(check['alignment_rotation'])
    predicted_rotations=rotation@data['extrinsics'][good,:,:3].transpose(0,2,1)
    gt_rotations=np.array([selected[i]['rotation_world_from_camera'] for i in good])
    angle=np.degrees(np.arccos(np.clip((np.trace(predicted_rotations.transpose(0,2,1)@gt_rotations,axis1=1,axis2=2)-1)/2,-1,1)))
    check.update(orientation_median_degrees=float(np.median(angle)),orientation_p90_degrees=float(np.percentile(angle,90)))
    depths=[]
    for i,r in enumerate(selected):
        if not r['depth']:continue
        raw=cv2.imread(str(session/r['depth']),cv2.IMREAD_UNCHANGED)
        if raw is None or raw.dtype!=np.uint16:raise ValueError('Invalid reference depth PNG')
        predicted=data['depth'][i]*check['fitted_scale']
        truth=cv2.resize(raw,(predicted.shape[1],predicted.shape[0]),interpolation=cv2.INTER_NEAREST_EXACT)/ref['depth_divisor']
        valid=(truth>0)&(truth<10)&np.isfinite(predicted)&(predicted>0)
        if valid.sum()<100:continue
        difference=predicted[valid]-truth[valid]
        depths.append(dict(index=i,valid_pixels=int(valid.sum()),valid_fraction=float(valid.mean()),
            abs_relative_mean=float(np.mean(np.abs(difference)/truth[valid])),
            rmse_metres=float(np.sqrt(np.mean(difference**2))),
            delta_1_25=float(np.mean(np.maximum(predicted[valid]/truth[valid],truth[valid]/predicted[valid])<1.25))))
    result=dict(dataset=ref['dataset'],rgb_only_inference=True,reference_used_for_inference=False,
                source=SOURCE,selected_frames=len(selected),matched_poses=len(good),trajectory=check,
                depth=dict(frames=depths,mean_frame_abs_relative=float(np.mean([d['abs_relative_mean'] for d in depths])) if depths else None,
                           scale_source='Single trajectory similarity fit; no per-image depth scale fitting',
                           validity='Registered measured depth between 0 and 10 m, nearest-exact resize; all valid predicted pixels, no confidence filtering'),
                warning='Reference-aligned accuracy on this saved sequence, not metric-scale recovery or live Pi performance.')
    if len(good)>=8:
        try:result['first_half_alignment']=trajectory_metrics(c[good],[selected[i]['position'] for i in good],len(good)//2)
        except ValueError as exc:result['first_half_alignment']=dict(unavailable=str(exc))
    write_json(trial/'reference-evaluation.json',result)
    return result


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    commands=parser.add_subparsers(dest='command',required=True)
    p=commands.add_parser('extract');p.add_argument('--archive',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p=commands.add_parser('prepare');p.add_argument('--dataset',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--stride',type=int,default=10)
    p=commands.add_parser('evaluate');p.add_argument('--trial',type=Path,required=True);p.add_argument('--session',type=Path,required=True)
    args=parser.parse_args()
    if args.command=='extract':result=extract_archive(args.archive,args.output)
    elif args.command=='prepare':result=prepare(args.dataset,args.output,args.stride)
    else:result=evaluate(args.trial,args.session)
    print(json.dumps(result,indent=2))
