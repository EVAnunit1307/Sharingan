#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WallhackSensorPeopleTypes.h"
#include "WallhackSpatialMath.h"
#include "WallhackSensorPeopleActor.generated.h"

class AWallhackPeopleRenderer;
class UStaticMeshComponent;
class UActorComponent;

struct FWallhackSensorPersonView
{
    int32 Id = INDEX_NONE;
    bool bRadar = false;
    FVector Feet = FVector::ZeroVector;
    WallhackSpatialMath::FContactView View;
};

/** Session-only registration of a stationary sensor reference to the Quest world. */
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
protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
private:
    bool ViewerPose(FVector& Position, FQuat& Orientation) const;
    bool AimFloor(FVector& Point) const;
    bool ReferencePose(FTransform& Out) const;
    void CreateReference();
    void ReleaseReference();
    void Suspend();
    void Resume();
    void HidePeople();
    UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> AimMarker;
    UPROPERTY(Transient) TObjectPtr<AWallhackPeopleRenderer> Renderer;
    TWeakObjectPtr<UActorComponent> Anchor;
    TArray<FWallhackSensorPersonView> Views;
    TArray<WallhackSpatialMath::FContactView> RadarViews;
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
