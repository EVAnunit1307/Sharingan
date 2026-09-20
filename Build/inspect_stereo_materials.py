"""Read the actual asset and renderer settings, without changing them."""
import json
from pathlib import Path
import unreal

report = {"materials": {}, "renderer": {}}
for name in ('M_HumanSilhouette', 'M_WallhackTrail', 'M_WallhackContact'):
    material = unreal.load_asset('/Game/Materials/' + name)
    values = {}
    for prop in ('blend_mode', 'translucency_pass', 'enable_mobile_separate_translucency',
                 'disable_depth_test', 'two_sided'):
        try:
            values[prop] = str(material.get_editor_property(prop))
        except Exception as e:
            values[prop] = str(e)
    report['materials'][name] = values
for cvar in ('r.MobileHDR', 'vr.MobileMultiView', 'vr.InstancedStereo',
             'r.Mobile.ShadingPath', 'r.SeparateTranslucency'):
    report['renderer'][cvar] = unreal.SystemLibrary.get_console_variable_int_value(cvar)
path = Path(unreal.Paths.project_saved_dir()) / 'NavigationVerification/stereo-material-settings.json'
path.write_text(json.dumps(report, indent=2) + '\n')
unreal.log('STEREO_MATERIAL_BASELINE ' + json.dumps(report))
