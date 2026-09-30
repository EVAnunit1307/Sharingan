"""Experimental XFeat correspondences + COLMAP geometry on an existing recording.

Writes an isolated research result. Does not replace the dashboard's saved map.
Requires the research venv and a pinned verlab/accelerated_features checkout.
"""
import argparse
import json
from pathlib import Path
import sqlite3
import subprocess
import sys
import time

import cv2
import numpy as np


def run(session, source_database, repo, output, geometry_python, progress_file=None):
    import torch
    torch.set_num_threads(4)
    cv2.setNumThreads(1)
    sys.path.insert(0, str(repo.resolve()))
    from modules.xfeat import XFeat
    weights=torch.load(repo/'weights/xfeat.pt',map_location='cpu',weights_only=True)
    extractor=XFeat(weights=weights,top_k=1024)
    output.mkdir(parents=True,exist_ok=False)
    from Mapping.reconstruct import atomic_json
    def progress(message):
        if progress_file:
            atomic_json(progress_file,dict(state='running',revision=output.name,phase=message))
    progress('Extracting learned image features')
    database=output/'database.db'
    with sqlite3.connect(source_database) as source, sqlite3.connect(database) as target:
        source.backup(target)
        images=target.execute('SELECT image_id,name FROM images ORDER BY name').fetchall()
        for table in ('keypoints','descriptors','matches','two_view_geometries'):
            target.execute('DELETE FROM '+table)
        target.commit()
    started=time.perf_counter();features={}
    # Keep Torch and COLMAP in different processes: their macOS wheels contain
    # conflicting OpenMP runtimes. Never suppress the duplicate-runtime check.
    db=sqlite3.connect(database)
    for index,(image_id,name) in enumerate(images):
        frame=cv2.imread(str(session/'images'/name))
        result=extractor.detectAndCompute(cv2.cvtColor(frame,cv2.COLOR_BGR2RGB).astype(np.float32)/255)[0]
        features[image_id]=result['descriptors']
        keypoints=np.ascontiguousarray(result['keypoints'].numpy()+.5,dtype=np.float32)
        db.execute('INSERT INTO keypoints VALUES(?,?,?,?)',(image_id,len(keypoints),2,keypoints.tobytes()))
        if index%40==0:print('Extracted',index+1,'/',len(images),flush=True)
    extracted=time.perf_counter()
    progress('Matching learned features and revisited views')
    # Local neighbors preserve motion continuity; distributed anchors test
    # revisits without requiring every possible image pair.
    pairs={(i,j) for i in range(len(images)) for j in range(i+1,min(i+16,len(images)))}
    pairs|={(min(i,j),max(i,j)) for i in range(0,len(images),25) for j in range(len(images)) if i!=j}
    pair_lines=[]
    for index,(i,j) in enumerate(sorted(pairs)):
        id0,name0=images[i];id1,name1=images[j]
        if min(len(features[id0]),len(features[id1]))<15:continue
        a,b=extractor.match(features[id0],features[id1],min_cossim=.82)
        if len(a)>=15:
            matches=np.column_stack([a.numpy(),b.numpy()]).astype(np.uint32)
            first,second=id0,id1
            if first>second:
                first,second=second,first
                matches=np.ascontiguousarray(matches[:,::-1])
            pair_id=first*2147483647+second
            db.execute('INSERT INTO matches VALUES(?,?,?,?)',(pair_id,len(matches),2,matches.tobytes()))
            pair_lines.append(name0+' '+name1)
        if index%1000==0:print('Matched',index+1,'/',len(pairs),flush=True)
    db.commit();db.close()
    matched=time.perf_counter()
    pair_file=output/'pairs.txt';pair_file.write_text('\n'.join(pair_lines)+'\n')
    print('Geometric verification:',len(pair_lines),'pairs',flush=True)
    command=[str(geometry_python),'-m','Mapping.verify_feature_db','--session',str(session),'--output',str(output)]
    if progress_file:command+=['--progress-file',str(progress_file)]
    subprocess.run(command,check=True)
    finished=time.perf_counter()
    geometry=json.loads((output/'geometry.json').read_text())
    summary={'session_id':session.name,'method':'XFeat 1024 + mutual NN 0.82 + COLMAP verification/mapping',
             'input_images':len(images),'candidate_pairs':len(pairs),'raw_matching_pairs':len(pair_lines),
             'timing_seconds':{'extraction':extracted-started,'matching':matched-extracted,
                               **geometry['timing_seconds'],'total':finished-started},
             'components':geometry['components'],
             'note':'Experimental; no measured metric scale or physical accuracy. Original map is preserved.'}
    (output/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps(summary),flush=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('session','source-database','xfeat-repo','output'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--geometry-python',type=Path,default=Path('.venv/bin/python'))
    parser.add_argument('--progress-file',type=Path)
    args=parser.parse_args()
    run(args.session,args.source_database,args.xfeat_repo,args.output,args.geometry_python,args.progress_file)
