"""Expose one explicitly selected, repeatability-screened offline fragment for inspection."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import uuid

import numpy as np
import pycolmap

from Mapping.orb_replay import coarse_points
from Mapping.reconstruct import atomic_json

def export(session,model_path,quality_path):
    session,model_path,quality_path=map(Path,(session,model_path,quality_path))
    entries=json.loads(quality_path.read_text())
    selected=[e for e in entries if str(e['seed42_component'])==model_path.name]
    if len(selected)!=1 or selected[0]['quality']['status']!='repeatable_partial':
        raise ValueError('Select a fragment that passed the saved repeatability screening')
    quality=selected[0]['quality']
    # The supplied report belongs to this exact seed/component tree.
    if model_path.resolve()!= (quality_path.parent/'seed-42'/model_path.name).resolve():
        raise ValueError('Model path and repeatability report do not share provenance')
    model=pycolmap.Reconstruction(model_path)
    manifest=json.loads((session/'manifest.json').read_text())
    rows=[json.loads(line) for line in (session/'frames.jsonl').read_text().splitlines() if line.strip()]
    times=np.array([r['sensor_timestamp_ns'] for r in rows],dtype=np.int64)
    seconds=(times-times[0])/1e9
    by_name={image.name:image for image in model.images.values() if image.has_pose}
    frames=[];indices=[];longest=run=0
    for i,(row,t) in enumerate(zip(rows,seconds)):
        image=by_name.get(Path(row['image']).name)
        frame=dict(image=row['image'],seconds=float(t),state=1,status='not reconstructed',
                   landmarks=0,processing_ms=None,map_id=None,position=None,quaternion=None)
        if image is not None:
            pose=image.cam_from_world().inverse()
            position=pose.translation.tolist();quaternion=pose.rotation.quat.tolist()
            if not np.isfinite(position+quaternion).all():raise ValueError('Non-finite fragment pose')
            frame.update(state=2,status='recovered offline',map_id=0,position=position,quaternion=quaternion,
                         landmarks=image.num_points3D)
            indices.append(i);run+=1
        else:run=0
        longest=max(longest,run);frames.append(frame)
    points=[point.xyz.tolist() for point in model.points3D.values() if point.track.length()>=3 and np.isfinite(point.xyz).all()]
    coarse,size=coarse_points(points)
    revision=datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')+'-'+uuid.uuid4().hex[:8]
    result=dict(schema_version=1,kind='offline_partial_replay',session_id=manifest['session_id'],revision=revision,
        created_at=datetime.now(timezone.utc).isoformat(),units='arbitrary',scale='unknown',gravity_aligned=False,live=False,
        input_frames=len(frames),fps=float(1/np.median(np.diff(seconds))),duration_seconds=float(seconds[-1]),
        retained_poses=len(indices),longest_contiguous_frames=longest,frames=frames,
        maps=[dict(id=0,frames=indices,points=points,coarse=coarse,cell_size_arbitrary=size)],
        geometry_quality=quality,source_model=str(model_path.resolve()),source_quality=str(quality_path.resolve()),
        warning='OFFLINE PARTIAL SCAN · One repeatable fragment from the earlier reconstruction. This is not the ORB-SLAM3 tracking result or a complete room map. Physical shape and scale remain unvalidated.',
        low_rate_warning=None,
        geometry_note='Grouped visual landmarks only. Unseen areas remain unknown; no wall or people positions inferred.')
    output=session/'partial'/revision;output.mkdir(parents=True)
    model.export_PLY(output/'points.ply')
    atomic_json(output/'result.json',result)
    atomic_json(session/'partial.json',result)
    print(json.dumps(dict(revision=revision,views=len(indices),landmarks=len(points),coarse_groups=len(coarse))))
    return result

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--session',type=Path,required=True)
    parser.add_argument('--model',type=Path,required=True)
    parser.add_argument('--quality',type=Path,required=True)
    args=parser.parse_args()
    export(args.session,args.model,args.quality)
