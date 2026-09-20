"""Rig the existing CC0 mesh in isolated Blender; no downloaded scripts execute."""
import bpy
import json
from mathutils import Vector
from pathlib import Path

root=Path(__file__).resolve().parents[1]
folder=root/'SourceAssets/HumanSilhouette'
out=root/'Saved/PersonPose'
out.mkdir(parents=True,exist_ok=True)
bpy.ops.object.select_all(action='SELECT')
bpy.ops.object.delete(use_global=False)
bpy.ops.import_scene.fbx(filepath=str(folder/'SM_HumanSilhouette.fbx'))
body=next(o for o in bpy.context.scene.objects if o.type=='MESH')
bpy.context.view_layer.objects.active=body
bpy.ops.object.transform_apply(location=True,rotation=True,scale=True)
body.name='SK_HumanSilhouette'
assert .99<body.dimensions.z<1.01, list(body.dimensions)
bones=[('root',(0,0,0),(0,0,.10),None),
       ('pelvis',(0,0,.53),(0,0,.60),'root'),
       ('spine',(0,0,.60),(0,0,.73),'pelvis'),
       ('chest',(0,0,.73),(0,0,.83),'spine'),
       ('neck',(0,0,.83),(0,0,.89),'chest'),
       ('head',(0,0,.89),(0,0,.99),'neck')]
for suffix,sign in [('l',1),('r',-1)]:
    bones.extend([
      (f'clavicle_{suffix}',(0,0,.81),(sign*.10,0,.81),'chest'),
      (f'upperarm_{suffix}',(sign*.10,0,.81),(sign*.165,0,.65),f'clavicle_{suffix}'),
      (f'lowerarm_{suffix}',(sign*.165,0,.65),(sign*.225,-.005,.50),f'upperarm_{suffix}'),
      (f'hand_{suffix}',(sign*.225,-.005,.50),(sign*.255,-.01,.445),f'lowerarm_{suffix}'),
      (f'thigh_{suffix}',(sign*.05,0,.53),(sign*.065,-.005,.285),'pelvis'),
      (f'calf_{suffix}',(sign*.065,-.005,.285),(sign*.075,0,.055),f'thigh_{suffix}'),
      (f'foot_{suffix}',(sign*.075,0,.055),(sign*.085,-.09,.03),f'calf_{suffix}')])
arm=bpy.data.armatures.new('HumanSkeleton')
rig=bpy.data.objects.new('HumanSkeleton',arm)
bpy.context.collection.objects.link(rig)
bpy.context.view_layer.objects.active=rig
rig.select_set(True); body.select_set(False)
bpy.ops.object.mode_set(mode='EDIT')
for name,head,tail,parent in bones:
    bone=arm.edit_bones.new(name);bone.head=head;bone.tail=tail
    if parent:bone.parent=arm.edit_bones[parent]
    if name=='root':bone.use_deform=False
bpy.ops.object.mode_set(mode='OBJECT')
body.select_set(True)
try:
    bpy.ops.object.parent_set(type='ARMATURE_AUTO')
except RuntimeError:
    # Explicit bounded segment weighting below also fills isolated eye islands.
    body.parent=rig
if not any(m.type=='ARMATURE' for m in body.modifiers):
    modifier=body.modifiers.new('HumanSkeleton','ARMATURE');modifier.object=rig
groups={name:body.vertex_groups.get(name) or body.vertex_groups.new(name=name)
        for name,_,_,_ in bones if name!='root'}
def distance(p,a,b):
    a,b=Vector(a),Vector(b);d=b-a;t=max(0,min(1,(p-a).dot(d)/d.length_squared))
    return (p-a-t*d).length
filled=0
for v in body.data.vertices:
    weights=[(g.group,g.weight) for g in v.groups if g.weight>1.e-6]
    if not weights:
        nearest=sorted((distance(v.co,a,b),name) for name,a,b,_ in bones if name!='root')[:2]
        total=sum(1/(d+.005)**4 for d,_ in nearest)
        for d,name in nearest:groups[name].add([v.index],(1/(d+.005)**4)/total,'REPLACE')
        filled+=1
    else:
        kept=sorted(weights,key=lambda w:w[1],reverse=True)[:4]
        total=sum(w for _,w in kept)
        for group in body.vertex_groups:group.remove([v.index])
        for index,w in kept:body.vertex_groups[index].add([v.index],w/total,'REPLACE')
for p in body.data.polygons:p.use_smooth=True
bpy.context.view_layer.objects.active=rig
rig.select_set(True);body.select_set(True)
bpy.ops.export_scene.fbx(filepath=str(folder/'SK_HumanSilhouette.fbx'),use_selection=True,
    object_types={'MESH','ARMATURE'},use_mesh_modifiers=True,axis_forward='-Y',axis_up='Z',
    add_leaf_bones=False,bake_anim=False,mesh_smooth_type='FACE',use_custom_props=False)
bpy.ops.wm.save_as_mainfile(filepath=str(out/'articulated-human.blend'))
report=dict(bones=[dict(name=n,head=a,tail=b,parent=p) for n,a,b,p in bones],
    vertices=len(body.data.vertices),fallback_weight_vertices=filled,
    max_influences=max(len(v.groups) for v in body.data.vertices),license='CC0-1.0',
    height_m=body.dimensions.z,source='existing Blender Studio realistic base mesh')
(folder/'rig-preparation.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report))
