"""Isolated XFeat/LighterGlue correspondence trial; run in the research venv.

Never imports COLMAP alongside Torch. Geometry is evaluated separately with
Mapping.stability_trials in the main venv. No active dashboard result is changed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sqlite3
import sys
import time


def run(session, source, repo, output, limit=None):
    import cv2
    import numpy as np
    import torch
    import kornia
    torch.set_num_threads(4)
    cv2.setNumThreads(1)
    sys.path.insert(0, str(repo.resolve()))
    from modules.xfeat import XFeat
    from Mapping.reconstruct import atomic_json
    output.mkdir(parents=True, exist_ok=False)
    weights = torch.load(repo/'weights/xfeat.pt', map_location='cpu', weights_only=True)
    extractor = XFeat(weights=weights, top_k=1024)
    database = output/'database.db'
    with sqlite3.connect(f'file:{source}?mode=ro', uri=True) as src, sqlite3.connect(database) as db:
        src.backup(db)
        images = db.execute('SELECT image_id,name FROM images ORDER BY name').fetchall()
        for table in ('keypoints', 'descriptors', 'matches', 'two_view_geometries'):
            db.execute('DELETE FROM '+table)
        started = time.perf_counter()
        features = {}
        for image_id, name in images:
            frame = cv2.imread(str(session/'images'/name))
            feature = extractor.detectAndCompute(cv2.cvtColor(frame, cv2.COLOR_BGR2RGB).astype(np.float32)/255)[0]
            feature['image_size'] = (frame.shape[1], frame.shape[0])
            features[image_id] = feature
            keypoints = np.ascontiguousarray(feature['keypoints'].cpu().numpy()+.5, dtype=np.float32)
            db.execute('INSERT INTO keypoints VALUES(?,?,?,?)', (image_id, len(keypoints), 2, keypoints.tobytes()))
        extracted = time.perf_counter()
        pairs = {(i,j) for i in range(len(images)) for j in range(i+1, min(i+16, len(images)))}
        pairs |= {(min(i,j),max(i,j)) for i in range(0,len(images),25) for j in range(len(images)) if i!=j}
        pairs = sorted(pairs)
        if limit is not None: pairs = pairs[:limit]
        lines = []
        for index, (i,j) in enumerate(pairs):
            id0,name0 = images[i]; id1,name1 = images[j]
            _,_,indices = extractor.match_lighterglue(features[id0], features[id1], min_conf=.1)
            matches = np.ascontiguousarray(indices, dtype=np.uint32)
            if len(matches) >= 15:
                first,second = id0,id1
                if first > second:
                    first,second = second,first
                    matches = np.ascontiguousarray(matches[:,::-1])
                db.execute('INSERT INTO matches VALUES(?,?,?,?)', (first*2147483647+second,len(matches),2,matches.tobytes()))
                lines.append(name0+' '+name1)
            if index%100 == 0:
                print(f'Matched {index+1}/{len(pairs)}; {time.perf_counter()-extracted:.1f} s', flush=True)
        db.commit()
    (output/'pairs.txt').write_text('\n'.join(lines)+'\n')
    atomic_json(output/'features.json', dict(method='XFeat 1024 + LighterGlue confidence 0.1',
                candidate_pairs=len(pairs),raw_matching_pairs=len(lines),input_images=len(images),
                extraction_seconds=extracted-started,matching_seconds=time.perf_counter()-extracted,
                torch_version=torch.__version__,kornia_version=kornia.__version__,
                weights_sha256=hashlib.sha256((repo/'weights/xfeat-lighterglue.pt').read_bytes()).hexdigest(),
                published=False))


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('session','source','repo','output'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--limit',type=int)
    args=parser.parse_args()
    run(args.session,args.source,args.repo,args.output,args.limit)
