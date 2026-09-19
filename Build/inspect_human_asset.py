"""Inspect the downloaded Blender bundle without executing embedded scripts."""
import bpy
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = root / 'SourceAssets/HumanSilhouette/human-base-meshes-bundle-v1.4.1/human_base_meshes_bundle.blend'
with bpy.data.libraries.load(str(source), link=False) as (src, dst):
    dst.objects = [n for n in src.objects if 'body' in n.lower() and 'realistic' in n.lower()]
    dst.texts = src.texts
rows = []
for o in dst.objects:
    if o is None:
        continue
    bpy.context.scene.collection.objects.link(o)
    rows.append(dict(name=o.name, type=o.type, location=list(o.location), rotation=list(o.rotation_euler),
                     dimensions=list(o.dimensions), vertices=len(o.data.vertices) if o.type == 'MESH' else 0,
                     groups=[g.name for g in o.vertex_groups] if o.type == 'MESH' else [],
                     modifiers=[dict(name=m.name,type=m.type) for m in o.modifiers],
                     author=o.asset_data.author if o.asset_data else '',
                     description=o.asset_data.description if o.asset_data else ''))
report = dict(objects=rows, texts={t.name:t.as_string() for t in dst.texts if t is not None})
(root/'Saved/SilhouetteVerification/source-inspection.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print(json.dumps(report,indent=2))
