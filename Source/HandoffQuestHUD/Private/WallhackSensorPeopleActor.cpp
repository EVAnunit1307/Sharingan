#include "WallhackSensorPeopleActor.h"
#include "WallhackAnchorLifetime.h"
#include "WallhackPeopleRenderer.h"
#include "WallhackTelemetrySubsystem.h"
#include "WallhackVRPawn.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "IXRTrackingSystem.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/CoreDelegates.h"
#include "UObject/ConstructorHelpers.h"

AWallhackSensorPeopleActor::AWallhackSensorPeopleActor()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickGroup = TG_PostUpdateWork;
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("SensorReference")));
    AimMarker = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("FloorCursor"));
    AimMarker->SetupAttachment(GetRootComponent());
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
    AimMarker->SetStaticMesh(Sphere.Object);
    AimMarker->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    AimMarker->SetCastShadow(false);
    AimMarker->SetHiddenInGame(true);
}

void AWallhackSensorPeopleActor::BeginPlay()
{
    Super::BeginPlay();
#if !PLATFORM_ANDROID
    bPreview = FParse::Param(FCommandLine::Get(), TEXT("WallhackSensorPeoplePreview"));
#endif
    Renderer = GetWorld()->SpawnActor<AWallhackPeopleRenderer>();
    BackgroundHandle = FCoreDelegates::ApplicationWillEnterBackgroundDelegate.AddUObject(this, &AWallhackSensorPeopleActor::Suspend);
    ForegroundHandle = FCoreDelegates::ApplicationHasEnteredForegroundDelegate.AddUObject(this, &AWallhackSensorPeopleActor::Resume);
    RecenterHandle = FCoreDelegates::VRHeadsetRecenter.AddUObject(this, &AWallhackSensorPeopleActor::ResetPlacement);
}

bool AWallhackSensorPeopleActor::ViewerPose(FVector& Position, FQuat& Orientation) const
{
    if (bSuspended || bEnding) return false;
    if (!bPreview)
    {
        if (!GEngine || !GEngine->XRSystem.IsValid()) return false;
        const auto XR = GEngine->XRSystem;
        if (!XR->IsHeadTrackingAllowedForWorld(*GetWorld()) || !XR->IsTracking(IXRTrackingSystem::HMDDeviceId)
            || !XR->HasValidTrackingPosition()) return false;
    }
    const auto* PC = GetWorld()->GetFirstPlayerController();
    const APlayerCameraManager* Camera = PC ? PC->PlayerCameraManager.Get() : nullptr;
    if (!Camera) return false;
    Position = Camera->GetCameraLocation(); Orientation = Camera->GetCameraRotation().Quaternion();
    return !Position.ContainsNaN() && !Orientation.ContainsNaN();
}

bool AWallhackSensorPeopleActor::AimFloor(FVector& Point) const
{
    FVector Viewer; FQuat Orientation;
    if (!ViewerPose(Viewer, Orientation)) return false;
    const auto* PC = GetWorld()->GetFirstPlayerController();
    const auto* Pawn = PC ? Cast<AWallhackVRPawn>(PC->GetPawn()) : nullptr;
    FVector RayOrigin, Direction;
    if (!Pawn || !Pawn->GetNavigationAim(RayOrigin, Direction)) return false;
    float FloorZ = 0;
    if (!bPreview && GEngine && GEngine->XRSystem.IsValid()) FloorZ = GEngine->XRSystem->GetTrackingToWorldTransform().GetLocation().Z;
    const float Scale = GetWorld()->GetWorldSettings()->WorldToMeters;
    return Scale > 0 && WallhackSensorPeopleMath::FloorAim(RayOrigin, Direction, FloorZ, Point)
        && FVector::Distance(RayOrigin, Point) <= 20 * Scale;
}

void AWallhackSensorPeopleActor::ConfirmPlacement()
{
    if (bPending || bReady || bHidden || RegistrationKey.IsEmpty()) return;
    FVector Point;
    if (!AimFloor(Point)) { Status = TEXT("POINT AT THE FLOOR"); return; }
    if (PlacementStep == 0)
    {
        Origin = Point; PlacementStep = 1;
        Status = TEXT("MARK FORWARD / AT LEAST 0.5 M FROM ORIGIN");
        return;
    }
    FVector Direction = Point - Origin; Direction.Z = 0;
    if (Direction.Size() < .5 * GetWorld()->GetWorldSettings()->WorldToMeters)
    { Status = TEXT("FORWARD POINT MUST BE AT LEAST 0.5 M AWAY"); return; }
    Forward = Direction.GetSafeNormal();
    CreateReference();
}

