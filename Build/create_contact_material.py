"""Run with UnrealEditor-Cmd -run=pythonscript -script=<this file>."""
import unreal

asset_path = '/Game/Materials/M_WallhackContact'
material = unreal.load_asset(asset_path)
if material is None:
    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        'M_WallhackContact', '/Game/Materials', unreal.Material, unreal.MaterialFactoryNew())
    material.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_UNLIT)
    material.set_editor_property('blend_mode', unreal.BlendMode.BLEND_OPAQUE)
    color = unreal.MaterialEditingLibrary.create_material_expression(
        material, unreal.MaterialExpressionConstant3Vector, -200, 0)
    color.set_editor_property('constant', unreal.LinearColor(0.12, 1.0, 0.005, 1.0))
    assert unreal.MaterialEditingLibrary.connect_material_property(
        color, '', unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    unreal.MaterialEditingLibrary.recompile_material(material)
    assert unreal.EditorAssetLibrary.save_loaded_asset(material)
assert material.get_editor_property('shading_model') == unreal.MaterialShadingModel.MSM_UNLIT
assert material.get_editor_property('blend_mode') == unreal.BlendMode.BLEND_OPAQUE
unreal.log('WALLHACK_CONTACT_MATERIAL_VERIFIED ' + material.get_path_name())
