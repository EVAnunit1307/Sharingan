#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "WallhackVRPawn.generated.h"

class UCameraComponent;
class USceneComponent;
class UMotionControllerComponent;

/**
 * Minimal VR pawn.
 *
 * The project previously had no Pawn class at all (GameMode never set
 * DefaultPawnClass, Content/ has no Pawn Blueprint, and the default map is
 * Engine's generic /Engine/Maps/Entry). That meant the engine fell back to
 * ADefaultPawn, which has no UCameraComponent. Without an HMD-locked camera
 * component, APlayerCameraManager::GetCameraLocation()/GetCameraRotation()
 * (what WallhackVRHUDActor and the heading readout in
 * WallhackOverlayWidget both read every frame) never actually tracked the
 * Quest headset's real pose. Persistent Passthrough itself is a
 * system-composited layer positioned directly from the XR runtime's head
 * tracking, independent of the game's camera - which is why passthrough
 * followed head movement fine while the HUD panel stayed wherever the
 * untracked default camera happened to be, invisible no matter where you
 * looked.
 *
 * This pawn exists purely to give the camera manager a real, HMD-tracked
 * camera to report.
 */
UCLASS()
class HANDOFFQUESTHUD_API AWallhackVRPawn : public APawn
{
    GENERATED_BODY()

public:
    AWallhackVRPawn();
    virtual void Tick(float DeltaSeconds) override;
    bool GetNavigationAim(FVector& Origin, FVector& Direction) const;
    bool GetTrackedGrip(bool bLeft,FTransform& Out) const;
    bool GetSensorRigAim(FTransform& Out,bool* bEstimated=nullptr) const;
    bool GetSensorCalibrationProbe(FTransform& Out) const;
    void GetTrackedNavigationHands(TArray<FVector>& WorldPositions) const;

protected:
    virtual void BeginPlay() override;

private:
    UPROPERTY(VisibleAnywhere)
    TObjectPtr<USceneComponent> VROrigin;

    UPROPERTY(VisibleAnywhere)
    TObjectPtr<UCameraComponent> Camera;
    UPROPERTY(VisibleAnywhere)
    TObjectPtr<UMotionControllerComponent> RightAim;
    UPROPERTY(VisibleAnywhere)
    TObjectPtr<UMotionControllerComponent> RightGrip;
    UPROPERTY(VisibleAnywhere)
    TObjectPtr<UMotionControllerComponent> LeftGrip;
    UPROPERTY(VisibleAnywhere)
    TObjectPtr<UMotionControllerComponent> LeftAim;
};
