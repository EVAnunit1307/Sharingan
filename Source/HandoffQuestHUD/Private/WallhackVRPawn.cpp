#include "WallhackVRPawn.h"
#include "WallhackControllerPose.h"
#include "WallhackMountedController.h"

#include "Camera/CameraComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Features/IModularFeatures.h"
#include "HAL/IConsoleManager.h"
#include "IMotionController.h"
#include "IXRTrackingSystem.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "InputCoreTypes.h"
#include "Math/RotationMatrix.h"
#include "MotionControllerComponent.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/App.h"

namespace
{
    bool ActiveControllerPose(UObject* WorldContext, EControllerHand Hand,
        EXRControllerPoseType PoseType, FTransform& Out)
    {
        if (!GEngine || !GEngine->XRSystem.IsValid()) return false;
        FXRMotionControllerState State;
        // Ask the active HMD runtime to select its own input provider and apply
        // tracking-to-world exactly once. A generic motion component polls every
        // registered provider; Meta's legacy provider can coexist with OpenXR.
        GEngine->XRSystem->GetMotionControllerState(WorldContext,
            EXRSpaceType::UnrealWorldSpace, Hand, PoseType, State);
        return WallhackControllerPose::Resolve(State, Out);
    }

#if !UE_BUILD_SHIPPING
    FAutoConsoleCommandWithWorld ControllerStatusCommand(
        TEXT("Wallhack.ControllerStatus"), TEXT("Log active XR and physical controller pose diagnostics."),
        FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
        {
            if (!World || !GEngine || !GEngine->XRSystem.IsValid()) return;
            const auto XR = GEngine->XRSystem;
            UE_LOG(LogTemp, Display, TEXT("ControllerStatus runtime=%s use_focus=%d has_focus=%d head_tracked=%d"),
                *XR->GetSystemName().ToString(), FApp::UseVRFocus(), FApp::HasVRFocus(), XR->IsTracking(IXRTrackingSystem::HMDDeviceId));
            UE_LOG(LogTemp, Display, TEXT("ControllerStatus mounted %s"), *WallhackMountedController::GetDiagnostics());
            for (const EControllerHand Hand : {EControllerHand::Left, EControllerHand::Right})
            {
                FXRMotionControllerState State;
                XR->GetMotionControllerState(World, EXRSpaceType::UnrealWorldSpace, Hand,
                    Hand == EControllerHand::Left ? EXRControllerPoseType::Aim : EXRControllerPoseType::Grip, State);
                UE_LOG(LogTemp, Display, TEXT("ControllerStatus active hand=%s profile=%s valid=%d status=%d world=%s"),
                    Hand == EControllerHand::Left ? TEXT("LeftAim") : TEXT("RightGrip"),
                    *State.DeviceName.ToString(), State.bValid, int32(State.TrackingStatus), *State.ControllerLocation.ToString());
            }
            for (auto* Provider : IModularFeatures::Get().GetModularFeatureImplementations<IMotionController>(IMotionController::GetModularFeatureName()))
            {
                for (const FName Source : {FName(TEXT("LeftAim")), FName(TEXT("RightGrip"))})
                {
                    FVector Position = FVector::ZeroVector; FRotator Rotation = FRotator::ZeroRotator;
                    const bool bPose = Provider->GetControllerOrientationAndPosition(0, Source, Rotation, Position, World->GetWorldSettings()->WorldToMeters);
                    UE_LOG(LogTemp, Display, TEXT("ControllerStatus provider=%s source=%s pose=%d status=%d local=%s"),
                        *Provider->GetMotionControllerDeviceTypeName().ToString(), *Source.ToString(), bPose,
                        int32(Provider->GetControllerTrackingStatus(0, Source)), *Position.ToString());
                }
            }
        }));
#endif
}

