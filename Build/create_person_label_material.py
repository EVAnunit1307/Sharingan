"""Cooked stereo-safe material for the runtime-font corner-label textures."""
import unreal

material = unreal.load_asset('/Game/Materials/M_PersonLabel')
if material is None:
    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        'M_PersonLabel', '/Game/Materials', unreal.Material, unreal.MaterialFactoryNew())
material.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_UNLIT)
material.set_editor_property('blend_mode', unreal.BlendMode.BLEND_TRANSLUCENT)
material.set_editor_property('two_sided', True)
material.set_editor_property('disable_depth_test', True)
material.set_editor_property('translucency_pass', unreal.MaterialTranslucencyPass.MTP_BEFORE_DOF)
material.set_editor_property('enable_mobile_separate_translucency', False)
texture = unreal.MaterialEditingLibrary.create_material_expression(
    material, unreal.MaterialExpressionTextureSampleParameter2D, -200, 0)
texture.set_editor_property('parameter_name', 'LabelTexture')
# The runtime render target is linear RGBA8. A matching linear default is also
# required at cook time; the engine's white texture is sRGB and fails validation.
default_path = '/Game/Materials/T_PersonLabelDefault'
default = unreal.load_asset(default_path) if unreal.EditorAssetLibrary.does_asset_exist(default_path) else unreal.EditorAssetLibrary.duplicate_asset(
    '/Engine/EngineResources/WhiteSquareTexture', default_path)
default.set_editor_property('srgb', False)
assert unreal.EditorAssetLibrary.save_loaded_asset(default)
texture.set_editor_property('texture', default)
texture.set_editor_property('sampler_type', unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
assert unreal.MaterialEditingLibrary.connect_material_property(texture, 'RGB', unreal.MaterialProperty.MP_EMISSIVE_COLOR)
assert unreal.MaterialEditingLibrary.connect_material_property(texture, 'A', unreal.MaterialProperty.MP_OPACITY)
unreal.MaterialEditingLibrary.recompile_material(material)
assert unreal.EditorAssetLibrary.save_loaded_asset(material)
unreal.log('PERSON_LABEL_MATERIAL_VERIFIED ' + material.get_path_name())
