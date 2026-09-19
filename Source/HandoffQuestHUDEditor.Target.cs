using UnrealBuildTool;

public class HandoffQuestHUDEditorTarget : TargetRules
{
    public HandoffQuestHUDEditorTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Editor;
        DefaultBuildSettings = BuildSettingsVersion.V6;
        IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
        ExtraModuleNames.Add("HandoffQuestHUD");
    }
}
