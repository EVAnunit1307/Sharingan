"""Optional XFeat build orchestration. Torch stays in a separate Python process."""
from datetime import datetime, timezone
import json
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import uuid

ROOT=Path(__file__).resolve().parent.parent
RESEARCH=ROOT/'Saved'/'MappingResearch'
RESEARCH_PYTHON=RESEARCH/'venv'/'bin'/'python'
XFEAT_REPO=RESEARCH/'accelerated_features'


def available():
    return RESEARCH_PYTHON.is_file() and (XFEAT_REPO/'weights'/'xfeat.pt').is_file()


def publish(session,output):
    import pycolmap
    from Mapping.reconstruct import export_reconstruction
    models={int(path.name):pycolmap.Reconstruction(path) for path in (output/'sparse').iterdir() if path.is_dir() and path.name.isdigit()}
    return export_reconstruction(session,output,models,'xfeat_mutual_nn_experimental',experimental=True)


def reconstruct(session):
    from Mapping.reconstruct import atomic_json
    if not available():
        raise ValueError('Optional learned mapper is not installed; see Mapping/RESEARCH.md')
    manifest=json.loads((session/'manifest.json').read_text())
    if manifest.get('status') not in ('complete','interrupted'):
        raise ValueError('Finish the recording before reconstruction')
    names=sorted(p.name for p in (session/'images').glob('*.jpg'))
    if not 12<=len(names)<=400:
        raise ValueError('The experimental learned mapper supports 12–400 images per walk')
    revision=datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ-')+uuid.uuid4().hex[:8]
    output=session/'reconstructions'/revision
    output.parent.mkdir(parents=True,exist_ok=True)
    progress=session/'reconstruction.json'
    atomic_json(progress,dict(state='running',revision=revision,phase='Preparing the stronger matcher'))
    database=None
    for path in sorted((session/'reconstructions').glob('*/database.db')):
        try:
            with sqlite3.connect(f'file:{path}?mode=ro',uri=True) as db:
                stored={row[0] for row in db.execute('SELECT name FROM images')}
            if stored==set(names):database=path;break
        except sqlite3.Error:pass
    with tempfile.TemporaryDirectory(prefix='.learned-bootstrap-',dir=session) as temporary:
        if database is None:
            import pycolmap
            database=Path(temporary)/'database.db'
            options=pycolmap.FeatureExtractionOptions();options.num_threads=4;options.use_gpu=False
            pycolmap.extract_features(database,session/'images',image_names=names,
                                     camera_mode=pycolmap.CameraMode.SINGLE,
                                     reader_options=dict(camera_model='SIMPLE_RADIAL'),
                                     extraction_options=options,device=pycolmap.Device.cpu)
        subprocess.run([str(RESEARCH_PYTHON),'-m','Mapping.reconstruct_xfeat','--session',str(session),
                        '--source-database',str(database),'--xfeat-repo',str(XFEAT_REPO),'--output',str(output),
                        '--geometry-python',sys.executable,'--progress-file',str(progress)],cwd=ROOT,check=True)
    return publish(session,output)
