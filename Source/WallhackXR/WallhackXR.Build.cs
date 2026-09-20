using UnrealBuildTool;

public class WallhackXR : ModuleRules
{
    public WallhackXR(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.Add("Core");
        if (Target.Platform == UnrealTargetPlatform.Android)
        {
            PrivateDependencyModuleNames.AddRange(new[] {
                "CoreUObject", "Engine", "HeadMountedDisplay", "OpenXRHMD",
                "KhronosOpenXRHeaders"
            });
            AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenXR");
        }
    }
}