AWallhackVRPawn::AWallhackVRPawn()
{
    PrimaryActorTick.bCanEverTick = false;
#if !PLATFORM_ANDROID && !UE_BUILD_SHIPPING
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = false;
#endif

    VROrigin = CreateDefaultSubobject<USceneComponent>(TEXT("VROrigin"));
    SetRootComponent(VROrigin);

    Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
    Camera->SetupAttachment(VROrigin);

    // This is the actual fix: bLockToHmd (true by default, set explicitly here
    // to make it obvious) is what makes the engine drive this camera's
    // transform from the Quest's live tracked head pose every frame. Because
    // nothing in the project previously had a camera component at all, the
    // PlayerCameraManager had nothing HMD-tracked to report, and anything
    // positioned relative to it (the world-space HUD panel, the heading
    // readout) floated somewhere disconnected from where the player was
    // actually looking.
    Camera->bLockToHmd = true;
    Camera->SetFieldOfView(90.f);
    Camera->bAutoActivate = true;
    RightAim = CreateDefaultSubobject<UMotionControllerComponent>(TEXT("RightNavigationAim"));
    RightAim->SetupAttachment(VROrigin);
    RightAim->SetTrackingMotionSource(FName(TEXT("RightAim")));
    RightGrip = CreateDefaultSubobject<UMotionControllerComponent>(TEXT("RightNavigationGrip"));
    RightGrip->SetupAttachment(VROrigin);
    RightGrip->SetTrackingMotionSource(FName(TEXT("Right")));
    LeftGrip = CreateDefaultSubobject<UMotionControllerComponent>(TEXT("LeftNavigationGrip"));
    LeftGrip->SetupAttachment(VROrigin);
    LeftGrip->SetTrackingMotionSource(FName(TEXT("Left")));
    LeftAim=CreateDefaultSubobject<UMotionControllerComponent>(TEXT("LeftSensorAim"));
    LeftAim->SetupAttachment(VROrigin);
    LeftAim->SetTrackingMotionSource(FName(TEXT("LeftAim")));
}

void AWallhackVRPawn::BeginPlay()
{
    Super::BeginPlay();

#if !PLATFORM_ANDROID && !UE_BUILD_SHIPPING
    if (FParse::Param(FCommandLine::Get(), TEXT("WallhackTrackingPreview"))
        || FParse::Param(FCommandLine::Get(), TEXT("WallhackNavigationPreview"))
        || FParse::Param(FCommandLine::Get(), TEXT("WallhackSensorPeoplePreview")))
    {
        Camera->bLockToHmd = false;
        Camera->SetRelativeTransform(FTransform::Identity);
        if (FParse::Param(FCommandLine::Get(), TEXT("WallhackNavigationPreview"))
            || FParse::Param(FCommandLine::Get(), TEXT("WallhackSensorPeoplePreview"))) SetActorLocation(FVector(0,0,170));
        SetActorTickEnabled(true);
    }
#endif
}

void AWallhackVRPawn::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);

#if !PLATFORM_ANDROID && !UE_BUILD_SHIPPING
    APlayerController* PC = Cast<APlayerController>(GetController());
    if (!PC || !(FParse::Param(FCommandLine::Get(), TEXT("WallhackTrackingPreview"))
        || FParse::Param(FCommandLine::Get(), TEXT("WallhackNavigationPreview"))
        || FParse::Param(FCommandLine::Get(), TEXT("WallhackSensorPeoplePreview"))))
    {
        return;
    }

    // This camera is only for the explicit desktop tracking preview. Movement
    // stays horizontal even while looking up or down, at one metre per second.
    const float StepSeconds = FMath::Min(DeltaSeconds, 0.1f);
    const auto KeyAxis = [PC](const FKey& Positive, const FKey& Negative)
    {
        return (PC->IsInputKeyDown(Positive) ? 1.f : 0.f) -
            (PC->IsInputKeyDown(Negative) ? 1.f : 0.f);
    };
    FRotator ViewRotation = GetActorRotation();
    ViewRotation.Yaw = FMath::UnwindDegrees(ViewRotation.Yaw +
        KeyAxis(EKeys::Right, EKeys::Left) * 60.f * StepSeconds);
    ViewRotation.Pitch = FMath::Clamp(ViewRotation.Pitch +
        KeyAxis(EKeys::Up, EKeys::Down) * 60.f * StepSeconds, -85.0, 85.0);
    ViewRotation.Roll = 0.f;
    SetActorRotation(ViewRotation);

    const FRotator HorizontalRotation(0.f, ViewRotation.Yaw, 0.f);
    const FVector Forward = HorizontalRotation.Vector();
    const FVector Right = FRotationMatrix(HorizontalRotation).GetUnitAxis(EAxis::Y);
    FVector Move = Forward * KeyAxis(EKeys::W, EKeys::S) +
        Right * KeyAxis(EKeys::D, EKeys::A) +
        FVector::UpVector * KeyAxis(EKeys::E, EKeys::Q);
    Move = Move.GetClampedToMaxSize(1.f);
    const float WorldToMeters = GetWorld()->GetWorldSettings()->WorldToMeters;
    AddActorWorldOffset(Move * WorldToMeters * StepSeconds);
