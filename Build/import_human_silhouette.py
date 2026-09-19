"""Import the anatomical mesh and create its inexpensive translucent material."""
import json
from pathlib import Path
import unreal

root = Path(unreal.Paths.project_dir()).resolve()
assert (root/'SourceAssets/HumanSilhouette/SM_HumanSilhouette.fbx').is_file()
# Use the FBX options below deterministically, rather than the Interchange UI pipeline.
unreal.SystemLibrary.execute_console_command(None, 'Interchange.FeatureFlags.Import.FBX 0')
task = unreal.AssetImportTask()
task.filename = str(root / 'SourceAssets/HumanSilhouette/SM_HumanSilhouette.fbx')
task.destination_path = '/Game/People'
task.destination_name = 'SM_HumanSilhouette'
task.automated = True
task.replace_existing = True
task.save = True
options = unreal.FbxImportUI()
options.import_mesh = True
options.import_as_skeletal = False
options.import_materials = False
options.import_textures = False
options.import_animations = False
options.automated_import_should_detect_type = False
options.mesh_type_to_import = unreal.FBXImportType.FBXIT_STATIC_MESH
options.static_mesh_import_data.combine_meshes = True
options.static_mesh_import_data.auto_generate_collision = False
options.static_mesh_import_data.generate_lightmap_u_vs = False
options.static_mesh_import_data.convert_scene = True
options.static_mesh_import_data.force_front_x_axis = True
options.static_mesh_import_data.convert_scene_unit = True
task.options = options
unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
mesh = unreal.load_asset('/Game/People/SM_HumanSilhouette')
assert isinstance(mesh, unreal.StaticMesh), task.imported_object_paths
mesh.set_editor_property('allow_cpu_access', True)  # Bounds/topology verification on desktop.

material = unreal.load_asset('/Game/Materials/M_HumanSilhouette')
if material is None:
    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        'M_HumanSilhouette', '/Game/Materials', unreal.Material, unreal.MaterialFactoryNew())
material.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_UNLIT)
material.set_editor_property('blend_mode', unreal.BlendMode.BLEND_TRANSLUCENT)
material.set_editor_property('two_sided', False)
material.set_editor_property('disable_depth_test', True)
edit = unreal.MaterialEditingLibrary
color = edit.create_material_expression(material, unreal.MaterialExpressionVectorParameter, -500, 0)
color.set_editor_property('parameter_name', 'Tint')
color.set_editor_property('default_value', unreal.LinearColor(0.32, 0.7, 0.55, 1.0))
opacity = edit.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -500, 180)
opacity.set_editor_property('parameter_name', 'Opacity')
opacity.set_editor_property('default_value', 0.16)
fresnel = edit.create_material_expression(material, unreal.MaterialExpressionFresnel, -500, 350)
fresnel.set_editor_property('base_reflect_fraction', 0.28)
fresnel.set_editor_property('exponent', 2.0)
multiply = edit.create_material_expression(material, unreal.MaterialExpressionMultiply, -150, 200)
assert edit.connect_material_expressions(opacity, '', multiply, 'A')
assert edit.connect_material_expressions(fresnel, '', multiply, 'B')
assert edit.connect_material_property(color, '', unreal.MaterialProperty.MP_EMISSIVE_COLOR)
assert edit.connect_material_property(multiply, '', unreal.MaterialProperty.MP_OPACITY)
edit.recompile_material(material)
mesh.set_material(0, material)
assert unreal.EditorAssetLibrary.save_loaded_asset(material)
assert unreal.EditorAssetLibrary.save_loaded_asset(mesh)
bounds = mesh.get_bounding_box()
size = bounds.max - bounds.min
assert abs(size.z - 100) < 0.5, str(bounds)
assert abs(bounds.min.z) < 0.5, str(bounds)
report = dict(mesh=mesh.get_path_name(), material=material.get_path_name(),
    bounds_min=[bounds.min.x,bounds.min.y,bounds.min.z],
    bounds_max=[bounds.max.x,bounds.max.y,bounds.max.z],
    height_cm=size.z, through_walls=material.get_editor_property('disable_depth_test'))
(root/'Saved/SilhouetteVerification/import.json').write_text(json.dumps(report,indent=2)+'\n')
unreal.log('HUMAN_SILHOUETTE_IMPORTED ' + json.dumps(report))
