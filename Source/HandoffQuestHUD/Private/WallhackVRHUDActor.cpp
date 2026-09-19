#include "WallhackVRHUDActor.h"

#include "Components/SceneComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "WallhackHUDProjection.h"
#include "Components/StereoLayerComponent.h"
#include "CanvasTypes.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/TextureRenderTarget2D.h"
#include "UObject/ConstructorHelpers.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputMappingContext.h"
#include "InputTriggers.h"
#include "InputModifiers.h"
#include "IXRTrackingSystem.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "MotionControllerComponent.h"
#include "Misc/ScopeExit.h"
#include "WallhackHUDSettings.h"
#include "WallhackHUDPalette.h"
#include "WallhackTelemetrySubsystem.h"
#include "WallhackWorldContact.h"
#include "WallhackNavigationSubsystem.h"
#include "WallhackSpatialMath.h"
#include "WallhackCanvasLabels.h"
#include "EngineUtils.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/IConsoleManager.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace
{
    bool IsDesktopTrackingPreview()
    {
#if !PLATFORM_ANDROID && !UE_BUILD_SHIPPING
        return FParse::Param(FCommandLine::Get(), TEXT("WallhackTrackingPreview")) || FParse::Param(FCommandLine::Get(), TEXT("WallhackNavigationPreview"));
#else
        return false;
#endif
    }

    FAutoConsoleCommandWithWorld ExportSpatialHUDCommand(
        TEXT("wallhack.ExportHUD"), TEXT("Export the current spatial HUD texture to Saved/TrackingVerification/SpatialHUD.png."),
        FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
        {
            if (World) for (TActorIterator<AWallhackVRHUDActor> It(World); It; ++It) It->ExportSpatialHUD();
        }));
    constexpr float CanvasWidth = 2048.f;
    constexpr float CanvasHeight = 1152.f;
    constexpr float BootSeconds = 1.1f;      // frame power-on duration
    constexpr float EventLifetimeSeconds = 4.5f;
    const FLinearColor Cyan = WallhackHUDPalette::Accent;
    const FLinearColor DimCyan = WallhackHUDPalette::Secondary;
    const FLinearColor Good = WallhackHUDPalette::Accent;
    const FLinearColor Warning(1.f, 0.62f, 0.04f, 1.f);
    const FLinearColor Danger(1.f, 0.15f, 0.10f, 1.f);

    FString LinkStateText(EWallhackLinkState State)
    {
        switch (State)
        {
            case EWallhackLinkState::Live: return TEXT("GROUND LINK / LIVE");
            case EWallhackLinkState::Connecting: return TEXT("GROUND LINK / ACQUIRING");
            case EWallhackLinkState::Stale: return TEXT("GROUND LINK / STALE");
            default: return TEXT("GROUND LINK / OFFLINE");
        }
    }

    FLinearColor LinkStateColor(EWallhackLinkState State)
    {
        return State == EWallhackLinkState::Live ? Good : State == EWallhackLinkState::Connecting ? Warning : Danger;
    }

    FLinearColor WithAlpha(FLinearColor Color, float Alpha)
    {
        Color.A *= Alpha;
        return Color;
    }
}

void UWallhackCurvedStereoLayerComponent::ConfigureVisorCurve(float Radius, float ArcLength, int32 Height)
{
    UStereoLayerShapeCylinder* CurvedShape = NewObject<UStereoLayerShapeCylinder>(this, TEXT("WallhackVisorCurve"));
    CurvedShape->SetRadius(Radius);
    CurvedShape->SetOverlayArc(ArcLength);
    CurvedShape->SetHeight(Height);
    Shape = CurvedShape;
    StereoLayerType = SLT_FaceLocked;
    MarkStereoLayerDirty();
}

AWallhackVRHUDActor::AWallhackVRHUDActor()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickInterval = 0.f;
    // Sample after camera updates, then submit the layer in the same tick group.
    PrimaryActorTick.TickGroup = TG_PostUpdateWork;

    Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    SetRootComponent(Root);

    // Two seven-segment ID digits per contact, rendered in real stereo using
    // the same cooked sphere/material as the dots. No extra font asset or
    // screen-space approximation is needed for their spatial registration.
    ContactLabels = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("SpatialContactLabels"));
    ContactLabels->SetupAttachment(Root);
    ContactLabels->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    ContactLabels->SetCastShadow(false);
    ContactLabels->SetGenerateOverlapEvents(false);
    ContactLabels->SetHiddenInGame(true);
    static ConstructorHelpers::FObjectFinder<UStaticMesh> LabelSphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
    static ConstructorHelpers::FObjectFinder<UMaterialInterface> LabelMaterial(TEXT("/Game/Materials/M_WallhackContact.M_WallhackContact"));
    if (LabelSphere.Succeeded()) ContactLabels->SetStaticMesh(LabelSphere.Object);
    if (LabelMaterial.Succeeded()) ContactLabels->SetMaterial(0, LabelMaterial.Object);

    StereoLayer = CreateDefaultSubobject<UWallhackCurvedStereoLayerComponent>(TEXT("WallhackCompositorLayer"));
    StereoLayer->SetupAttachment(Root);
    StereoLayer->SetTickGroup(TG_PostUpdateWork);
    StereoLayer->AddTickPrerequisiteActor(this);
    // A cylinder hugs the visor instead of presenting a distant flat screen.
    // This default quad size is overwritten by ConfigureVisorCurve() in
    // BeginPlay() once the Shape is swapped to a cylinder; see there for
    // the actual arc dimensions.
    StereoLayer->SetQuadSize(FVector2D(280.f, 180.f));
    StereoLayer->SetPriority(100);
    StereoLayer->bLiveTexture = true;
    StereoLayer->bSupportsDepth = false;
    StereoLayer->bNoAlphaChannel = false;
    StereoLayer->SetRelativeLocation(FVector::ZeroVector);
    StereoLayer->SetRelativeRotation(FRotator::ZeroRotator);

    // Tracks the physical left controller so DrawOperatorHUD can tell how
    // close it is to the headset -- that's the "raised your wrist to look
    // at it" gesture the comms panel shows on. See LeftHandController's
    // comment in the header for the rest of that feature.
    LeftHandController = CreateDefaultSubobject<UMotionControllerComponent>(TEXT("WallhackLeftHandController"));
    LeftHandController->SetupAttachment(Root);
    LeftHandController->SetTrackingMotionSource(FName(TEXT("Left")));

    // The imported Offline font reference remains disabled: on 5 Sept its
    // inclusion reproduced an Android BufferReader.h:52 startup assertion,
    // including after reimport, and removing it restored launch. Runtime
    // engine fonts are supported; local composite tests on 7 Sept traced
    // invisible text to RGB-only canvas blending. Text() now writes alpha.
    // static ConstructorHelpers::FObjectFinder<UFont> HudFontFinder(TEXT("/Game/Roboto-Regular_Font.Roboto-Regular_Font"));
    // if (HudFontFinder.Succeeded())
    // {
    //     HardRefFont = HudFontFinder.Object;
    // }
}

AWallhackVRHUDActor::~AWallhackVRHUDActor() = default;

