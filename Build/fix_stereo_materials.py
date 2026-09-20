"""Keep world overlays in the main stereo translucency pass; preserve appearance."""
import json
from pathlib import Path
import unreal

report = {}
for name in ('M_HumanSilhouette', 'M_WallhackTrail'):
    material = unreal.load_asset('/Game/Materials/' + name)
    assert isinstance(material, unreal.Material), name
    report[name] = {'previous_pass': str(material.get_editor_property('translucency_pass')),
                    'previous_mobile_separate': material.get_editor_property('enable_mobile_separate_translucency')}
    material.set_editor_property('translucency_pass', unreal.MaterialTranslucencyPass.MTP_BEFORE_DOF)
    material.set_editor_property('enable_mobile_separate_translucency', False)
    unreal.MaterialEditingLibrary.recompile_material(material)
    assert unreal.EditorAssetLibrary.save_loaded_asset(material)
    assert material.get_editor_property('disable_depth_test'), name
    report[name]['pass'] = str(material.get_editor_property('translucency_pass'))
    report[name]['mobile_separate'] = material.get_editor_property('enable_mobile_separate_translucency')
path = Path(unreal.Paths.project_saved_dir()) / 'NavigationVerification/stereo-material-update.json'
path.write_text(json.dumps(report, indent=2) + '\n')
unreal.log('STEREO_MATERIAL_UPDATE ' + json.dumps(report))
