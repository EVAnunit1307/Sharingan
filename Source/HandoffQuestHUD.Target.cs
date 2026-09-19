using UnrealBuildTool;

public class HandoffQuestHUDTarget : TargetRules
{
    public HandoffQuestHUDTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Game;
        DefaultBuildSettings = BuildSettingsVersion.V6;
        IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
        ExtraModuleNames.Add("HandoffQuestHUD");
    }
}
