using UnrealBuildTool;

public class HandoffQuestHUD : ModuleRules
{
    public HandoffQuestHUD(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core", "CoreUObject", "Engine", "InputCore", "WebSockets", "Json", "JsonUtilities",
            "DeveloperSettings", "HeadMountedDisplay", "UMG", "ImageWrapper"
        });

        PrivateDependencyModuleNames.AddRange(new[] { "Slate", "SlateCore", "EnhancedInput", "ProceduralMeshComponent", "AndroidPermission", "XRBase", "WallhackXR" });
        if (Target.bBuildEditor)
        {
            PrivateDependencyModuleNames.AddRange(new[] { "RenderCore", "RHI" });
        }

        // The Meta runtime is an Android/Quest feature.  Keeping it out of the
        // Windows target makes the desktop simulator work without an attached
        // headset or the Oculus desktop runtime.
        if (Target.Platform == UnrealTargetPlatform.Android)
        {
            PrivateDependencyModuleNames.AddRange(new[] { "OculusXRHMD", "OculusXRPassthrough", "OculusXRAnchors", "MRUtilityKit", "OculusXRScene" });
            // UE 5.7's base Quest manifest advertises hands even when Meta's
            // HandTrackingSupport is ControllersOnly. Keep app input controller
            // based; WallhackXR explicitly tracks unheld physical controllers.
            AdditionalPropertiesForReceipt.Add("AndroidPlugin",
                System.IO.Path.Combine(ModuleDirectory, "WallhackControllers_APL.xml"));
        }
    }
}