#endif
}

bool AWallhackVRPawn::GetNavigationAim(FVector& Origin, FVector& Direction) const
{
#if PLATFORM_ANDROID
    if (!RightAim || !RightAim->IsTracked()) return false;
    Origin=RightAim->GetComponentLocation(); Direction=RightAim->GetForwardVector();
#else
    if (!Camera) return false;
    Origin=Camera->GetComponentLocation(); Direction=Camera->GetForwardVector();
#endif
    return true;
}

void AWallhackVRPawn::GetTrackedNavigationHands(TArray<FVector>& WorldPositions) const
{
    WorldPositions.Reset();
    if(RightGrip && RightGrip->IsTracked())WorldPositions.Add(RightGrip->GetComponentLocation());
    if(LeftGrip && LeftGrip->IsTracked())WorldPositions.Add(LeftGrip->GetComponentLocation());
}

bool AWallhackVRPawn::GetTrackedGrip(bool bLeft,FTransform& Out) const
{
    if (GEngine && GEngine->XRSystem.IsValid())
        return ActiveControllerPose(const_cast<AWallhackVRPawn*>(this), bLeft ? EControllerHand::Left : EControllerHand::Right, EXRControllerPoseType::Grip, Out);
    const auto* Grip=bLeft?LeftGrip.Get():RightGrip.Get();
    // Inertial-only orientation is not a measured six-degree-of-freedom pose.
    if(!Grip||!Grip->IsTracked()||Grip->CurrentTrackingStatus!=ETrackingStatus::Tracked)return false;
    Out=Grip->GetComponentTransform();
    return !Out.ContainsNaN();
}

bool AWallhackVRPawn::GetSensorRigAim(FTransform& Out,bool* bEstimated) const
{
    if(bEstimated)*bEstimated=false;
    bool Estimated=false;
    if (WallhackMountedController::GetAim(Out,Estimated))
    {if(bEstimated)*bEstimated=Estimated;return true;}
    if (GEngine && GEngine->XRSystem.IsValid())
        return ActiveControllerPose(const_cast<AWallhackVRPawn*>(this), EControllerHand::Left, EXRControllerPoseType::Aim, Out);
    if(!LeftAim||!LeftAim->IsTracked()||LeftAim->CurrentTrackingStatus!=ETrackingStatus::Tracked)return false;
    Out=LeftAim->GetComponentTransform();return !Out.ContainsNaN();
}

bool AWallhackVRPawn::GetSensorCalibrationProbe(FTransform& Out) const
{
    FTransform Aim;
    if(GEngine&&GEngine->XRSystem.IsValid())
    {
        if(!ActiveControllerPose(const_cast<AWallhackVRPawn*>(this),EControllerHand::Right,EXRControllerPoseType::Aim,Aim))return false;
    }
    else
    {
        if(!RightAim||!RightAim->IsTracked()||RightAim->CurrentTrackingStatus!=ETrackingStatus::Tracked)return false;
        Aim=RightAim->GetComponentTransform();
    }
    const double Scale=GetWorld()->GetWorldSettings()->WorldToMeters;
    if(Aim.ContainsNaN()||!FMath::IsFinite(Scale)||Scale<=0)return false;
    Out=Aim;
    // A visible free-space probe avoids placing controller plastic inside the rig.
    Out.SetLocation(Aim.GetLocation()+Aim.GetUnitAxis(EAxis::X)*(.12*Scale));
    return true;
}
