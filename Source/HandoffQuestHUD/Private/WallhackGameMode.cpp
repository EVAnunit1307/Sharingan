#include "WallhackGameMode.h"
#include "WallhackHUD.h"
#include "WallhackVRHUDActor.h"
#include "WallhackVRPawn.h"
#include "WallhackWorldContact.h"
#include "WallhackNavigationSubsystem.h"
#include "Engine/Engine.h"
#include "IXRTrackingSystem.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#if PLATFORM_ANDROID
#include "OculusXRPassthroughSubsystem.h"
#endif

AWallhackGameMode::AWallhackGameMode()
{
    // AWallhackHUD is a flat, screen-space Canvas HUD. It's only meant as a
    // desktop preview (see DrawDesktopOperatorPreview). On Quest it was
    // still being assigned here, so its non-simulated branch (a compass
    // strip plus a black "LINK LOST" box) was drawing in screen space on
    // top of the real HUD every frame -- visually stacked on top of and
    // fighting with AWallhackVRHUDActor's curved compositor layer, which is
    // the only HUD Quest should ever show. Quest now gets the bare AHUD
    // (draws nothing) so the compositor layer is the sole visible HUD.
#if PLATFORM_ANDROID
    HUDClass = AHUD::StaticClass();
#else
    HUDClass = AWallhackHUD::StaticClass();
#endif

    // The project had no Pawn class set anywhere (Content/ is empty, no
    // Blueprint Pawn, default map is the generic /Engine/Maps/Entry), so it
    // was falling back to ADefaultPawn, which has no camera component at
    // all. See WallhackVRPawn.h for the full explanation of why that's why
    // the HUD panel never appeared even though passthrough itself tracked
    // head movement fine.
    DefaultPawnClass = AWallhackVRPawn::StaticClass();
}

void AWallhackGameMode::BeginPlay()
{
    Super::BeginPlay();

    // On Quest, this creates a real-world camera layer beneath the native HUD.
#if PLATFORM_ANDROID
    if (GEngine && GEngine->XRSystem.IsValid()) GEngine->XRSystem->SetTrackingOrigin(EHMDTrackingOrigin::Stage);
    if (UOculusXRPassthroughSubsystem* Passthrough = UOculusXRPassthroughSubsystem::GetPassthroughSubsystem(GetWorld()))
    {
        FOculusXRPersistentPassthroughParameters Parameters;
        Passthrough->InitializePersistentPassthrough(Parameters, FOculusXRPassthrough_LayerResumed_Single());
    }
#endif

    // The compositor layer only belongs in the Quest build.  Windows uses
    // AWallhackHUD's clearly-labelled simulated operator preview instead.
    const bool bDemo = FParse::Param(FCommandLine::Get(), TEXT("WallhackDemo")) || FParse::Param(FCommandLine::Get(), TEXT("WallhackTrackingPreview"));
    const bool bBridge = FParse::Param(FCommandLine::Get(), TEXT("WallhackBridge"));
    bool bCreateSpatialHUD = bDemo || bBridge || FParse::Param(FCommandLine::Get(), TEXT("WallhackNavigationPreview"));
#if PLATFORM_ANDROID
    bCreateSpatialHUD = true;
#endif
    if (GetWorld() && bCreateSpatialHUD)
    {
        if (bDemo) WorldContact = GetWorld()->SpawnActor<AWallhackWorldContact>();
        else if (!bBridge) GetWorld()->GetSubsystem<UWallhackNavigationSubsystem>()->Start();
        VRHUDActor = GetWorld()->SpawnActor<AWallhackVRHUDActor>();
        VRHUDActor->SetWorldContact(WorldContact);
    }
}