void AWallhackVRHUDActor::BeginPlay()
{
    Super::BeginPlay();

    // Cylinder arc is specified as arc length in Unreal units. The previous
    // 422cm/270cm arc at 220cm radius worked out to ~110 deg horizontal by
    // ~63 deg vertical -- wider and taller than the headset's comfortable,
    // undistorted FOV, so panels laid out near the edges of the 2048x1152
    // canvas (telemetry top-right, minimap/drone bottom corners) landed
    // outside what the operator could actually see and looked "cut off".
    // Tightened to ~80 deg horizontal by ~48 deg vertical, wrapped closer
    // to the visor, so the whole canvas sits inside the visible cone.
    StereoLayer->ConfigureVisorCurve(200.f, 280.f, 180);

    HUDRenderTarget = NewObject<UTextureRenderTarget2D>(this, TEXT("WallhackOperatorHUD"));
    HUDRenderTarget->ClearColor = FLinearColor(0.f, 0.f, 0.f, 0.f);
    // UI colors are LDR. RGBA8 preserves alpha at half RGBA16f's storage/bandwidth.
    HUDRenderTarget->RenderTargetFormat = RTF_RGBA8;
    HUDRenderTarget->InitAutoFormat(static_cast<uint32>(CanvasWidth), static_cast<uint32>(CanvasHeight));
    HUDRenderTarget->UpdateResourceImmediate(true);
    StereoLayer->SetTexture(HUDRenderTarget);
    StereoLayer->MarkTextureForUpdate();
    SpatialLabelAtlas = WallhackCanvasLabels::CreateAtlas();

    // --- Interactive HUD density toggle -------------------------------
    // Modeled on the "immersive/photo mode" toggle nearly every modern
    // game ships, bound to the right controller's B button. It's not just
    // a game-style flourish here: this is a real AR overlay over the
    // operator's actual surroundings, so a single press to clear it away
    // when the real world needs full attention is a genuine safety
    // control, not a nice-to-have. Built entirely in C++ (Enhanced Input
    // action + mapping context constructed at runtime) since the project
    // has no Content assets to hang a Blueprint input asset off of.
    CycleHUDAction = NewObject<UInputAction>(this, TEXT("WallhackCycleHUDAction"));
    CycleHUDAction->ValueType = EInputActionValueType::Boolean;

    // Left-controller X re-zeroes the compass reference. In spatial mode
    // this changes compass labels only; world contact and local-map geometry
    // continue to use the tracked viewer pose. Construct the action at runtime.
    CalibrateNorthAction = NewObject<UInputAction>(this, TEXT("WallhackCalibrateNorthAction"));
    CalibrateNorthAction->ValueType = EInputActionValueType::Boolean;

    // Left-controller trigger: push-to-talk for the wrist-raise comms panel
    // (see TransmitAction's comment in the header -- UI/button state only,
    // no audio path wired up yet).
    TransmitAction = NewObject<UInputAction>(this, TEXT("WallhackTransmitAction"));
    TransmitAction->ValueType = EInputActionValueType::Boolean;
    PlaceContactAction = NewObject<UInputAction>(this, TEXT("WallhackPlaceContactAction"));
    PlaceContactAction->ValueType = EInputActionValueType::Boolean;
    SelectContactAction = NewObject<UInputAction>(this, TEXT("WallhackSelectContactAction"));
    SelectContactAction->ValueType = EInputActionValueType::Boolean;
    MapRangeAction = NewObject<UInputAction>(this, TEXT("WallhackMapRangeAction"));
    MapRangeAction->ValueType = EInputActionValueType::Boolean;
    PauseSimulationAction = NewObject<UInputAction>(this, TEXT("WallhackPauseSimulationAction"));
    PauseSimulationAction->ValueType = EInputActionValueType::Boolean;
    ContactUpdatesAction = NewObject<UInputAction>(this, TEXT("WallhackContactUpdatesAction"));
    ContactUpdatesAction->ValueType = EInputActionValueType::Boolean;

    HUDMappingContext = NewObject<UInputMappingContext>(this, TEXT("WallhackHUDMappingContext"));
    HUDMappingContext->MapKey(CycleHUDAction, EKeys::OculusTouch_Right_B_Click);
    HUDMappingContext->MapKey(CalibrateNorthAction, EKeys::OculusTouch_Left_X_Click);
    const bool bDemo = IsValid(WorldContact) || FParse::Param(FCommandLine::Get(), TEXT("WallhackDemo")) || FParse::Param(FCommandLine::Get(), TEXT("WallhackTrackingPreview"));
    if (bDemo)
    {
        HUDMappingContext->MapKey(TransmitAction, EKeys::OculusTouch_Left_Trigger_Click);
        HUDMappingContext->MapKey(PlaceContactAction, EKeys::OculusTouch_Right_A_Click);
        HUDMappingContext->MapKey(SelectContactAction, EKeys::OculusTouch_Right_Trigger_Click);
        HUDMappingContext->MapKey(MapRangeAction, EKeys::OculusTouch_Right_Thumbstick_Click);
        HUDMappingContext->MapKey(PauseSimulationAction, EKeys::OculusTouch_Left_Y_Click);
        HUDMappingContext->MapKey(ContactUpdatesAction, EKeys::OculusTouch_Left_Thumbstick_Click);
    }
    else
    {
        NavigationAimAction=NewObject<UInputAction>(this,TEXT("NavigationAim"));
        NavigationConfirmAction=NewObject<UInputAction>(this,TEXT("NavigationConfirm"));
        NavigationCancelAction=NewObject<UInputAction>(this,TEXT("NavigationCancel"));
        HUDMappingContext->MapKey(NavigationAimAction,EKeys::OculusTouch_Right_Grip_Click);
        HUDMappingContext->MapKey(NavigationConfirmAction,EKeys::OculusTouch_Right_Trigger_Click);
        HUDMappingContext->MapKey(NavigationCancelAction,EKeys::OculusTouch_Right_A_Click);
        HUDMappingContext->MapKey(NavigationAimAction,EKeys::G);
        HUDMappingContext->MapKey(NavigationConfirmAction,EKeys::Enter);
        HUDMappingContext->MapKey(NavigationCancelAction,EKeys::C);
        PeopleModeAction=NewObject<UInputAction>(this,TEXT("PeopleMode"));
        PeopleSelectAction=NewObject<UInputAction>(this,TEXT("PeopleSelect"));
        PeopleHeightAction=NewObject<UInputAction>(this,TEXT("PeopleHeight"));
        PeopleFacingAction=NewObject<UInputAction>(this,TEXT("PeopleFacing"));
        PeopleMoveAction=NewObject<UInputAction>(this,TEXT("PeopleMove"));
        PeopleHeightAction->ValueType=EInputActionValueType::Axis1D;
        PeopleFacingAction->ValueType=EInputActionValueType::Axis1D;
        HUDMappingContext->MapKey(PeopleModeAction,EKeys::OculusTouch_Left_Y_Click);
        HUDMappingContext->MapKey(PeopleSelectAction,EKeys::OculusTouch_Right_Thumbstick_Click);
        HUDMappingContext->MapKey(PeopleMoveAction,EKeys::OculusTouch_Left_Trigger_Click);
        HUDMappingContext->MapKey(PeopleMoveAction,EKeys::R);
        HUDMappingContext->MapKey(PeopleHeightAction,EKeys::OculusTouch_Right_Thumbstick_Y);
        HUDMappingContext->MapKey(PeopleFacingAction,EKeys::OculusTouch_Right_Thumbstick_X);
        HUDMappingContext->MapKey(PeopleModeAction,EKeys::H);
        HUDMappingContext->MapKey(PeopleSelectAction,EKeys::Tab);
        HUDMappingContext->MapKey(PeopleHeightAction,EKeys::RightBracket);
        HUDMappingContext->MapKey(PeopleHeightAction,EKeys::LeftBracket).Modifiers.Add(NewObject<UInputModifierNegate>(this));
        HUDMappingContext->MapKey(PeopleFacingAction,EKeys::Period);
        HUDMappingContext->MapKey(PeopleFacingAction,EKeys::Comma).Modifiers.Add(NewObject<UInputModifierNegate>(this));
    }
    if (IsDesktopTrackingPreview())
    {
        if(bDemo) HUDMappingContext->MapKey(PlaceContactAction, EKeys::SpaceBar);
        HUDMappingContext->MapKey(CycleHUDAction, EKeys::B);
        HUDMappingContext->MapKey(CalibrateNorthAction, EKeys::X);
        if(bDemo) HUDMappingContext->MapKey(SelectContactAction, EKeys::Tab);
        HUDMappingContext->MapKey(MapRangeAction, EKeys::M);
        HUDMappingContext->MapKey(PauseSimulationAction, EKeys::P);
        HUDMappingContext->MapKey(ContactUpdatesAction, EKeys::F);
    }

    if (APlayerController* PC = UGameplayStatics::GetPlayerController(this, 0))
    {
        AutoReceiveInput = EAutoReceiveInput::Player0;
        EnableInput(PC);
        if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
        {
            Subsystem->AddMappingContext(HUDMappingContext, 0);
        }
        if (UEnhancedInputComponent* EIC = Cast<UEnhancedInputComponent>(InputComponent))
        {
            if(NavigationAimAction)
            {
                EIC->BindAction(NavigationAimAction,ETriggerEvent::Started,this,&AWallhackVRHUDActor::BeginNavigationAim);
                EIC->BindAction(NavigationAimAction,ETriggerEvent::Completed,this,&AWallhackVRHUDActor::EndNavigationAim);
                EIC->BindAction(NavigationAimAction,ETriggerEvent::Canceled,this,&AWallhackVRHUDActor::EndNavigationAim);
                EIC->BindAction(NavigationConfirmAction,ETriggerEvent::Started,this,&AWallhackVRHUDActor::ConfirmNavigation);
                EIC->BindAction(NavigationCancelAction,ETriggerEvent::Started,this,&AWallhackVRHUDActor::CancelNavigation);
                EIC->BindAction(PeopleModeAction,ETriggerEvent::Started,this,&AWallhackVRHUDActor::TogglePeopleEditing);
                EIC->BindAction(PeopleSelectAction,ETriggerEvent::Started,this,&AWallhackVRHUDActor::SelectNextPerson);
                EIC->BindAction(PeopleMoveAction,ETriggerEvent::Started,this,&AWallhackVRHUDActor::MoveSelectedPerson);
                EIC->BindAction(PeopleHeightAction,ETriggerEvent::Triggered,this,&AWallhackVRHUDActor::AdjustPersonHeight);
                EIC->BindAction(PeopleFacingAction,ETriggerEvent::Triggered,this,&AWallhackVRHUDActor::AdjustPersonFacing);
            }
            EIC->BindAction(CycleHUDAction, ETriggerEvent::Started, this, &AWallhackVRHUDActor::CycleHUDDensity);
            EIC->BindAction(CalibrateNorthAction, ETriggerEvent::Started, this, &AWallhackVRHUDActor::CalibrateNorth);
            EIC->BindAction(PlaceContactAction, ETriggerEvent::Started, this, &AWallhackVRHUDActor::PlaceWorldContact);
            EIC->BindAction(SelectContactAction, ETriggerEvent::Started, this, &AWallhackVRHUDActor::SelectNextContact);
            EIC->BindAction(MapRangeAction, ETriggerEvent::Started, this, &AWallhackVRHUDActor::CycleMapRange);
            EIC->BindAction(PauseSimulationAction, ETriggerEvent::Started, this, &AWallhackVRHUDActor::ToggleSimulation);
            EIC->BindAction(ContactUpdatesAction, ETriggerEvent::Started, this, &AWallhackVRHUDActor::ToggleContactUpdates);
            EIC->BindAction(TransmitAction, ETriggerEvent::Started, this, &AWallhackVRHUDActor::StartTransmit);
            EIC->BindAction(TransmitAction, ETriggerEvent::Completed, this, &AWallhackVRHUDActor::StopTransmit);
            EIC->BindAction(TransmitAction, ETriggerEvent::Canceled, this, &AWallhackVRHUDActor::StopTransmit);
        }
    }

    PushEvent(TEXT("WALLHACK HUD ONLINE"), Good);
    DrawOperatorHUD();
    UE_LOG(LogTemp, Display, TEXT("Wallhack HUD: full-FOV operator layer attached (%dx%d)"), HUDRenderTarget->SizeX, HUDRenderTarget->SizeY);
    UE_LOG(LogTemp, Display, TEXT("Wallhack HUD: heading refresh every game frame, direct tracked yaw (no smoothing)"));
    UE_LOG(LogTemp, Display, TEXT("Wallhack HUD v2: scrolling compass ticks, face-locked RGBA8 layer, post-camera tick"));
    UE_LOG(LogTemp, Display, TEXT("Wallhack HUD v3: batched canvas submission (one flush per HUD redraw)"));
}

void AWallhackVRHUDActor::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    UpdateSpatialLabels();
    ElapsedSeconds += DeltaSeconds;
    if (DeltaSeconds > KINDA_SMALL_NUMBER)
    {
        const float InstantFPS = 1.f / DeltaSeconds;
        SmoothedFPS = SmoothedFPS <= 0.f ? InstantFPS : FMath::Lerp(SmoothedFPS, InstantFPS, 0.1f);
    }
    // Head-driven graphics must follow the game's frame rate, independently
    // of the bridge's telemetry rate. A 20 Hz redraw adds visible head-turn lag.
    const double SubmitStart = FPlatformTime::Seconds();
    DrawOperatorHUD(DeltaSeconds);
#if !UE_BUILD_SHIPPING
    if (DeltaSeconds > 0.f && DeltaSeconds < 0.25f)
    {
        PerformanceWindowSeconds += DeltaSeconds;
        HUDSubmitSeconds += FPlatformTime::Seconds() - SubmitStart;
        ++PerformanceFrames;
        if (PerformanceWindowSeconds >= 5.f)
        {
            // CPU submission cost only; this is not GPU time or motion-to-photon latency.
            UE_LOG(LogTemp, Display, TEXT("Wallhack HUD perf: game_fps=%.1f frame_ms=%.2f hud_cpu_submit_ms=%.2f density=%d"),
                PerformanceFrames / PerformanceWindowSeconds,
                PerformanceWindowSeconds * 1000.f / PerformanceFrames,
                HUDSubmitSeconds * 1000.0 / PerformanceFrames, static_cast<int32>(HUDDensity));
            PerformanceWindowSeconds = 0.f;
            HUDSubmitSeconds = 0.0;
            PerformanceFrames = 0;
        }
    }
    else
    {
        PerformanceWindowSeconds = 0.f;
        HUDSubmitSeconds = 0.0;
        PerformanceFrames = 0;
    }
#endif
}

void AWallhackVRHUDActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (APlayerController* PC = UGameplayStatics::GetPlayerController(this, 0))
    {
        if (ULocalPlayer* Player = PC->GetLocalPlayer())
            if (auto* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(Player))
                if (HUDMappingContext) Subsystem->RemoveMappingContext(HUDMappingContext);
    }
    if (ContactLabels) ContactLabels->SetHiddenInGame(true);
    Super::EndPlay(EndPlayReason);
}

void AWallhackVRHUDActor::CycleHUDDensity()
{
    HUDDensity = static_cast<EWallhackHUDDensity>((static_cast<uint8>(HUDDensity) + 1) % 3);
    SpatialHelpUntilSeconds = ElapsedSeconds + 4.f;
    if (WorldContact) WorldContact->SetActorHiddenInGame(HUDDensity == EWallhackHUDDensity::Hidden);
    if (!WorldContact) if(auto* Nav=GetWorld()->GetSubsystem<UWallhackNavigationSubsystem>()) Nav->SetHidden(HUDDensity == EWallhackHUDDensity::Hidden);
    UpdateSpatialLabels();
    switch (HUDDensity)
    {
        case EWallhackHUDDensity::Full: PushEvent(TEXT("HUD / FULL"), Cyan); break;
        case EWallhackHUDDensity::Minimal: PushEvent(TEXT("HUD / MINIMAL"), Warning); break;
        case EWallhackHUDDensity::Hidden: PushEvent(TEXT("HUD / HIDDEN"), Warning); break;
    }
    DrawOperatorHUD();
}

void AWallhackVRHUDActor::CalibrateNorth()
{
    if(!WorldContact)
    {
        if(const auto* Nav=GetWorld()->GetSubsystem<UWallhackNavigationSubsystem>())
        {
            const auto& D=Nav->GetDisplaySnapshot();
            if(D.bGuidance){NorthOffsetDegrees=-D.Orientation.Rotator().Yaw;bHasAutoCalibratedNorth=true;DrawOperatorHUD();}
        }
        return;
    }
    FQuat Orientation = FQuat::Identity;
    FVector Position = FVector::ZeroVector;
    const bool bHasPose = IsValid(WorldContact)
        ? WorldContact->GetViewerWorldPose(Position, Orientation)
        : GEngine && GEngine->XRSystem.IsValid() &&
            GEngine->XRSystem->GetCurrentPose(IXRTrackingSystem::HMDDeviceId, Orientation, Position);
    if (!bHasPose)
    {
        PushEvent(TEXT("HEADING TRACKING UNAVAILABLE"), Warning);
        return;
    }
    const float RawYaw = FMath::Fmod(Orientation.Rotator().Yaw + 360.f, 360.f);
    // The current facing direction becomes "000" on the compass ribbon.
    // The local spatial map and world contact do not use this display offset.
    NorthOffsetDegrees = FMath::Fmod(360.f - RawYaw, 360.f);
    bHasAutoCalibratedNorth = true;
    PushEvent(TEXT("NORTH CALIBRATED"), Good);
    DrawOperatorHUD();
}

void AWallhackVRHUDActor::SetWorldContact(AWallhackWorldContact* Contact)
{
    WorldContact = Contact;
    if (WorldContact) AddTickPrerequisiteActor(WorldContact);
}

void AWallhackVRHUDActor::PlaceWorldContact()
{
    if (WorldContact) WorldContact->PlaceInFrontOfViewer();
}

void AWallhackVRHUDActor::ExportSpatialHUD()
{
    if (!HUDRenderTarget) return;
    const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("TrackingVerification"));
    IFileManager::Get().MakeDirectory(*Directory, true);
    UKismetRenderingLibrary::ExportRenderTarget(this, HUDRenderTarget, Directory, TEXT("SpatialHUD.png"));
    UE_LOG(LogTemp, Display, TEXT("Wallhack spatial HUD exported: %s/SpatialHUD.png"), *Directory);
}

