"""Import the rigged silhouette; retain the legacy manual/static renderer."""
import json
from pathlib import Path
import unreal
root=Path(unreal.Paths.project_dir()).resolve()
unreal.SystemLibrary.execute_console_command(None,'Interchange.FeatureFlags.Import.FBX 0')
task=unreal.AssetImportTask()
task.filename=str(root/'SourceAssets/HumanSilhouette/SK_HumanSilhouette.fbx')
task.destination_path='/Game/People'
task.destination_name='SK_HumanSilhouette'
task.automated=True;task.replace_existing=True;task.save=True
options=unreal.FbxImportUI()
options.import_mesh=True;options.import_as_skeletal=True
options.import_materials=False;options.import_textures=False;options.import_animations=False
options.create_physics_asset=False
options.automated_import_should_detect_type=False
options.mesh_type_to_import=unreal.FBXImportType.FBXIT_SKELETAL_MESH
options.skeletal_mesh_import_data.convert_scene=True
options.skeletal_mesh_import_data.force_front_x_axis=True
options.skeletal_mesh_import_data.convert_scene_unit=True
task.options=options
unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
mesh=unreal.load_asset('/Game/People/SK_HumanSilhouette')
assert isinstance(mesh,unreal.SkeletalMesh),task.imported_object_paths
material=unreal.load_asset('/Game/Materials/M_HumanSilhouette')
material.set_editor_property('used_with_skeletal_mesh',True)
unreal.MaterialEditingLibrary.recompile_material(material)
materials=mesh.get_editor_property('materials')
for i,m in enumerate(materials):
    m.material_interface=material
    materials[i]=m # Unreal struct-array iteration yields copies.
mesh.set_editor_property('materials',materials)
editor=unreal.get_editor_subsystem(unreal.SkeletalMeshEditorSubsystem)
assert editor.regenerate_lod(mesh,3,True,False),'Mobile skeletal LOD generation failed'
assert unreal.WallhackPeopleRenderer.prepare_articulated_asset(mesh),'CPU skin cook preparation failed'
assert unreal.EditorAssetLibrary.save_loaded_asset(mesh.get_editor_property('skeleton'))
unreal.EditorAssetLibrary.save_loaded_asset(material)
unreal.EditorAssetLibrary.save_loaded_asset(mesh)
assert mesh.get_editor_property('skeleton') is not None
report=dict(mesh=mesh.get_path_name(),skeleton=mesh.get_editor_property('skeleton').get_path_name(),
    imported=list(task.imported_object_paths),lod_vertices=[editor.get_num_verts(mesh,i) for i in range(3)],
    material_slots=[m.material_interface.get_path_name() for m in mesh.get_editor_property('materials')])
(root/'Saved/PersonPose/import.json').write_text(json.dumps(report,indent=2)+'\n')
unreal.log('ARTICULATED_HUMAN_IMPORTED '+json.dumps(report))
