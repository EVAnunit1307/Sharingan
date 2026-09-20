"""Generate the trail's cooked, through-wall vertex-color material in the editor."""
import unreal

path = '/Game/Materials/M_WallhackTrail'
material = unreal.load_asset(path)
if material is None:
    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        'M_WallhackTrail', '/Game/Materials', unreal.Material, unreal.MaterialFactoryNew())
# CDO references root the loaded material in commandlets. Reconnect new nodes
# without deleting rooted expressions; unused nodes do not enter the shader.
material.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_UNLIT)
material.set_editor_property('blend_mode', unreal.BlendMode.BLEND_TRANSLUCENT)
material.set_editor_property('two_sided', True)
# This also bypasses Meta's hard-occlusion depth buffer for translucent guidance.
# Environment depth remains enabled for obstacle observations and pathfinding.
material.set_editor_property('disable_depth_test', True)
material.set_editor_property('translucency_pass', unreal.MaterialTranslucencyPass.MTP_BEFORE_DOF)
material.set_editor_property('enable_mobile_separate_translucency', False)
vertex = unreal.MaterialEditingLibrary.create_material_expression(material, unreal.MaterialExpressionVertexColor, -200, 0)
assert unreal.MaterialEditingLibrary.connect_material_property(vertex, '', unreal.MaterialProperty.MP_EMISSIVE_COLOR)
assert unreal.MaterialEditingLibrary.connect_material_property(vertex, 'A', unreal.MaterialProperty.MP_OPACITY)
unreal.MaterialEditingLibrary.recompile_material(material)
assert unreal.EditorAssetLibrary.save_loaded_asset(material)
assert material.get_editor_property('disable_depth_test')
unreal.log('WALLHACK_NAVIGATION_MATERIAL_VERIFIED ' + material.get_path_name())