void AWallhackSensorPeopleActor::CreateReference()
{
    ReleaseReference();
    ++Generation;
    SetActorLocationAndRotation(Origin, Forward.Rotation());
    if (bPreview) { bReady = true; Status = TEXT("DESKTOP REGISTRATION / NOT HEADSET VALIDATION"); return; }
#if PLATFORM_ANDROID
    bPending = true; RequestedAt = FPlatformTime::Seconds(); Status = TEXT("CREATING SENSOR ANCHOR");
    const uint32 Expected = Generation;
    const TWeakObjectPtr<AWallhackSensorPeopleActor> WeakThis(this);
    auto Callback = FOculusXRSpatialAnchorCreateDelegate::CreateLambda(
        [WeakThis, Expected](EOculusXRAnchorResult::Type Result, UOculusXRAnchorComponent* Created)
        {
            auto* Self = WeakThis.Get();
            if (!Self || Self->bEnding || Expected != Self->Generation) { DestroyWallhackAnchor(Created); return; }
            Self->bPending = false;
            if (!UOculusXRAnchorBPFunctionLibrary::IsAnchorResultSuccess(Result) || !IsValid(Created) || !Created->HasValidHandle())
            { DestroyWallhackAnchor(Created); Self->Status = TEXT("ANCHOR FAILED / A TO RETRY"); return; }
            Self->Anchor = Created;
            Self->AddTickPrerequisiteComponent(Created);
            Self->bReady = true;
            Self->Status = TEXT("LOCALIZING SENSOR ANCHOR");
        });
    EOculusXRAnchorResult::Type Result = EOculusXRAnchorResult::Failure;
    if (!OculusXRAnchors::FOculusXRAnchors::CreateSpatialAnchor(GetActorTransform(), this, Callback, Result))
    { bPending = false; Status = TEXT("ANCHOR FAILED / A TO RETRY"); }
#else
    Status = TEXT("ANCHOR REQUIRES QUEST / USE EXPLICIT DESKTOP PREVIEW");
#endif
}

bool AWallhackSensorPeopleActor::ReferencePose(FTransform& Out) const
{
    if (!bReady) return false;
    if (bPreview) { Out = GetActorTransform(); return true; }
#if PLATFORM_ANDROID
    auto* Component = Cast<UOculusXRAnchorComponent>(Anchor.Get());
    if (!IsValid(Component) || !Component->HasValidHandle()) return false;
    FOculusXRAnchorLocationFlags Flags;
    return UOculusXRAnchorBPFunctionLibrary::TryGetAnchorTransformByHandle(Component->GetHandle(), Out, Flags, EOculusXRAnchorSpace::World)
        && Flags.IsValid() && !Out.ContainsNaN();
#else
    return false;
#endif
}

void AWallhackSensorPeopleActor::HidePeople()
{
    Views.Reset(); RadarViews.Reset();
    if (Renderer) Renderer->SetActorHiddenInGame(true);
}

