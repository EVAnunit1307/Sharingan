"""Extract only the CC0 realistic base mesh, without running bundle scripts.
Run with Blender --background --factory-startup --disable-autoexec --python.
"""
import bpy
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
folder = root / 'SourceAssets/HumanSilhouette'
source = folder / 'human-base-meshes-bundle-v1.4.1/human_base_meshes_bundle.blend'
bpy.ops.object.select_all(action='SELECT')
bpy.ops.object.delete(use_global=False)
names = ['GEO-body_male_realistic', 'GEO-body_male_realistic.eye.L', 'GEO-body_male_realistic.eye.R']
with bpy.data.libraries.load(str(source), link=False) as (src, dst):
    assert all(n in src.objects for n in names)
    dst.objects = list(names)
for obj in dst.objects:
    bpy.context.scene.collection.objects.link(obj)
    obj.modifiers.clear()  # Export the detailed base topology, not sculpt subdivisions.
    obj.select_set(True)
body = bpy.data.objects[names[0]]
bpy.context.view_layer.objects.active = body
bpy.ops.object.join()
bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)
z_min = min(v.co.z for v in body.data.vertices)
z_max = max(v.co.z for v in body.data.vertices)
height = z_max - z_min
for vertex in body.data.vertices:
    vertex.co.z -= z_min
    vertex.co /= height
body.location = (0, 0, 0)
body.name = 'SM_HumanSilhouette'
body.data.materials.clear()
for polygon in body.data.polygons:
    polygon.use_smooth = True
bpy.ops.export_scene.fbx(filepath=str(folder / 'SM_HumanSilhouette.fbx'),
    use_selection=True, object_types={'MESH'}, use_mesh_modifiers=False,
    axis_forward='-Y', axis_up='Z', add_leaf_bones=False, bake_anim=False,
    mesh_smooth_type='FACE', use_custom_props=False)
body.data.calc_loop_triangles()
report = dict(source_objects=names, vertices=len(body.data.vertices),
    triangles=len(body.data.loop_triangles), height_meters=1.0,
    original_height_meters=height, origin='Floor, under body centre',
    source_forward='-Y', license='CC0-1.0', version='1.4.1')
(folder / 'mesh-preparation.json').write_text(json.dumps(report, indent=2)+'\n')
print(json.dumps(report))
