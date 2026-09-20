#include "WallhackVRPawn.h"

#include "Camera/CameraComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "InputCoreTypes.h"
#include "Math/RotationMatrix.h"
#include "MotionControllerComponent.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

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