void AWallhackSensorPeopleActor::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    HidePeople(); AimMarker->SetHiddenInGame(true);
    if (bEnding || bSuspended) return;
    auto* Telemetry = GetGameInstance() ? GetGameInstance()->GetSubsystem<UWallhackTelemetrySubsystem>() : nullptr;
    const auto Frame = Telemetry ? Telemetry->GetSensorPeopleFrame() : FWallhackSensorPeopleFrame{};
    bReplay = Frame.bReplay; Unpositioned = Frame.Unpositioned;
    if (!Frame.RegistrationKey.IsEmpty() && Frame.RegistrationKey != RegistrationKey)
    { RegistrationKey = Frame.RegistrationKey; ResetPlacement(); }
    if (RegistrationKey.IsEmpty()) { Status = TEXT("WAITING FOR GROUND STATION"); return; }
    if (bPending && FPlatformTime::Seconds() - RequestedAt > 20)
    { ResetPlacement(); Status = TEXT("ANCHOR TIMED OUT / A TO RETRY"); return; }
    FVector Viewer; FQuat Orientation;
    if (!ViewerPose(Viewer, Orientation)) { Status = TEXT("HEAD TRACKING UNAVAILABLE"); return; }
    if (bHidden) return;
    const float Scale = GetWorld()->GetWorldSettings()->WorldToMeters;
    if (!FMath::IsFinite(Scale) || Scale <= 0) return;
    if (!bReady)
    {
        if (Status == TEXT("HEAD TRACKING UNAVAILABLE"))
            Status = bPending ? TEXT("CREATING SENSOR ANCHOR") : PlacementStep == 0
                ? TEXT("MARK FLOOR BELOW RADAR / RIGHT TRIGGER") : TEXT("MARK FORWARD / AT LEAST 0.5 M FROM ORIGIN");
        FVector Aim;
        if (!bPending && AimFloor(Aim))
        {
            AimMarker->SetWorldLocation(Aim + FVector(0,0,.02 * Scale));
            AimMarker->SetWorldScale3D(FVector(.05 * Scale / 100));
            AimMarker->SetHiddenInGame(false);
        }
        return;
    }
    FTransform Reference;
    if (!ReferencePose(Reference)) { Status = TEXT("SENSOR ANCHOR NOT LOCALIZED"); return; }
    Status = Frame.RegistrationKey.IsEmpty() ? TEXT("GROUND LINK DISCONNECTED")
        : Frame.People.IsEmpty() && Frame.Radar.IsEmpty() ? TEXT("NO FRESH POSITIONED CONTACTS") : TEXT("SENSOR CONTACTS LIVE");
    TArray<FWallhackPersonPose> Poses;
    for (const auto& Person : Frame.People)
    {
        const FVector Feet = WallhackSensorPeopleMath::ToWorld(Person.Position, Reference, Scale);
        FWallhackSensorPersonView View; View.Id = Person.Id; View.bRadar = Person.bRadar; View.Feet = Feet;
        if (!WallhackSpatialMath::ProjectContact(Feet, Viewer, Orientation.Rotator().Yaw, Scale, View.View)) continue;
        Views.Add(View);
        FWallhackPersonPose Pose; Pose.Id = Person.Id; Pose.Feet = Feet / Scale; Pose.Height = 1.65f;
        Pose.Facing = (Reference.GetLocation() - Feet).Rotation().Yaw;
        Pose.Tint = Person.bRadar ? FLinearColor(.25f,.9f,.35f,1) : FLinearColor(1,.62f,.08f,1);
        Pose.SourceLabel = Person.bRadar ? TEXT("RADAR") : TEXT("ESTIMATED");
        Poses.Add(Pose);
    }
    // The relay supplies unmatched radar returns separately. They have their own
    // freshness deadline and do not need a camera detection or alignment match.
    // A generic body is a display assumption, not a camera-confirmed person.
    for (const auto& Dot : Frame.Radar)
    {
        const FVector Feet = WallhackSensorPeopleMath::ToWorld(Dot.Position, Reference, Scale);
        FWallhackSensorPersonView View;
        View.Id = Dot.Id; View.bRadar = true; View.bRadarOnly = true; View.Feet = Feet;
        if (!WallhackSpatialMath::ProjectContact(Feet, Viewer, Orientation.Rotator().Yaw, Scale, View.View)) continue;
        if (Poses.Num() >= UWallhackPeopleSubsystem::MaxPeople)
        {
            RadarViews.Add(View.View); // Keep overflow on the map without exceeding the body budget.
            continue;
        }
        Views.Add(View);
        FWallhackPersonPose Pose; Pose.Id = Dot.Id; Pose.bRadarOnly = true;
        Pose.Feet = Feet / Scale; Pose.Height = 1.65f;
        Pose.Facing = (Reference.GetLocation() - Feet).Rotation().Yaw;
        Pose.Tint = FLinearColor(.35f,.7f,1,1);
        Pose.SourceLabel = TEXT("RADAR ONLY");
        Poses.Add(Pose);
    }
    // Reuse the stereo-tested corner labels with the actual viewer pose.
    // Separate TextRender labels would duplicate telemetry and use a different
    // mobile translucency path from the fixed manual-person renderer.
    if (Renderer) Renderer->Present(Poses, INDEX_NONE, nullptr, true, Scale,
        Viewer / Scale, Orientation, 0, FPlatformTime::Seconds());
}

void AWallhackSensorPeopleActor::ReleaseReference()
{
    bReady = false;
    if (auto* Component = Anchor.Get())
    {
        RemoveTickPrerequisiteComponent(Component);
#if PLATFORM_ANDROID
        DestroyWallhackAnchor(Cast<UOculusXRAnchorComponent>(Component));
#endif
    }
    Anchor.Reset();
}

void AWallhackSensorPeopleActor::ResetPlacement()
{
    ++Generation; bPending = false; PlacementStep = 0;
    ReleaseReference(); HidePeople();
    Status = TEXT("MARK FLOOR BELOW RADAR / RIGHT TRIGGER");
}

void AWallhackSensorPeopleActor::SetPresentationHidden(bool Hidden)
{ bHidden = Hidden; if (Hidden) { HidePeople(); AimMarker->SetHiddenInGame(true); } }
void AWallhackSensorPeopleActor::Suspend() { bSuspended = true; ResetPlacement(); }
void AWallhackSensorPeopleActor::Resume() { bSuspended = false; ResetPlacement(); }
void AWallhackSensorPeopleActor::EndPlay(const EEndPlayReason::Type Reason)
{
    bEnding = true; ResetPlacement();
    FCoreDelegates::ApplicationWillEnterBackgroundDelegate.Remove(BackgroundHandle);
    FCoreDelegates::ApplicationHasEnteredForegroundDelegate.Remove(ForegroundHandle);
    FCoreDelegates::VRHeadsetRecenter.Remove(RecenterHandle);
    if (Renderer) Renderer->Destroy();
    Super::EndPlay(Reason);
}