void AWallhackVRHUDActor::StartTransmit()
{
    bIsTransmitting = true;
    // TODO: no audio channel to the drone exists yet -- the ground bridge
    // only carries JSON telemetry over its WebSocket today. This flips the
    // HUD state (and, once wired up, would open a mic capture) but doesn't
    // actually send anything anywhere yet. Real fix needs an audio path
    // added to ground/bridge.py and something on the drone/Pi end to play
    // it, which is separate work from this HUD.
    PushEvent(TEXT("MIC / TRANSMITTING"), Danger);
    DrawOperatorHUD();
}

void AWallhackVRHUDActor::StopTransmit()
{
    bIsTransmitting = false;
    PushEvent(TEXT("MIC / CLOSED"), Good);
    DrawOperatorHUD();
}

void AWallhackVRHUDActor::PushEvent(const FString& Text, const FLinearColor& Color)
{
    EventLog.Insert(FWallhackHUDEvent{ Text, Color, ElapsedSeconds }, 0);
    if (EventLog.Num() > 4)
    {
        EventLog.SetNum(4);
    }
}

void AWallhackVRHUDActor::DrawOperatorHUD(float DeltaSeconds)
{
    if (!HUDRenderTarget || !GetWorld()) return;

    UKismetRenderingLibrary::ClearRenderTarget2D(this, HUDRenderTarget, FLinearColor(0.f, 0.f, 0.f, 0.f));
    UCanvas* Canvas = nullptr;
    FVector2D Size;
    FDrawToRenderTargetContext Context;
    UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(this, HUDRenderTarget, Canvas, Size, Context);
    if (!Canvas)
    {
        UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(this, Context);
        return;
    }

    // BeginDrawCanvasToRenderTarget creates an immediate-mode FCanvas. Without
    // this guard, EACH line/text item flushes its own render graph and waits on
    // the RHI thread (measured: 888 submissions / ~220 ms per frame on Quest).
    // This HUD never interleaves material-parameter changes, so defer flushing
    // until EndDraw. Preserve DeleteOnRender and restore flags on every exit.
    const uint32 CanvasModes = Canvas->Canvas->GetAllowedModes();
    Canvas->Canvas->SetAllowedModes(CanvasModes & ~FCanvas::Allow_Flush);
    ON_SCOPE_EXIT
    {
        Canvas->Canvas->SetAllowedModes(CanvasModes);
        UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(this, Context);
        StereoLayer->MarkTextureForUpdate();
    };

    // Prefer an explicitly supplied font, otherwise use the engine's runtime
    // font cache. Runtime glyphs need destination alpha for this compositor
    // texture, which Text() requests below. Sequential assignment keeps the
    // TObjectPtr/raw-pointer fallback compatible with Android Clang.
    UFont* Font = HardRefFont.Get();
    if (!Font && GEngine) { Font = GEngine->GetSmallFont(); }
    if (!Font && GEngine) { Font = GEngine->GetMediumFont(); }
    if (!Font && GEngine) { Font = GEngine->GetLargeFont(); }

    if (IsValid(WorldContact))
    {
        DrawSpatialHUD(Canvas, Font);
        return;
    }
    DrawNavigationHUD(Canvas, Font);
    return;

    const UWallhackTelemetrySubsystem* Telemetry = GetGameInstance() ? GetGameInstance()->GetSubsystem<UWallhackTelemetrySubsystem>() : nullptr;
    FWallhackDisplayFrame Frame = Telemetry ? Telemetry->GetDisplayFrame() : FWallhackDisplayFrame{};

    // Spatial simulation is separate from the ground bridge. Never invent a
    // live link or mix room coordinates with unregistered drone coordinates.
    WallhackSpatialMath::FContactView SpatialView;
    FVector ContactPosition, ViewerPosition;
    FQuat ViewerOrientation;
    const bool bSpatialMode = IsValid(WorldContact);
    const bool bSpatialTracked = bSpatialMode &&
        WorldContact->GetContactWorldPosition(ContactPosition) &&
        WorldContact->GetViewerWorldPose(ViewerPosition, ViewerOrientation) &&
        WallhackSpatialMath::ProjectContact(ContactPosition, ViewerPosition,
            ViewerOrientation.Rotator().Yaw, GetWorld()->GetWorldSettings()->WorldToMeters, SpatialView);
    FQuat HmdOrientation = FQuat::Identity;
    FVector HmdPosition = FVector::ZeroVector;
    bool bHasHmdPose = GEngine && GEngine->XRSystem.IsValid() &&
        GEngine->XRSystem->GetCurrentPose(IXRTrackingSystem::HMDDeviceId, HmdOrientation, HmdPosition);
    if (!bHasHmdPose && IsDesktopTrackingPreview() && IsValid(WorldContact))
    {
        bHasHmdPose = WorldContact->GetViewerWorldPose(HmdPosition, HmdOrientation);
    }
    const bool bLive = Frame.LinkState == EWallhackLinkState::Live;

    // Track state changes for the comms ticker -- a real operator glances at
    // a log, not a wall of static numbers, to know what just happened.
    if (Frame.LinkState != LastLinkState)
    {
        PushEvent(LinkStateText(Frame.LinkState), LinkStateColor(Frame.LinkState));
        LastLinkState = Frame.LinkState;
    }
    // A lock is only ever earned while the link is actually live (see the
    // target-lock block further down) -- so the moment the link stops being
    // live, drop it explicitly instead of leaving a stale LockedContactId
    // sitting around unannounced. Same "never imply a capability that isn't
    // there" rule as the stale-data gate: a HUD that's still quietly "locked"
    // onto a contact it can no longer see is lying by omission.
    if (!bLive && LockedContactId != -1)
    {
        PushEvent(TEXT("TARGET LOCK LOST"), Warning);
        LockedContactId = -1;
        LockAcquiredAtSeconds = ElapsedSeconds;
    }
    if (bLive)
    {
        TSet<int32> CurrentIds;
        for (const FWallhackContact& Contact : Frame.Contacts)
        {
            CurrentIds.Add(Contact.Id);
            if (!KnownContactIds.Contains(Contact.Id))
            {
                PushEvent(FString::Printf(TEXT("CONTACT %02d ACQUIRED"), Contact.Id), Good);
            }
        }
        for (const int32 Id : KnownContactIds)
        {
            if (!CurrentIds.Contains(Id))
            {
                PushEvent(FString::Printf(TEXT("CONTACT %02d LOST"), Id), Warning);
            }
        }
        KnownContactIds = MoveTemp(CurrentIds);
    }

    // Calibrate the compass display from the first valid viewer yaw, or from
    // a later manual X press. This reference is not measured magnetic/true
    // north. Local spatial projection uses ViewerOrientation directly above,
    // so re-zeroing these labels cannot move the map dot or world contact.
    if (bHasHmdPose)
    {
        const float RawHeadYaw = FMath::Fmod(HmdOrientation.Rotator().Yaw + 360.f, 360.f);
        if (!bHasAutoCalibratedNorth)
        {
            NorthOffsetDegrees = FMath::Fmod(360.f - RawHeadYaw, 360.f);
            bHasAutoCalibratedNorth = true;
        }
        // Hold the last heading on pose failure instead of jumping to identity yaw.
        DisplayedHeading = FMath::UnwindDegrees(RawHeadYaw + NorthOffsetDegrees);
    }
    const int32 Heading = FMath::RoundToInt(FMath::Fmod(DisplayedHeading + 360.f, 360.f)) % 360;

    const float BootAlpha = FMath::Clamp(ElapsedSeconds / BootSeconds, 0.f, 1.f);
    const bool bContentReady = BootAlpha >= 1.f;

    auto Line = [Canvas](float X1, float Y1, float X2, float Y2, const FLinearColor& Color, float Thickness = 2.f)
    {
        // Preserve fade/hidden states while separating the green from scenery.
        Canvas->K2_DrawLine(FVector2D(X1, Y1), FVector2D(X2, Y2), Thickness + 3.f, FLinearColor(0.f, 0.f, 0.f, Color.A));
        Canvas->K2_DrawLine(FVector2D(X1, Y1), FVector2D(X2, Y2), Thickness, Color);
    };
    auto Text = [Canvas, Font](const FString& Value, float X, float Y, float Scale, const FLinearColor& Color, bool bCentre = false)
    {
        if (!Canvas->Canvas || !Font || Value.IsEmpty()) return;

        // The old canvas blend wrote text RGB but left destination alpha zero,
        // making the labels invisible in the compositor. This flag makes the
        // runtime font path select SE_BLEND_TranslucentAlphaOnlyWriteAlpha.
        const bool bPreviousWriteAlpha = Canvas->Canvas->IsWriteDestinationAlphaSet();
        Canvas->Canvas->SetWriteDestinationAlpha(true);
        Canvas->K2_DrawText(Font, Value, FVector2D(X, Y), FVector2D(Scale, Scale), Color, 0.f, FLinearColor(0.f, 0.f, 0.f, Color.A), FVector2D(1.f, 1.f), bCentre, false, true, FLinearColor(0.f, 0.f, 0.f, Color.A));
        Canvas->Canvas->SetWriteDestinationAlpha(bPreviousWriteAlpha);
    };
    auto Label = [this, Canvas](const FString& Value, float X, float Y, float Height, const FLinearColor& Color, bool bCentre = false)
    {
        WallhackCanvasLabels::Draw(Canvas, SpatialLabelAtlas, Value, X, Y, Height, Color, bCentre);
    };

    // Seven-segment numeric readouts retain their stroke-based HUD style.
    // Covers 0-9, '.', '-', '/', ':', and a degree mark; other characters
    // occupy blank space. Text() and Label() draw the accompanying labels.
    auto SegChar = [Line, Circle = [Canvas](float CX, float CY, float R, const FLinearColor& Color, float Thickness)
        {
            constexpr int32 Segs = 8;
            FVector2D Prev(CX + R, CY);
            for (int32 Index = 1; Index <= Segs; ++Index)
            {
                const float Angle = (2.f * UE_PI * Index) / Segs;
                const FVector2D Next(CX + R * FMath::Cos(Angle), CY + R * FMath::Sin(Angle));
                Canvas->K2_DrawLine(Prev, Next, Thickness, Color);
                Prev = Next;
            }
        }](TCHAR Ch, float X, float Y, float H, const FLinearColor& Color, float Thickness) -> float
    {
        const float W = H * 0.56f;
        const float Gap = H * 0.22f;
        const float MidY = Y + H * 0.5f;
        auto Seg = [&](bool bOn, float X1, float Y1, float X2, float Y2) { if (bOn) { Line(X1, Y1, X2, Y2, Color, Thickness); } };
        if (Ch >= TEXT('0') && Ch <= TEXT('9'))
        {
            static const bool Table[10][7] =
            {
                { true,  true,  true,  true,  true,  true,  false }, // 0
                { false, true,  true,  false, false, false, false }, // 1
                { true,  true,  false, true,  true,  false, true  }, // 2
                { true,  true,  true,  true,  false, false, true  }, // 3
                { false, true,  true,  false, false, true,  true  }, // 4
                { true,  false, true,  true,  false, true,  true  }, // 5
                { true,  false, true,  true,  true,  true,  true  }, // 6
                { true,  true,  true,  false, false, false, false }, // 7
                { true,  true,  true,  true,  true,  true,  true  }, // 8
                { true,  true,  true,  true,  false, true,  true  }, // 9
            };
            const bool* S = Table[Ch - TEXT('0')];
            Seg(S[0], X, Y, X + W, Y);            // a: top
            Seg(S[1], X + W, Y, X + W, MidY);     // b: top-right
            Seg(S[2], X + W, MidY, X + W, Y + H); // c: bottom-right
            Seg(S[3], X, Y + H, X + W, Y + H);    // d: bottom
            Seg(S[4], X, MidY, X, Y + H);         // e: bottom-left
            Seg(S[5], X, Y, X, MidY);             // f: top-left
            Seg(S[6], X, MidY, X + W, MidY);      // g: middle
            return W + Gap;
        }
        if (Ch == TEXT('.'))
        {
            Line(X, Y + H - Thickness * 0.5f, X + Thickness, Y + H, Color, Thickness * 1.6f);
            return Thickness * 2.5f + Gap;
        }
        if (Ch == TEXT('-'))
        {
            Line(X, MidY, X + W * 0.7f, MidY, Color, Thickness);
            return W * 0.7f + Gap;
        }
        if (Ch == TEXT('/'))
        {
            Line(X, Y + H, X + W, Y, Color, Thickness);
            return W + Gap;
        }
        if (Ch == TEXT(':'))
        {
            Line(X + W * 0.5f, Y + H * 0.26f, X + W * 0.5f, Y + H * 0.34f, Color, Thickness * 1.6f);
            Line(X + W * 0.5f, Y + H * 0.66f, X + W * 0.5f, Y + H * 0.74f, Color, Thickness * 1.6f);
            return W * 0.5f + Gap;
        }
        if (Ch == TEXT('\xB0')) // degree sign
        {
            Circle(X + H * 0.14f, Y + H * 0.14f, H * 0.14f, Color, FMath::Max(1.f, Thickness * 0.8f));
            return H * 0.28f + Gap;
        }
        return W * 0.6f + Gap; // space / unsupported char -> blank advance
    };
    auto SegNum = [SegChar](const FString& Str, float X, float Y, float H, const FLinearColor& Color, float Thickness, bool bCentre)
    {
        const float EstAdvance = H * 0.78f;
        float CursorX = bCentre ? (X - Str.Len() * EstAdvance * 0.5f) : X;
        for (TCHAR Ch : Str)
        {
            CursorX += SegChar(Ch, CursorX, Y, H, Color, Thickness);
        }
    };

    auto StrokePath = [Canvas](TConstArrayView<FVector2D> Points, const FLinearColor& Color, float Thickness)
    {
        // Draw the complete outline before any foreground segments. A black
        // outline per segment erases adjacent green segments on small curves.
        for (int32 Pass = 0; Pass < 2; ++Pass)
        {
            const FLinearColor PassColor = Pass == 0 ? FLinearColor(0.f, 0.f, 0.f, Color.A) : Color;
            const float PassThickness = Pass == 0 ? Thickness + 3.f : Thickness;
            for (int32 Index = 1; Index < Points.Num(); ++Index)
            {
                Canvas->K2_DrawLine(Points[Index - 1], Points[Index], PassThickness, PassColor);
            }
        }
    };
    auto Circle = [StrokePath](float CX, float CY, float R, const FLinearColor& Color, float Thickness)
    {
        constexpr int32 Segments = 56;
        TArray<FVector2D, TInlineAllocator<Segments + 1>> Points;
        Points.Add(FVector2D(CX + R, CY));
        for (int32 Index = 1; Index <= Segments; ++Index)
        {
            const float Angle = (2.f * UE_PI * Index) / Segments;
            Points.Add(FVector2D(CX + R * FMath::Cos(Angle), CY + R * FMath::Sin(Angle)));
        }
        StrokePath(MakeArrayView(Points.GetData(), Points.Num()), Color, Thickness);
    };
    // Draws only the wedge of the ring between two angles (radians, standard
    // screen convention: 0 = +X/right, +90deg = +Y/down). Used to render the
    // tactical map's actual sensor coverage instead of always implying 360.
    auto Arc = [StrokePath](float CX, float CY, float R, float StartAngle, float EndAngle, const FLinearColor& Color, float Thickness)
    {
        const int32 Segments = FMath::Max(2, FMath::RoundToInt(FMath::Abs(EndAngle - StartAngle) / FMath::DegreesToRadians(6.f)));
        TArray<FVector2D, TInlineAllocator<64>> Points;
        Points.Reserve(Segments + 1);
        Points.Add(FVector2D(CX + R * FMath::Cos(StartAngle), CY + R * FMath::Sin(StartAngle)));
        for (int32 Index = 1; Index <= Segments; ++Index)
        {
            const float Angle = FMath::Lerp(StartAngle, EndAngle, static_cast<float>(Index) / Segments);
            Points.Add(FVector2D(CX + R * FMath::Cos(Angle), CY + R * FMath::Sin(Angle)));
        }
        StrokePath(MakeArrayView(Points.GetData(), Points.Num()), Color, Thickness);
    };
    // Two-pass line: a wide, dim pass underneath a thin, bright pass on top --
    // a cheap fake for the emissive bloom a real optical HUD would have,
    // since UCanvas has no blur/glow primitive of its own. Used only for
    // structural framing (bezels, the peripheral frame, the lock reticle),
    // not for data lines (contact markers, leader lines, ticker text) --
    // those stay crisp and single-pass so the HUD doesn't turn into a haze
    // of glow everywhere. Bezels glow like lit chrome; data stays legible.
    auto GlowLine = [Canvas](float X1, float Y1, float X2, float Y2, const FLinearColor& Color, float Thickness)
    {
        Canvas->K2_DrawLine(FVector2D(X1, Y1), FVector2D(X2, Y2), Thickness * 3.2f + 3.f, FLinearColor(0.f, 0.f, 0.f, Color.A));
        Canvas->K2_DrawLine(FVector2D(X1, Y1), FVector2D(X2, Y2), Thickness * 3.2f, WithAlpha(Color, 0.3f));
        Canvas->K2_DrawLine(FVector2D(X1, Y1), FVector2D(X2, Y2), Thickness, Color);
    };
    // Open corner-accent framing instead of a solid rectangle outline -- reads
    // as an instrument bezel rather than a flat UI panel, and is the same
    // visual language as the peripheral frame above so every panel matches.
    auto CornerFrame = [GlowLine](float X, float Y, float W, float H, const FLinearColor& Color, float Thickness, float CornerLen)
    {
        GlowLine(X, Y, X + CornerLen, Y, Color, Thickness);
        GlowLine(X, Y, X, Y + CornerLen, Color, Thickness);
        GlowLine(X + W, Y, X + W - CornerLen, Y, Color, Thickness);
        GlowLine(X + W, Y, X + W, Y + CornerLen, Color, Thickness);
        GlowLine(X, Y + H, X + CornerLen, Y + H, Color, Thickness);
        GlowLine(X, Y + H, X, Y + H - CornerLen, Color, Thickness);
        GlowLine(X + W, Y + H, X + W - CornerLen, Y + H, Color, Thickness);
        GlowLine(X + W, Y + H, X + W, Y + H - CornerLen, Color, Thickness);
    };

    if (HUDDensity == EWallhackHUDDensity::Hidden)
    {
        // Never go fully silent about the HUD's own state -- a HUD you can
        // turn off but can't tell is off (or how to bring back) is a UX
        // trap. One dim, unobtrusive line at the very bottom is enough.
        const float Breathe = 0.35f + 0.25f * FMath::Sin(ElapsedSeconds * 1.6f);
        Text(TEXT("HUD HIDDEN  /  [B] TO RESTORE"), 1024, 1090, 0.8f, WithAlpha(DimCyan, Breathe), true);
        Label(TEXT("B / RESTORE HUD"), 1024, 1070, 13.f, WithAlpha(DimCyan, Breathe), true);
        return;
    }

    // Peripheral frame: keeps the central view clear for the real world.
    // Margins are generous (100px) now that the whole canvas is wrapped
    // across a much smaller visor arc -- every panel below stays well
    // inside this frame instead of running out to the raw canvas edge.
    // Fades in with the rest of the frame during the boot sequence.
    const FLinearColor FrameColor = WithAlpha(Cyan, BootAlpha);
    GlowLine(60, 50, 275, 50, FrameColor, 4); GlowLine(60, 50, 60, 190, FrameColor, 4);
    GlowLine(1988, 50, 1773, 50, FrameColor, 4); GlowLine(1988, 50, 1988, 190, FrameColor, 4);
    GlowLine(60, 1102, 275, 1102, FrameColor, 4); GlowLine(60, 1102, 60, 962, FrameColor, 4);
    GlowLine(1988, 1102, 1773, 1102, FrameColor, 4); GlowLine(1988, 1102, 1988, 962, FrameColor, 4);

    // Heading ribbon appears in Full and Minimal density. Stroke ticks and
    // digits provide the primary readout; the text line adds head/rig context.
    CornerFrame(424, 90, 1200, 110, FrameColor, 2.f, 26.f);
    Text(FString::Printf(TEXT("OPS NAV   |   HEAD %03d°   |   RIG %03.0f°"), Heading, Frame.Rig.HeadingDegrees), 1024, 92, 0.85f, WithAlpha(DimCyan, BootAlpha), true);
    Line(474, 195, 1574, 195, WithAlpha(DimCyan, BootAlpha), 2);
    // Compass labels scroll with DisplayedHeading. "N" marks the direction
    // selected at initial calibration or by X; no magnetic-north sensor is used.
    struct FCompassPoint { float Degrees; const TCHAR* Label; };
    static const FCompassPoint CompassPoints[] = {
        {0.f, TEXT("N")}, {45.f, TEXT("NE")}, {90.f, TEXT("E")}, {135.f, TEXT("SE")},
        {180.f, TEXT("S")}, {225.f, TEXT("SW")}, {270.f, TEXT("W")}, {315.f, TEXT("NW")},
    };
    constexpr float CompassPixelsPerDegree = 6.f;
    constexpr float TapeLeft = 474.f;
    constexpr float TapeRight = 1574.f;
    constexpr float TapeFadeMargin = 70.f; // crossfade zone at each end of the ribbon
    auto TapeX = [this](float Bearing)
    {
        return 1024.f + FMath::FindDeltaAngleDegrees(DisplayedHeading, Bearing) * CompassPixelsPerDegree;
    };
    auto TapeAlpha = [](float X)
    {
        return FMath::Clamp(FMath::Min(X - TapeLeft, TapeRight - X) / TapeFadeMargin, 0.f, 1.f);
    };
    // Every tick has a bearing, just like its label. Turning right moves
    // BOTH left beneath the stationary index, continuously through north.
    for (int32 Degrees = 0; Degrees < 360; Degrees += 5)
    {
        const float X = TapeX(static_cast<float>(Degrees));
        const float Alpha = TapeAlpha(X);
        if (Alpha <= KINDA_SMALL_NUMBER) { continue; }
        const bool bMajor = Degrees % 30 == 0;
        const float Top = bMajor ? 174.f : Degrees % 15 == 0 ? 180.f : 186.f;
        Line(X, 195, X, Top, WithAlpha(FrameColor, Alpha), bMajor ? 3.f : 1.5f);
    }
    // Stroke cardinal letters match the tape's numeric style and stay attached
    // to the same display bearing as their ticks.
    auto Cardinal = [Line](const TCHAR* Label, float X, const FLinearColor& Color)
    {
        const int32 Count = FCString::Strlen(Label);
        const float Left = X - (Count * 14.f - 4.f) * 0.5f;
        for (int32 Index = 0; Index < Count; ++Index)
        {
            const float L = Left + Index * 14.f;
            const float R = L + 10.f;
            constexpr float Top = 149.f, Middle = 158.f, Bottom = 167.f;
            switch (Label[Index])
            {
                case TEXT('N'):
                    Line(L, Bottom, L, Top, Color, 2.f); Line(L, Top, R, Bottom, Color, 2.f); Line(R, Bottom, R, Top, Color, 2.f); break;
                case TEXT('E'):
                    Line(L, Bottom, L, Top, Color, 2.f); Line(L, Top, R, Top, Color, 2.f); Line(L, Middle, R, Middle, Color, 2.f); Line(L, Bottom, R, Bottom, Color, 2.f); break;
                case TEXT('S'):
                    Line(R, Top, L, Top, Color, 2.f); Line(L, Top, L, Middle, Color, 2.f); Line(L, Middle, R, Middle, Color, 2.f); Line(R, Middle, R, Bottom, Color, 2.f); Line(R, Bottom, L, Bottom, Color, 2.f); break;
                case TEXT('W'):
                    Line(L, Top, L + 2.f, Bottom, Color, 2.f); Line(L + 2.f, Bottom, L + 5.f, Middle, Color, 2.f); Line(L + 5.f, Middle, R - 2.f, Bottom, Color, 2.f); Line(R - 2.f, Bottom, R, Top, Color, 2.f); break;
            }
        }
    };
    for (const FCompassPoint& Point : CompassPoints)
    {
        const float X = TapeX(Point.Degrees);
        const float EdgeFade = TapeAlpha(X);
        if (EdgeFade > KINDA_SMALL_NUMBER)
        {
            Cardinal(Point.Label, X, WithAlpha(Point.Degrees == 0.f ? FLinearColor::White : DimCyan, BootAlpha * EdgeFade));
        }
    }
    // Numeric tick labels every 30 deg between the cardinals, matching the
    // reference HUD's "105 120 ... 150 165 ..." look. Segment-digit, not
    // Text(), so these are actually visible right now. Skipped at the 4
    // cardinal ticks (0/90/180/270) since a letter already sits there.
    for (int32 Deg = 0; Deg < 360; Deg += 30)
    {
        if (Deg % 90 == 0) { continue; }
        const float X = TapeX(static_cast<float>(Deg));
        const float EdgeFade = TapeAlpha(X);
        if (EdgeFade > KINDA_SMALL_NUMBER)
        {
            SegNum(FString::Printf(TEXT("%03d"), Deg), X, 148, 11.f, WithAlpha(DimCyan, BootAlpha * EdgeFade * 0.85f), 1.6f, true);
        }
    }
    // Contact bearing pips on the ribbon (6 Sept, per the HUD critique/
    // mockup): previously the compass only ever showed heading, with
    // contacts visible exclusively on the tactical map below -- an operator
    // had to look down to know a contact was even nearby. Small triangle
    // pips, color-coded the same way the map's dots are (Contact.State,
    // hostile pulsing red like its map tag), let a bearing register at a
    // glance without moving focus off the forward view. Same HeadRelBearing
    // math as the tactical-map contact loop further down (kept local here
    // since Frame/DisplayedHeading/bLive are already in scope this early) --
    // deliberately a lightweight pip rather than the map's full IFF shape:
    // a hexagon/box distinction doesn't read at 12px, color does.
    if (bContentReady && bLive && !bSpatialMode)
    {
        for (const FWallhackContact& Contact : Frame.Contacts)
        {
            const float HeadRelBearing = FMath::Fmod(Contact.RelativeBearingDegrees + Frame.Rig.HeadingDegrees - DisplayedHeading + 720.f, 360.f);
            const float ContactDelta = HeadRelBearing > 180.f ? HeadRelBearing - 360.f : HeadRelBearing;
            const float PipX = 1024.f + ContactDelta * CompassPixelsPerDegree;
            const float PipEdgeFade = TapeAlpha(PipX);
            if (PipEdgeFade <= KINDA_SMALL_NUMBER) { continue; }
            const bool bHostilePip = Contact.Classification == EWallhackContactClassification::Hostile;
            const FLinearColor PipBaseColor = Contact.State == EWallhackContactState::Nominal ? Good : Contact.State == EWallhackContactState::Degraded ? Warning : Danger;
            const FLinearColor PipColor = bHostilePip ? Danger : PipBaseColor;
            const float PipAlpha = (bHostilePip ? (0.6f + 0.4f * FMath::Sin(ElapsedSeconds * 7.f)) : 1.f) * BootAlpha * PipEdgeFade;
            Line(PipX, 199.f, PipX - 5.f, 207.f, WithAlpha(PipColor, PipAlpha), 2.2f);
            Line(PipX, 199.f, PipX + 5.f, 207.f, WithAlpha(PipColor, PipAlpha), 2.2f);
            Line(PipX - 5.f, 207.f, PipX + 5.f, 207.f, WithAlpha(PipColor, PipAlpha), 2.2f);
            SegNum(FString::Printf(TEXT("%02d"), Contact.Id), PipX, 209.f, 7.f, WithAlpha(PipColor, PipAlpha), 1.2f, true);
        }
    }
    // Center pointer: a small downward chevron over a bracketed heading
    // readout, the way Apex/Battlefield mark "this is your current heading"
    // on their compass tape -- replaces the old plain "HEAD/RIG" number
    // stack with something that reads as a pointer, not just a label.
    Line(1014.f, 181.f, 1024.f, 193.f, WithAlpha(FLinearColor::White, BootAlpha), 2.5f);
    Line(1034.f, 181.f, 1024.f, 193.f, WithAlpha(FLinearColor::White, BootAlpha), 2.5f);
    GlowLine(1024, 193, 1024, 205, WithAlpha(FLinearColor::White, BootAlpha), 3);
    SegChar(TEXT('/'), 1024.f - 60.f, 108.f, 26.f, WithAlpha(Cyan, BootAlpha), 3.f);
    SegNum(FString::Printf(TEXT("%03d"), Heading), 1024.f, 108.f, 26.f, WithAlpha(Cyan, BootAlpha), 3.5f, true);
    SegChar(TEXT('/'), 1024.f + 46.f, 108.f, 26.f, WithAlpha(Cyan, BootAlpha), 3.f);
    // RIG heading kept as a smaller secondary readout beside the tactical
    // map's own "RIG" label further down -- see the SegNum call next to
    // that Text(TEXT("RIG"), ...) call below.

    // Compact link-state pill sits under the ribbon in every density so the
    // operator always has a one-glance read on the ground link.
    const FLinearColor LinkColor = LinkStateColor(Frame.LinkState);
    Text(LinkStateText(Frame.LinkState), 1024, 218, 0.85f, WithAlpha(LinkColor, BootAlpha), true);

    // IFF / hostile banner. Deliberately reads Contact.Classification, never
    // Contact.State -- a merely low-confidence contact must never be able to
    // paint this bright red. Until the ground bridge actually sends a
    // classification this simply never lights up (every contact defaults to
    // Unknown), which is the honest behaviour: no sensor for it yet, no alert.
    if (bContentReady && bLive && !bSpatialMode)
    {
        const FWallhackContact* NearestHostile = nullptr;
        for (const FWallhackContact& Contact : Frame.Contacts)
        {
            if (Contact.Classification == EWallhackContactClassification::Hostile &&
                (!NearestHostile || Contact.RangeMeters < NearestHostile->RangeMeters))
            {
                NearestHostile = &Contact;
            }
        }
        if (NearestHostile)
        {
            const float FlashAlpha = 0.5f + 0.5f * FMath::Sin(ElapsedSeconds * 6.f);
            const float FlashScale = 1.f + 0.05f * FMath::Sin(ElapsedSeconds * 6.f);
            Text(FString::Printf(TEXT("!! HOSTILE CONTACT %02d  /  BRG %03.0f  RNG %.0fm"), NearestHostile->Id, FMath::Fmod(NearestHostile->RelativeBearingDegrees + 360.f, 360.f), NearestHostile->RangeMeters), 1024, 240, 1.15f * FlashScale, WithAlpha(Danger, FlashAlpha), true);
        }
        else if (HUDDensity == EWallhackHUDDensity::Full)
        {
            Text(TEXT("IFF / NO HOSTILES CLASSIFIED"), 1024, 244, 0.72f, WithAlpha(DimCyan, 0.8f), true);
        }
    }

    if (bSpatialMode)
    {
        const FLinearColor TrackingColor = bSpatialTracked ? Good : Warning;
        Label(TEXT("LOCAL SIMULATION / PERSON 01"), 1024, 247, 18.f, Good, true);
        Label(WorldContact->GetTrackingStatus(), 1024, 276, 14.f, TrackingColor, true);

        // Stable room location, displayed in a heading-up operator map. No
        // compass calibration, sensor FOV mask, scan animation or auto zoom.
        constexpr float CX = 320.f, CY = 790.f, Radius = 149.f, MapRange = 5.f;
        Label(TEXT("LOCAL MAP / HEAD UP"), 160, 583, 16.f, Good);
        Label(TEXT("RADIUS 5 M"), 160, 611, 12.f, DimCyan);
        Circle(CX, CY, Radius, WithAlpha(Good, BootAlpha), 2.f);
        Circle(CX, CY, Radius * 0.5f, WithAlpha(DimCyan, BootAlpha * 0.45f), 1.f);
        Line(CX - Radius, CY, CX + Radius, CY, WithAlpha(DimCyan, 0.22f), 1.f);
        Line(CX, CY - Radius, CX, CY + Radius, WithAlpha(DimCyan, 0.22f), 1.f);
        Line(CX, CY - 10, CX - 7, CY + 6, FLinearColor::White, 2.f);
        Line(CX, CY - 10, CX + 7, CY + 6, FLinearColor::White, 2.f);
        Line(CX - 7, CY + 6, CX + 7, CY + 6, FLinearColor::White, 2.f);
        Label(TEXT("YOU"), CX + 14, CY + 8, 10.f, FLinearColor::White);

        if (bSpatialTracked)
        {
            const FVector2D Offset = WallhackSpatialMath::MapOffset(SpatialView, Radius, MapRange);
            const float X = CX + Offset.X, Y = CY + Offset.Y;
            const bool bOutsideMap = SpatialView.GroundRangeMeters > MapRange;
            Circle(X, Y, 7.f, Good, 2.5f);
            if (!bOutsideMap) Line(X - 3, Y, X + 3, Y, Good, 5.f);
            Label(TEXT("01"), X + 13, Y - 6, 12.f, Good);
            Label(FString::Printf(TEXT("RANGE %.2f M"), SpatialView.RangeMeters), 160, 961, 18.f, Good);
            Label(bOutsideMap ? TEXT("OUTSIDE 5 M / EDGE MARKER") : TEXT("FIXED WORLD POSITION"), 160, 992, 12.f, bOutsideMap ? Warning : DimCyan);

            // Same bearing as the map, registered to the heading tape. The
            // actual 3D marker is rendered by the scene, never on this layer.
            const float PipX = 1024.f + SpatialView.BearingDegrees * CompassPixelsPerDegree;
            const float Fade = TapeAlpha(PipX);
            if (Fade > KINDA_SMALL_NUMBER)
            {
                const FLinearColor PipColor = WithAlpha(Good, Fade);
                Line(PipX - 5, 207, PipX, 199, PipColor, 2.f);
                Line(PipX, 199, PipX + 5, 207, PipColor, 2.f);
                Label(TEXT("01"), PipX, 213, 10.f, PipColor, true);
            }
        }
        else
        {
            Label(TEXT("POSITION UNAVAILABLE"), 160, 961, 14.f, Warning);
            Label(IsDesktopTrackingPreview() ? TEXT("SPACE / PLACE WHEN TRACKED") : TEXT("A / PLACE WHEN TRACKED"), 160, 992, 12.f, DimCyan);
        }
        Label(IsDesktopTrackingPreview() ? TEXT("SPACE / PLACE 3 M AHEAD    B / HUD    X / COMPASS ZERO") :
            TEXT("A / PLACE 3 M AHEAD    B / HUD    X / COMPASS ZERO"), 1024, 1030, 13.f, DimCyan, true);

        if (HUDDensity == EWallhackHUDDensity::Full)
        {
            CornerFrame(1388, 620, 560, 420, FrameColor, 2.f, 28.f);
            Label(TEXT("SIMULATED PERSON / 01"), 1410, 649, 17.f, Good);
            Label(TEXT("STATIONARY TEST CONTACT"), 1410, 685, 12.f, DimCyan);
            // A schematic person, with the dot at the same reference point.
            const FLinearColor PersonColor = bSpatialTracked ? Good : WithAlpha(DimCyan, 0.25f);
            Circle(1480, 758, 17.f, PersonColor, 2.f);
            Line(1480, 775, 1480, 852, PersonColor, 3.f);
            Line(1436, 805, 1524, 805, PersonColor, 3.f);
            Line(1480, 852, 1450, 922, PersonColor, 3.f);
            Line(1480, 852, 1510, 922, PersonColor, 3.f);
            if (bSpatialTracked)
            {
                Label(FString::Printf(TEXT("RANGE %.2f M"), SpatialView.RangeMeters), 1570, 743, 18.f, Good);
                Label(FString::Printf(TEXT("BEARING %.1f DEG"), SpatialView.BearingDegrees), 1570, 785, 14.f, DimCyan);
                Label(FString::Printf(TEXT("HEIGHT %+.2f M"), SpatialView.HeightMeters), 1570, 825, 14.f, DimCyan);
                Label(FString::Printf(TEXT("GROUND %.2f M"), SpatialView.GroundRangeMeters), 1570, 865, 14.f, DimCyan);
                Label(TEXT("HEIGHT / RELATIVE TO EYES"), 1410, 955, 12.f, DimCyan);
                Label(IsDesktopTrackingPreview() ? TEXT("SOURCE / DESKTOP TEST") : TEXT("SOURCE / LOCAL ANCHOR"), 1410, 987, 12.f, Good);
            }
            else Label(TEXT("WAITING FOR TRACKING"), 1570, 785, 14.f, Warning);
        }
    }

    if (HUDDensity == EWallhackHUDDensity::Minimal)
    {
        return;
    }

    if (!bSpatialMode)
    {
    // Tactical map. Shrunk from the previous 220px radius (it was crowding
    // the visor) and, critically, no longer always a full circle: a 360 ring
    // implies an omnidirectional sensor, which most rigs/drone cameras are
    // not. It only draws full-circle when SensorFOVDegrees says the sensor
    // really is all-around; otherwise it draws the honest wedge the sensor
    // actually covers, forward-facing (up on the map = rig forward), with a
    // dim static hatch filling the blind zone so "no coverage there" reads as
    // deliberate, not as a bug.
    const UWallhackHUDSettings* Settings = GetDefault<UWallhackHUDSettings>();
    const float MapCX = 320.f, MapCY = 790.f, MapRadius = 165.f;
    const bool bOmniSensor = Settings->SensorFOVDegrees >= 359.f;
    constexpr float ForwardAngle = -UE_HALF_PI; // up on screen = rig forward
    const float HalfFOV = FMath::DegreesToRadians(Settings->SensorFOVDegrees * 0.5f);

    if (bOmniSensor)
    {
        Circle(MapCX, MapCY, MapRadius, FrameColor, 2.f);
        Circle(MapCX, MapCY, MapRadius * 0.66f, WithAlpha(DimCyan, BootAlpha * 0.5f), 1.f);
        Circle(MapCX, MapCY, MapRadius * 0.33f, WithAlpha(DimCyan, BootAlpha * 0.5f), 1.f);
    }
    else
    {
        // Blind-zone static: sparse short dashes filling the uncovered pie,
        // reading as sensor clutter/no-signal rather than empty space.
        for (float Angle = ForwardAngle + HalfFOV; Angle < ForwardAngle + 2.f * UE_PI - HalfFOV; Angle += FMath::DegreesToRadians(9.f))
        {
            const float Jitter = 0.55f + 0.35f * FMath::Sin(Angle * 13.7f + ElapsedSeconds * 0.6f);
            const FVector2D Inner(MapCX + MapRadius * 0.3f * FMath::Cos(Angle), MapCY + MapRadius * 0.3f * FMath::Sin(Angle));
            const FVector2D Outer(MapCX + MapRadius * Jitter * FMath::Cos(Angle), MapCY + MapRadius * Jitter * FMath::Sin(Angle));
            Canvas->K2_DrawLine(Inner, Outer, 1.f, WithAlpha(DimCyan, BootAlpha * 0.18f));
        }
        Arc(MapCX, MapCY, MapRadius, ForwardAngle - HalfFOV, ForwardAngle + HalfFOV, FrameColor, 2.5f);
        Arc(MapCX, MapCY, MapRadius * 0.66f, ForwardAngle - HalfFOV, ForwardAngle + HalfFOV, WithAlpha(DimCyan, BootAlpha * 0.6f), 1.f);
        Arc(MapCX, MapCY, MapRadius * 0.33f, ForwardAngle - HalfFOV, ForwardAngle + HalfFOV, WithAlpha(DimCyan, BootAlpha * 0.6f), 1.f);
        Line(MapCX, MapCY, MapCX + MapRadius * FMath::Cos(ForwardAngle - HalfFOV), MapCY + MapRadius * FMath::Sin(ForwardAngle - HalfFOV), WithAlpha(FrameColor, BootAlpha * 0.8f), 1.5f);
        Line(MapCX, MapCY, MapCX + MapRadius * FMath::Cos(ForwardAngle + HalfFOV), MapCY + MapRadius * FMath::Sin(ForwardAngle + HalfFOV), WithAlpha(FrameColor, BootAlpha * 0.8f), 1.5f);
    }

    if (bContentReady)
    {
        // Eagle Eye style (5 Sept, per Evan): Anduril's Eagle Eye shows a
        // static field-of-regard cone, not an animated mechanically-scanned
        // search radar -- the underlying sensor is continuous, so a sweep
        // line implying an intermittent scan cycle would be dishonest in
        // the same way the old always-360 ring was before the FOV-wedge
        // fix above. Replaces the old rotating/back-and-forth sweep with a
        // soft, slow "breathing" fill: still reads as alive, implies no
        // scan cycle that isn't real. Works for the omni case too (HalfFOV
        // is 180 deg there, so this just fills the whole circle).
        const float Breathe = 0.5f + 0.5f * FMath::Sin(ElapsedSeconds * 1.1f);
        for (int32 Ring = 1; Ring <= 5; ++Ring)
        {
            const float RingRadius = MapRadius * (static_cast<float>(Ring) / 5.f);
            const float RingAlpha = (0.12f + 0.10f * Breathe) * (1.f - static_cast<float>(Ring) / 6.f);
            Arc(MapCX, MapCY, RingRadius, ForwardAngle - HalfFOV, ForwardAngle + HalfFOV, WithAlpha(Good, RingAlpha), 3.f);
        }
    }
    Text(bOmniSensor ? TEXT("TACTICAL MAP / 360° COVERAGE") : FString::Printf(TEXT("TACTICAL MAP / %.0f° SENSOR FOV"), Settings->SensorFOVDegrees), MapCX - MapRadius, MapCY - MapRadius - 34, 0.9f, FrameColor);
    GlowLine(MapCX - 10, MapCY, MapCX + 10, MapCY, WithAlpha(FLinearColor::White, BootAlpha), 2.5f); GlowLine(MapCX, MapCY - 10, MapCX, MapCY + 10, WithAlpha(FLinearColor::White, BootAlpha), 2.5f);
    Text(TEXT("RIG"), MapCX + 14, MapCY - 26, 0.68f, WithAlpha(FLinearColor::White, BootAlpha));
    // Segment-digit RIG heading -- the drone's own compass heading, distinct
    // from the operator-relative readout on the top ribbon (see the center
    // pointer comment up there). Small/dim: reference info, not the primary
    // number the operator reads at a glance.
    SegNum(FString::Printf(TEXT("%03.0f"), Frame.Rig.HeadingDegrees), MapCX + 14, MapCY - 8, 10.f, WithAlpha(DimCyan, BootAlpha), 1.5f, false);

    if (bContentReady && bLive && !bSpatialMode)
    {
        // --- Target lock: the single most important live contact gets a
        // locking reticle instead of every contact reading as visually equal.
        // Priority is nearest hostile; if IFF hasn't classified anything yet
        // (still the common case -- see the banner above, which only lights
        // up once the ground bridge actually sends a classification), it
        // falls back to nearest contact overall, so the reticle is never
        // idle just because nothing has been tagged hostile. Purely a
        // rendering read on Frame.Contacts -- no new sensor, no new packet
        // field, nothing added to the telemetry subsystem.
        const FWallhackContact* LockTarget = nullptr;
        for (const FWallhackContact& Contact : Frame.Contacts)
        {
            // (LockTarget, once set in this loop, is always Hostile -- no
            // need to re-check its classification each iteration.)
            if (Contact.Classification == EWallhackContactClassification::Hostile &&
                (!LockTarget || Contact.RangeMeters < LockTarget->RangeMeters))
            {
                LockTarget = &Contact;
            }
        }
        if (!LockTarget)
        {
            for (const FWallhackContact& Contact : Frame.Contacts)
            {
                if (!LockTarget || Contact.RangeMeters < LockTarget->RangeMeters)
                {
                    LockTarget = &Contact;
                }
            }
        }
        const int32 NewLockedId = LockTarget ? LockTarget->Id : -1;
        if (NewLockedId != LockedContactId)
        {
            if (NewLockedId != -1)
            {
                PushEvent(FString::Printf(TEXT("TARGET LOCK / CONTACT %02d"), NewLockedId), Cyan);
            }
            else if (LockedContactId != -1)
            {
                PushEvent(TEXT("TARGET LOCK LOST"), Warning);
            }
            LockedContactId = NewLockedId;
            LockAcquiredAtSeconds = ElapsedSeconds;
        }
        const float TimeSinceLock = ElapsedSeconds - LockAcquiredAtSeconds;
        constexpr float LockAcquireSeconds = 0.6f;
        const bool bLockAcquiring = LockedContactId != -1 && TimeSinceLock < LockAcquireSeconds;

        // Halo-style radar plot, now head-relative (5 Sept, per Evan's Eagle
        // Eye direction): Contact.RelativeBearingDegrees is heading-corrected
        // upstream against the RIG's own heading (WallhackTelemetrySubsystem
        // subtracts Frame.Rig.HeadingDegrees when building it) -- 0 deg there
        // is dead ahead of the DRONE, not the operator. That was the right
        // frame for a rig-forward-locked map (the 5 Sept bearing fix), but
        // this map is now locked to the OPERATOR's own forward instead (map
        // "up" = wherever the operator is currently facing, same
        // DisplayedHeading the compass ribbon reads -- see the
        // NorthOffsetDegrees calibration above). So each contact's rig-
        // relative bearing gets undone back to an absolute/world bearing
        // (+ Frame.Rig.HeadingDegrees) and then re-expressed relative to the
        // operator's calibrated heading (- DisplayedHeading) instead. Turn
        // your head and the dots swing the other way around a fixed forward
        // cone, exactly like a real head-locked AR HUD -- a world-fixed
        // contact should appear to move opposite your own turn, not sit
        // still while the cone silently reinterprets what "ahead" means.
        float MaxRange = 3.f;
        for (const FWallhackContact& Contact : Frame.Contacts) MaxRange = FMath::Max(MaxRange, Contact.RangeMeters + 1.f);
        for (const FWallhackContact& Contact : Frame.Contacts)
        {
            const float HeadRelBearing = FMath::Fmod(Contact.RelativeBearingDegrees + Frame.Rig.HeadingDegrees - DisplayedHeading + 720.f, 360.f);
            const float NormalizedRange = FMath::Clamp(Contact.RangeMeters / MaxRange, 0.f, 1.f);
            const float PlotAngle = ForwardAngle + FMath::DegreesToRadians(HeadRelBearing);
            const float X = MapCX + FMath::Cos(PlotAngle) * NormalizedRange * (MapRadius - 24.f);
            const float Y = MapCY + FMath::Sin(PlotAngle) * NormalizedRange * (MapRadius - 24.f);
            const FLinearColor ContactColor = Contact.State == EWallhackContactState::Nominal ? Good : Contact.State == EWallhackContactState::Degraded ? Warning : Danger;
            const bool bHostile = Contact.Classification == EWallhackContactClassification::Hostile;

            // Sensor-fusion style entity marker (small diamond + expanding
            // ping ring + offset tag on a leader line) replaces the old
            // Titanfall/CoD-style 4-corner lock brackets -- those read as a
            // game reticle; this reads closer to a real fused-track display
            // (Anduril Lattice, most modern C2 UIs). The diamond still
            // breathes with confidence so a healthy contact reads as steady
            // and a degraded one as visibly still searching; the ping ring is
            // what keeps the whole map feeling alive even when nothing moves.
            const float PulseHz = Contact.State == EWallhackContactState::Critical ? 3.2f : Contact.State == EWallhackContactState::Degraded ? 2.f : 0.9f;
            const float Pulse = 1.f + 0.18f * FMath::Sin(ElapsedSeconds * PulseHz * 2.f * UE_PI);
            const float H = 6.5f * Pulse;
            // IFF now carries shape as well as color (6 Sept, per the HUD
            // critique/mockup) -- every contact used to draw as the same
            // diamond regardless of Classification, so color alone was doing
            // all the work, which fails under a monochrome or NVG-tinted
            // display mode. Roughly follows NATO APP-6: hostile stays a
            // diamond (matches the real standard already), friendly becomes
            // a box, neutral a ring, and unknown a hexagon standing in for
            // the standard's four-lobed quatrefoil, which doesn't read
            // cleanly at this size.
            switch (Contact.Classification)
            {
                case EWallhackContactClassification::Friendly:
                    Line(X - H, Y - H, X + H, Y - H, ContactColor, 2.f);
                    Line(X + H, Y - H, X + H, Y + H, ContactColor, 2.f);
                    Line(X + H, Y + H, X - H, Y + H, ContactColor, 2.f);
                    Line(X - H, Y + H, X - H, Y - H, ContactColor, 2.f);
                    break;
                case EWallhackContactClassification::Neutral:
                    Circle(X, Y, H, ContactColor, 2.f);
                    break;
                case EWallhackContactClassification::Unknown:
                {
                    FVector2D HexPrev(X + H, Y);
                    for (int32 HexIndex = 1; HexIndex <= 6; ++HexIndex)
                    {
                        const float HexAngle = HexIndex * UE_PI / 3.f;
                        const FVector2D HexNext(X + H * FMath::Cos(HexAngle), Y + H * FMath::Sin(HexAngle));
                        Line(HexPrev.X, HexPrev.Y, HexNext.X, HexNext.Y, ContactColor, 2.f);
                        HexPrev = HexNext;
                    }
                    break;
                }
                case EWallhackContactClassification::Hostile:
                default:
                    Line(X, Y - H, X + H, Y, ContactColor, 2.f); Line(X + H, Y, X, Y + H, ContactColor, 2.f);
                    Line(X, Y + H, X - H, Y, ContactColor, 2.f); Line(X - H, Y, X, Y - H, ContactColor, 2.f);
                    break;
            }

            constexpr float PingPeriod = 2.2f;
            const float PingPhase = FMath::Frac((ElapsedSeconds + Contact.Id * 0.37f) / PingPeriod);
            Circle(X, Y, FMath::Lerp(H + 2.f, 24.f, PingPhase), WithAlpha(ContactColor, (1.f - PingPhase) * 0.45f), 1.5f);

            const FLinearColor TagColor = bHostile ? Danger : ContactColor;
            const float TagAlpha = bHostile ? (0.6f + 0.4f * FMath::Sin(ElapsedSeconds * 7.f)) : 1.f;
            const FVector2D LeaderStart(X + H * 0.75f, Y - H * 0.75f);
            const FVector2D LeaderElbow(X + 20.f, Y - 20.f);
            Line(LeaderStart.X, LeaderStart.Y, LeaderElbow.X, LeaderElbow.Y, WithAlpha(TagColor, TagAlpha), 1.f);
            Line(LeaderElbow.X, LeaderElbow.Y, LeaderElbow.X + 12.f, LeaderElbow.Y, WithAlpha(TagColor, TagAlpha), 1.f);
            // FString::Printf's format string must be a literal at each call site (its
            // compile-time format checker rejects a ternary between two TEXT() literals,
            // even though both branches are valid) -- so this picks the whole call, not
            // just the format string.
            const FString ContactTagText = bHostile
                ? FString::Printf(TEXT("!%02d %.0f%%"), Contact.Id, Contact.Confidence * 100.f)
                : FString::Printf(TEXT("%02d %.0f%%"), Contact.Id, Contact.Confidence * 100.f);
            Text(ContactTagText, LeaderElbow.X + 14.f, LeaderElbow.Y - 10.f, 0.68f, WithAlpha(TagColor, TagAlpha));

            // Second tag line: bearing/range at a glance. The lock system
            // (below) gives a sustained detail readout for one contact at a
            // time; this is for triage -- scanning the whole map without
            // having to lock each contact just to see how far it is. Can
            // read as crowded with many close-together contacts at once;
            // no attempt made to de-clutter beyond that for this event.
            // Same HeadRelBearing used to plot this dot -- printed bearing
            // must agree with the pixel position, not the rig-relative raw
            // value, or the number and the dot would visibly disagree the
            // moment the operator's head isn't pointed the same way as the rig.
            const FString ContactRangeText = FString::Printf(TEXT("%03.0f° %.1fm"), HeadRelBearing, Contact.RangeMeters);
            Text(ContactRangeText, LeaderElbow.X + 14.f, LeaderElbow.Y + 6.f, 0.58f, WithAlpha(TagColor, TagAlpha * 0.85f));
            // Segment-digit fallback (see SegNum comment near the top of this
            // function) -- this is the one number that actually matters for
            // eyeballing the bearing fix without a real bridge connected, so
            // it uses the same numeric style as the heading and range readouts.
            SegNum(ContactRangeText, LeaderElbow.X + 14.f, LeaderElbow.Y + 10.f, 14.f, WithAlpha(TagColor, TagAlpha * 0.85f), 2.f, false);

            if (Contact.Id == LockedContactId)
            {
                // Wide-to-tight snap over LockAcquireSeconds -- the "target
                // acquired" idiom nearly every FPS/flight-sim HUD uses.
                // Reuses the same CornerFrame bezel primitive as every other
                // panel, just centered on a contact and animated instead of
                // static.
                const float AcquireT = FMath::Clamp(TimeSinceLock / LockAcquireSeconds, 0.f, 1.f);
                // Ease-out cubic instead of a linear lerp: the bracket snaps
                // in fast and settles gently, rather than closing at a
                // constant speed the whole way -- the same "designed motion"
                // cue as a camera-shutter or FPS scope-in animation.
                const float AcquireEased = 1.f - FMath::Pow(1.f - AcquireT, 3.f);
                const float HalfSize = FMath::Lerp(34.f, 16.f, AcquireEased);
                const FLinearColor LockColor = bHostile ? Danger : Cyan;
                const float LockAlpha = bLockAcquiring ? (0.55f + 0.45f * FMath::Sin(ElapsedSeconds * 14.f)) : 1.f;
                CornerFrame(X - HalfSize, Y - HalfSize, HalfSize * 2.f, HalfSize * 2.f, WithAlpha(LockColor, LockAlpha), 2.5f, HalfSize * 0.55f);
            }
        }
        Text(FString::Printf(TEXT("CONTACTS %02d / LIVE FUSED POSITIONS"), Frame.Contacts.Num()), MapCX - MapRadius, MapCY + MapRadius + 16, 0.82f, Good);
        if (LockedContactId != -1 && LockTarget)
        {
            const FLinearColor LockColor = LockTarget->Classification == EWallhackContactClassification::Hostile ? Danger : Cyan;
            const FString LockStatus = bLockAcquiring
                ? FString::Printf(TEXT("LOCK ACQUIRING / CONTACT %02d"), LockedContactId)
                : FString::Printf(TEXT("LOCK / CONTACT %02d  BRG %03.0f  RNG %.0fm"), LockedContactId, FMath::Fmod(LockTarget->RelativeBearingDegrees + 360.f, 360.f), LockTarget->RangeMeters);
            Text(LockStatus, MapCX - MapRadius, MapCY + MapRadius + 38, 0.78f, WithAlpha(LockColor, bLockAcquiring ? (0.6f + 0.4f * FMath::Sin(ElapsedSeconds * 10.f)) : 1.f));
        }
    }
    else if (bContentReady)
    {
        Text(TEXT("AWAITING FRESH PI BRIDGE DATA"), MapCX - MapRadius, MapCY + MapRadius + 16, 0.72f, Danger);
    }

    // Former "drone video" placeholder -- there was never a real video source
    // (still true), and an empty box reading "NO VIDEO ENDPOINT CONFIGURED"
    // was the single largest dead zone on the whole HUD. Repurposed into a
    // target-detail readout for whatever the lock system (above) currently
    // has locked -- reads LockedContactId/LockAcquiredAtSeconds, both
    // already computed in the tactical-map block above; no new sensor, no
    // new packet field. Still honest when nothing is locked: a quiet
    // "NO ACTIVE LOCK" instead of pretending a feed exists.
    // Grown 340 -> 420px tall (6 Sept) to make room for the NLOS stick-figure
    // read added below -- still comfortably inside the peripheral frame's
    // bottom margin (frame's inner edge is ~962; this panel now bottoms out
    // at 620+420=1040, which only grazes the bottom-right corner accent, not
    // the ticker centered at x=1024).
    CornerFrame(1388, 620, 560, 420, FrameColor, 2.f, 28.f);
    Text(TEXT("TARGET DETAIL"), 1410, 642, 1.0f, FrameColor);
    if (bContentReady)
    {
        const FWallhackContact* DetailTarget = nullptr;
        if (bLive)
        {
            for (const FWallhackContact& Contact : Frame.Contacts)
            {
                if (Contact.Id == LockedContactId)
                {
                    DetailTarget = &Contact;
                    break;
                }
            }
        }
        if (DetailTarget)
        {
            const bool bHostileDetail = DetailTarget->Classification == EWallhackContactClassification::Hostile;
            const FLinearColor DetailColor = bHostileDetail ? Danger
                : DetailTarget->State == EWallhackContactState::Nominal ? Good
                : DetailTarget->State == EWallhackContactState::Degraded ? Warning : Danger;
            const FString ClassText = DetailTarget->Classification == EWallhackContactClassification::Hostile ? TEXT("HOSTILE")
                : DetailTarget->Classification == EWallhackContactClassification::Friendly ? TEXT("FRIENDLY")
                : DetailTarget->Classification == EWallhackContactClassification::Neutral ? TEXT("NEUTRAL") : TEXT("UNCLASSIFIED");
            const FString StateText = DetailTarget->State == EWallhackContactState::Nominal ? TEXT("NOMINAL")
                : DetailTarget->State == EWallhackContactState::Degraded ? TEXT("DEGRADED") : TEXT("CRITICAL");
            const float DetailPulse = 0.9f + 0.1f * FMath::Sin(ElapsedSeconds * 6.f);
            Text(FString::Printf(TEXT("CONTACT %02d  /  %s"), DetailTarget->Id, *ClassText), 1410, 700, 1.1f, WithAlpha(DetailColor, DetailPulse));

            // NLOS / through-wall stick figure (6 Sept, per Evan -- this is
            // the actual "wallhack" part of Wallhack). Drawn with Line()/
            // Circle() only, same as every other primitive in this file, not
            // Text() -- consistent with the rest of the HUD's "don't depend
            // on the still-broken glyph pipeline for anything that matters"
            // rule (see the HardRefFont comment up in the constructor).
            //
            // Deliberately gated on Posture != Unknown: a through-wall sensor
            // that hasn't actually classified a pose has no business showing
            // a confident stick figure -- same "never imply a capability
            // that isn't there" rule as the sensor-FOV wedge and the IFF
            // banner elsewhere in this file. No real bridge sends posture
            // yet (see ParsePacket), so this only ever lights up against the
            // desktop simulator today; that's correct, not a bug.
            constexpr float FigCX = 1470.f;
            constexpr float FigFeetY = 940.f;
            if (DetailTarget->Posture != EWallhackContactPosture::Unknown)
            {
                // Bone coordinates are lifted directly from the browser
                // mockup's stick-figure SVGs (x centered on 35, y measured
                // down from that pose's own top) so the in-headset figure
                // matches what was already reviewed and approved as a design
                // -- Scale/FigTopY below just place that same shape here.
                const bool bProne = DetailTarget->Posture == EWallhackContactPosture::Prone;
                const bool bCrouched = DetailTarget->Posture == EWallhackContactPosture::Crouched;
                const float Scale = 1.5f;
                const float PoseHeight = bProne ? 34.f : bCrouched ? 120.f : 150.f;
                const float FigTopY = FigFeetY - PoseHeight * Scale;
                auto Bone = [&](float X1, float Y1, float X2, float Y2)
                {
                    Line(FigCX + (X1 - 35.f) * Scale, FigTopY + Y1 * Scale, FigCX + (X2 - 35.f) * Scale, FigTopY + Y2 * Scale, DetailColor, 2.5f);
                };
                auto Joint = [&](float JX, float JY, float R)
                {
                    Circle(FigCX + (JX - 35.f) * Scale, FigTopY + JY * Scale, R * Scale, DetailColor, 2.f);
                };
                if (bProne)
                {
                    // Lying figure, drawn head-to-feet along X instead of Y --
                    // the only pose where "up" on the panel isn't "up" on
                    // the subject.
                    Joint(10.f, 20.f, 8.f);
                    Bone(18.f, 20.f, 70.f, 22.f);
                    Bone(24.f, 20.f, 24.f, 10.f); Bone(24.f, 20.f, 24.f, 30.f);
                    Bone(70.f, 22.f, 80.f, 20.f); Bone(70.f, 22.f, 80.f, 24.f);
                    Bone(80.f, 20.f, 120.f, 16.f); Bone(80.f, 24.f, 120.f, 28.f);
                    Bone(120.f, 16.f, 130.f, 14.f); Bone(120.f, 28.f, 130.f, 30.f);
                }
                else if (bCrouched)
                {
                    Joint(35.f, 14.f, 8.f);
                    Bone(35.f, 22.f, 35.f, 46.f);
                    Bone(35.f, 28.f, 18.f, 30.f); Bone(35.f, 28.f, 52.f, 30.f);
                    Bone(18.f, 30.f, 10.f, 48.f); Bone(52.f, 30.f, 60.f, 48.f);
                    Bone(35.f, 46.f, 24.f, 58.f); Bone(35.f, 46.f, 46.f, 58.f);
                    Bone(24.f, 58.f, 16.f, 78.f); Bone(46.f, 58.f, 54.f, 78.f);
                    Bone(24.f, 58.f, 20.f, 96.f); Bone(46.f, 58.f, 50.f, 96.f);
                }
                else
                {
                    Joint(35.f, 14.f, 9.f);
                    Bone(35.f, 24.f, 35.f, 70.f);
                    Bone(35.f, 34.f, 18.f, 36.f); Bone(35.f, 34.f, 52.f, 36.f);
                    Bone(18.f, 36.f, 12.f, 58.f); Bone(52.f, 36.f, 58.f, 58.f);
                    Bone(35.f, 70.f, 24.f, 74.f); Bone(35.f, 70.f, 46.f, 74.f);
                    Bone(24.f, 74.f, 20.f, 110.f); Bone(46.f, 74.f, 50.f, 110.f);
                    Bone(20.f, 110.f, 18.f, 140.f); Bone(50.f, 110.f, 52.f, 140.f);
                }

                // Vitals: a live breathing waveform once actually locked, an
                // honest "ACQUIRING" pulse otherwise -- never a flat line
                // pretending to be a real reading. Sits under the figure's
                // feet, inside the same 560-wide panel.
                const float VitalsY = FigFeetY + 20.f;
                if (DetailTarget->bVitalsLocked)
                {
                    FVector2D PrevPt(FigCX - 60.f, VitalsY);
                    for (int32 Sample = 1; Sample <= 30; ++Sample)
                    {
                        const float SX = FigCX - 60.f + Sample * 4.f;
                        const float SY = VitalsY + FMath::Sin(ElapsedSeconds * 2.2f + Sample * 0.5f) * 6.f;
                        Line(PrevPt.X, PrevPt.Y, SX, SY, Good, 1.6f);
                        PrevPt = FVector2D(SX, SY);
                    }
                    // SegNum only supports digits/./-/:/deg (see its comment
                    // up top) -- "BPM" would render as blank space through
                    // it, so the unit stays a Text() label instead, same as
                    // every other unit suffix in this file.
                    SegNum(FString::Printf(TEXT("%.0f"), DetailTarget->BreathingRateBpm), FigCX - 60.f, VitalsY + 16.f, 10.f, WithAlpha(Good, 0.85f), 1.6f, false);
                    Text(TEXT("BPM"), FigCX - 20.f, VitalsY + 18.f, 0.55f, WithAlpha(Good, 0.85f));
                }
                else
                {
                    const float AcquirePulse = 0.5f + 0.5f * FMath::Sin(ElapsedSeconds * 5.f);
                    Text(TEXT("VITALS ACQUIRING"), FigCX, VitalsY, 0.62f, WithAlpha(Warning, AcquirePulse), true);
                }
            }

            const FString PostureText = DetailTarget->Posture == EWallhackContactPosture::Standing ? TEXT("STANDING")
                : DetailTarget->Posture == EWallhackContactPosture::Crouched ? TEXT("CROUCHED")
                : DetailTarget->Posture == EWallhackContactPosture::Prone ? TEXT("PRONE") : TEXT("NO POSTURE DATA");

            // Stat column sits to the right of the figure, narrower than the
            // old full-width lines (0.9 -> 0.78 scale) so it fits beside the
            // figure instead of running the panel's full 560px.
            Text(FString::Printf(TEXT("TRACK / %s"), *StateText), 1620, 745, 0.78f, DimCyan);
            Text(FString::Printf(TEXT("CONF %.0f%%"), DetailTarget->Confidence * 100.f), 1620, 778, 0.78f, DimCyan);
            Text(FString::Printf(TEXT("BRG %03.0f°"), FMath::Fmod(DetailTarget->RelativeBearingDegrees + 360.f, 360.f)), 1620, 811, 0.78f, DimCyan);
            Text(FString::Printf(TEXT("RNG %.1fm"), DetailTarget->RangeMeters), 1620, 844, 0.78f, DimCyan);
            Text(FString::Printf(TEXT("LOCK %.1fs"), FMath::Max(0.f, ElapsedSeconds - LockAcquiredAtSeconds)), 1620, 877, 0.78f, DimCyan);
            Text(FString::Printf(TEXT("POSTURE %s"), *PostureText), 1620, 910, 0.78f, DetailTarget->Posture == EWallhackContactPosture::Unknown ? DimCyan : DetailColor);
        }
        else
        {
            CornerFrame(1415, 720, 480, 200, DimCyan, 1.5f, 18.f);
            Text(TEXT("NO ACTIVE LOCK"), 1655, 800, 1.05f, DimCyan, true);
            Text(TEXT("AWAITING A CONTACT TO TRACK"), 1655, 855, 0.76f, DimCyan, true);
        }
    }

    }

    // Live headset/system telemetry. This box previously sat hard against
    // the canvas' top-right corner, which put it right at the edge of (or
    // past) the visor arc -- it was rendering every frame, just outside
    // what the operator could comfortably see. Pulled inward to sit
    // squarely within the tightened arc.
    CornerFrame(1388, 230, 560, 350, FrameColor, 2.f, 28.f);
    Text(TEXT("SYSTEM TELEMETRY"), 1410, 252, 1.1f, FrameColor);
    if (bContentReady)
    {
        // Hierarchy pass (6 Sept, per Evan's HUD-critique mockup): every line
        // in this panel used to read at nearly the same size/weight, so
        // nothing told the eye what to check first. GROUND LINK is the one
        // fact that actually changes what an operator does next (trust the
        // map or don't), so it's now the loudest thing in the panel --
        // roughly 1.7x the old scale, with a thin divider separating it and
        // packet age/rig (still primary, operational numbers) from the
        // system-health tier below (battery/network/clock/FPS), which steps
        // down in both scale and brightness instead of matching the top.
        //
        // Alerts pulse in brightness instead of sitting static -- attention
        // grabbing without a jarring hard blink (borrowed from most tactical
        // shooter low-health/low-ammo indicators).
        const float AlertPulse = bLive ? 1.f : 0.55f + 0.45f * FMath::Sin(ElapsedSeconds * 4.f);
        Text(LinkStateText(Frame.LinkState), 1410, 296, 1.7f, WithAlpha(LinkColor, AlertPulse));
        Text(Frame.PacketAgeSeconds == TNumericLimits<float>::Max() ? TEXT("PACKET AGE / NO PACKETS") : FString::Printf(TEXT("PACKET AGE / %.0f ms"), Frame.PacketAgeSeconds * 1000.f), 1410, 358, 0.85f, DimCyan);
        Text(FString::Printf(TEXT("RIG / X %.2f  Y %.2f  HDG %03.0f°"), Frame.Rig.X, Frame.Rig.Y, Frame.Rig.HeadingDegrees), 1410, 396, 0.8f, DimCyan);

        Line(1410, 430, 1908, 430, WithAlpha(DimCyan, 0.4f), 1.f);

        const int32 Battery = FPlatformMisc::GetBatteryLevel();
        const bool bLowBattery = Battery >= 0 && Battery < 20;
        const float BatteryPulse = bLowBattery ? 0.55f + 0.45f * FMath::Sin(ElapsedSeconds * 4.f) : 1.f;
        Text(Battery >= 0 ? FString::Printf(TEXT("HEADSET BATTERY / %d%%"), Battery) : TEXT("HEADSET BATTERY / UNAVAILABLE"), 1410, 448, 0.76f, WithAlpha(bLowBattery ? Danger : DimCyan, bLowBattery ? BatteryPulse : 0.85f));
        const ENetworkConnectionType Network = FPlatformMisc::GetNetworkConnectionType();
        Text(Network == ENetworkConnectionType::WiFi ? TEXT("HEADSET NETWORK / WIFI") : Network == ENetworkConnectionType::Ethernet ? TEXT("HEADSET NETWORK / ETHERNET") : TEXT("HEADSET NETWORK / UNKNOWN"), 1410, 484, 0.76f, WithAlpha(DimCyan, 0.85f));
        Text(FString::Printf(TEXT("LOCAL %s"), *FDateTime::Now().ToString(TEXT("%H:%M:%S"))), 1410, 520, 0.76f, WithAlpha(FLinearColor::White, 0.85f));

        // Render load -- one more real read on headset health beyond battery.
        const bool bFPSLow = SmoothedFPS > 1.f && SmoothedFPS < 60.f;
        Text(FString::Printf(TEXT("RENDER RATE / %.0f FPS"), SmoothedFPS), 1410, 556, 0.76f, WithAlpha(bFPSLow ? Warning : DimCyan, bFPSLow ? 1.f : 0.85f));
    }

    // Comms/event ticker: recent state changes fade out over a few seconds
    // bottom-centre, so the operator gets peripheral situational awareness
    // (Destiny's public-event banner, Apex Legends' kill feed) instead of
    // having to keep re-reading the static telemetry block to notice a
    // change actually happened.
    if (bContentReady)
    {
        float TickerY = 1040.f;
        for (const FWallhackHUDEvent& Event : EventLog)
        {
            const float Age = ElapsedSeconds - Event.SpawnSeconds;
            if (Age > EventLifetimeSeconds) continue;
            const float FadeAlpha = 1.f - FMath::Clamp(Age / EventLifetimeSeconds, 0.f, 1.f);
            Text(Event.Text, 1024, TickerY, 0.85f, WithAlpha(Event.Color, FadeAlpha), true);
            TickerY -= 26.f;
        }
    }

    // Wrist-raise comms panel (5 Sept, per Evan): "raise your wrist to your
    // face" is the same gesture every smartwatch UI uses, so it reads as
    // familiar rather than needing to be taught. LeftHandController is a
    // physical-left-controller tracker (see header); comparing its distance
    // to the HMD is a cheap, hardware-agnostic stand-in for "did the operator
    // just bring their hand up to look at it" without needing any bone/hand
    // tracking. FInterpTo (not a hard cut) so the panel fades in/out instead
    // of popping, matching every other alpha transition in this HUD.
    // Push-to-talk is wired to StartTransmit()/StopTransmit() on the left
    // trigger (see header comment) -- UI/button-state only for now, no real
    // mic capture or audio channel to the drone exists yet.
    const bool bWristTracked = bHasHmdPose && LeftHandController && LeftHandController->IsTracked();
    const float WristDistance = bWristTracked ? FVector::Dist(LeftHandController->GetComponentLocation(), HmdPosition) : TNumericLimits<float>::Max();
    // 40cm was a guess made without hardware in hand, and the panel gave no
    // feedback at all when it missed -- "doesn't show" was indistinguishable
    // from "broken" versus "just barely out of range". Loosened to 60cm
    // (a raised forearm measures further from the headset than it feels,
    // since the controller sits well past the wrist) and, more importantly,
    // this distance is now ALWAYS printed via SegNum (bottom-left, dim, small)
    // regardless of whether the panel is showing -- so the actual live number
    // is visible in-headset and the threshold can be tuned from real data
    // instead of another guess.
    const bool bWristRaised = WristDistance < 60.f;
    WristPanelAlpha = FMath::FInterpTo(WristPanelAlpha, bWristRaised ? 1.f : 0.f, DeltaSeconds, 8.f);
    if (bContentReady)
    {
        const bool bNoTrackedHand = WristDistance >= TNumericLimits<float>::Max() * 0.5f;
        const FString WristDistText = bNoTrackedHand ? TEXT("---") : FString::Printf(TEXT("%03.0f"), WristDistance);
        SegNum(WristDistText, 130.f, 1060.f, 13.f, WithAlpha(bWristRaised ? Good : DimCyan, 0.85f), 1.8f, false);
    }
    if (WristPanelAlpha > 0.01f)
    {
        const float PanelAlpha = WristPanelAlpha * BootAlpha;
        constexpr float PX = 220.f, PY = 900.f; // lower-left of canvas -- where a raised left wrist actually sits in view
        CornerFrame(PX - 90.f, PY - 90.f, 180.f, 180.f, WithAlpha(Cyan, PanelAlpha), 2.f, 20.f);
        const FLinearColor MicColor = bIsTransmitting ? Danger : Cyan;
        const float MicPulse = bIsTransmitting ? (0.7f + 0.3f * FMath::Sin(ElapsedSeconds * 10.f)) : 1.f;
        // Simple mic glyph built from the same primitives as the rest of the
        // HUD (Circle/Line) rather than Text/SegNum, since it's an icon, not
        // a number -- capsule head as two circles, stand as a line, base as
        // a line, same as any stylized mic icon.
        Circle(PX, PY - 20.f, 22.f, WithAlpha(MicColor, PanelAlpha * MicPulse), 3.f);
        Circle(PX, PY - 20.f, 14.f, WithAlpha(MicColor, PanelAlpha * MicPulse * 0.7f), 2.f);
        Line(PX, PY + 2.f, PX, PY + 26.f, WithAlpha(MicColor, PanelAlpha), 3.f);
        Line(PX - 16.f, PY + 26.f, PX + 16.f, PY + 26.f, WithAlpha(MicColor, PanelAlpha), 3.f);
        if (bIsTransmitting)
        {
            // Outward-pulsing ring while actually transmitting -- the same
            // "live" cue as a recording indicator, so it's obvious at a
            // glance whether the trigger press registered.
            Circle(PX, PY - 20.f, 34.f + 6.f * FMath::Sin(ElapsedSeconds * 6.f), WithAlpha(Danger, PanelAlpha * 0.3f), 1.5f);
        }
        Text(TEXT("COMMS / HOLD TRIGGER"), PX, PY + 50.f, 0.6f, WithAlpha(DimCyan, PanelAlpha), true);
        // "ON AIR"-style status dash under the mic: a filled dash while
        // transmitting, a dim outline dash otherwise -- readable even while
        // the Text() glyph bug is unresolved, since it's Line()-drawn like
        // the rest of the segment-digit system, not a font glyph.
        Line(PX - 14.f, PY - 60.f, PX + 14.f, PY - 60.f, WithAlpha(MicColor, PanelAlpha), bIsTransmitting ? 4.f : 1.5f);
    }

}
