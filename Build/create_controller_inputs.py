"""Create startup-visible OpenXR input assets, using Unreal's Python commandlet."""
import json
from pathlib import Path
import unreal

ROOT = '/Game/Input'
CONTEXTS = {
    'Common': {
        'CycleHUD': ['OculusTouch_Right_B_Click', 'B'],
        'CalibrateNorth': ['OculusTouch_Left_X_Click', 'X'],
    },
    'Sensor': {
        'SensorConfirm': ['OculusTouch_Right_Trigger_Click', 'Enter'],
        'SensorReset': ['OculusTouch_Right_A_Click', 'C'],
    },
    'Navigation': {
        'MapRange': ['OculusTouch_Left_Thumbstick_Click', 'M'],
        'NavigationAim': ['OculusTouch_Right_Grip_Click', 'G'],
        'NavigationConfirm': ['OculusTouch_Right_Trigger_Click', 'Enter'],
        'NavigationCancel': ['OculusTouch_Right_A_Click', 'C'],
        'PeopleMode': ['OculusTouch_Left_Y_Click', 'H'],
        'PeopleSelect': ['OculusTouch_Right_Thumbstick_Click', 'Tab'],
        'PeopleMove': ['OculusTouch_Left_Trigger_Click', 'R'],
        'PeopleHeight': ['OculusTouch_Right_Thumbstick_Y', 'RightBracket', '-LeftBracket'],
        'PeopleFacing': ['OculusTouch_Right_Thumbstick_X', 'Period', '-Comma'],
    },
    'Demo': {
        'Transmit': ['OculusTouch_Left_Trigger_Click'],
        'PlaceContact': ['OculusTouch_Right_A_Click', 'SpaceBar'],
        'SelectContact': ['OculusTouch_Right_Trigger_Click', 'Tab'],
        'MapRange': ['OculusTouch_Right_Thumbstick_Click', 'M'],
        'PauseSimulation': ['OculusTouch_Left_Y_Click', 'P'],
        'ContactUpdates': ['OculusTouch_Left_Thumbstick_Click', 'F'],
    },
}
assets = unreal.AssetToolsHelpers.get_asset_tools()

def asset(name, cls, factory):
    path = f'{ROOT}/{name}'
    obj = unreal.load_asset(path) if unreal.EditorAssetLibrary.does_asset_exist(path) else assets.create_asset(name, ROOT, cls, factory())
    assert isinstance(obj, cls), path
    return obj

actions = {}
for bindings in CONTEXTS.values():
    for name in bindings:
        if name in actions:
            continue
        obj = asset('IA_' + name, unreal.InputAction, unreal.InputAction_Factory)
        obj.set_editor_property('value_type', unreal.InputActionValueType.AXIS1D if name in ('PeopleHeight', 'PeopleFacing') else unreal.InputActionValueType.BOOLEAN)
        obj.set_editor_property('action_description', name)
        assert unreal.EditorAssetLibrary.save_loaded_asset(obj)
        actions[name] = obj

report = {}
for mode, bindings in CONTEXTS.items():
    context = asset('IMC_Wallhack' + mode, unreal.InputMappingContext, unreal.InputMappingContext_Factory)
    context.set_editor_property('context_description', 'Wallhack ' + mode)
    context.unmap_all()
    mappings = []
    for name, keys in bindings.items():
        for key_name in keys:
            negate = key_name.startswith('-')
            key = unreal.Key()
            key.set_editor_property('key_name', key_name.lstrip('-'))
            mapping = unreal.EnhancedActionKeyMapping()
            mapping.set_editor_property('action', actions[name])
            mapping.set_editor_property('key', key)
            if negate:
                mapping.set_editor_property('modifiers', [unreal.new_object(unreal.InputModifierNegate, outer=context)])
            mappings.append(mapping)
    # Unreal Python returns copies of struct entries; write the full array back.
    default_mappings = unreal.InputMappingContextMappingData()
    default_mappings.set_editor_property('mappings', mappings)
    context.set_editor_property('default_key_mappings', default_mappings)
    assert unreal.EditorAssetLibrary.save_loaded_asset(context)
    report[mode] = {'path': context.get_path_name(), 'mappings': len(mappings)}
out = Path(unreal.Paths.project_dir()) / 'Saved/PersonPose/controller-input-assets.json'
out.write_text(json.dumps(report, indent=2) + '\n')
unreal.log('WALLHACK_CONTROLLER_INPUTS_CREATED ' + json.dumps(report))
