#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WallhackSensorPeopleTypes.h"
#include "WallhackSpatialMath.h"
#include "WallhackTrackedRig.h"
#include "WallhackSensorPeopleActor.generated.h"

class AWallhackPeopleRenderer;
class UStaticMeshComponent;
class UActorComponent;
class UProceduralMeshComponent;

struct FWallhackRigCalibrationView
{
    bool bControllerRig=false,bTracked=false,bEstimated=false;
    int32 Step=1; // place, sample, review, aligned
    float Progress=0;
    FVector OffsetCm=FVector::ZeroVector;
};

struct FWallhackSensorPersonView
{
    int32 Id = INDEX_NONE;
    bool bRadar = false;
    bool bRadarOnly = false;
    bool bFused = false;
    FLinearColor Color = FLinearColor::White;
    FVector Feet = FVector::ZeroVector;
    WallhackSpatialMath::FContactView View;
};

/** Session registration of a fixed anchor or a controller-tracked rigid mount. */
UCLASS()
class HANDOFFQUESTHUD_API AWallhackSensorPeopleActor : public AActor
{
    GENERATED_BODY()
public:
    AWallhackSensorPeopleActor();
    virtual void Tick(float DeltaSeconds) override;
    void ConfirmPlacement();
    void ResetPlacement();
    void SetPresentationHidden(bool Hidden);
    const FString& GetStatus() const { return Status; }
    const TArray<FWallhackSensorPersonView>& GetPeopleViews() const { return Views; }
    const TArray<WallhackSpatialMath::FContactView>& GetRadarViews() const { return RadarViews; }
    bool IsReplay() const { return bReplay; }
    int32 GetUnpositionedCount() const { return Unpositioned; }
    FWallhackRigCalibrationView GetCalibrationView() const;
protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
private:
    bool ViewerPose(FVector& Position, FQuat& Orientation) const;
    bool AimFloor(FVector& Point) const;
    bool ReferencePose(FTransform& Out,bool* bEstimated=nullptr) const;
    bool GripPose(bool bLeft,FTransform& Out,bool* bEstimated=nullptr) const;
    FString AlignmentPrompt() const;
    void CreateReference();
    void ReleaseReference();
    void Suspend();
    void Resume();
    void HidePeople();
    void PresentCalibrationGuides(const FTransform& SensorMetres);
    UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> AimMarker;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UProceduralMeshComponent> CalibrationGuides;
    UPROPERTY(Transient) TObjectPtr<AWallhackPeopleRenderer> Renderer;
    TWeakObjectPtr<UActorComponent> Anchor;
    TArray<FWallhackSensorPersonView> Views;
    TArray<WallhackSpatialMath::FContactView> RadarViews;
    FWallhackSensorPositionInterpolator Positions;
    FWallhackRigAlignment RigAlignment;
    FWallhackRigCalibrationCapture CalibrationCapture;
    FWallhackRigHistory RigHistory;
    struct FWorldTrack
    {
        FString Sample;
        FVector Position=FVector::ZeroVector,Velocity=FVector::ZeroVector;
        FTransform Reference;
        double At=-1,LastSeen=-1;
        float Facing=0;
        bool bHasFacing=false;
        bool bRigEstimated=false;
    };
    TMap<int32,FWorldTrack> WorldTracks;
    bool bControllerRig=false;
    bool bRigTracked=false;
    bool bRigEstimated=false,bMountEstimated=false;
    double ShowGuidesUntil=0;
    FString RegistrationKey;
    FString Status = TEXT("WAITING FOR GROUND STATION");
    FVector Origin = FVector::ZeroVector;
    FVector Forward = FVector::ForwardVector;
    uint32 Generation = 0;
    double RequestedAt = 0;
    int32 PlacementStep = 0;
    int32 Unpositioned = 0;
    bool bPending = false, bReady = false, bHidden = false, bSuspended = false, bEnding = false;
    bool bPreview = false, bReplay = false;
    FDelegateHandle BackgroundHandle, ForegroundHandle, RecenterHandle;
};
