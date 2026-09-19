#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WallhackSpatialContacts.h"
#include "WallhackWorldContact.generated.h"

class UActorComponent;
class USceneComponent;
class UStaticMeshComponent;

enum class EWallhackContactTrackingState : uint8
{
    Unplaced,
    Creating,
    Tracked,
    Unlocalized,
    Failed
};

/** Synthetic contacts sharing one stationary anchor for this application session. */
UCLASS()
class HANDOFFQUESTHUD_API AWallhackWorldContact : public AActor
{
    GENERATED_BODY()

public:
    AWallhackWorldContact();

    static constexpr int32 ContactId = 1;
    static constexpr float StaleAfterSeconds = 0.75f;

    /** False means no position should be displayed, including on the minimap. */
    bool GetContactWorldPosition(FVector& OutPosition) const;

    /** Stable IDs in ascending order; invalid observations retain only identity and age. */
    void GetSpatialContacts(TArray<FWallhackSpatialContact>& Out) const;
    void SetMultipleContactsEnabled(bool bEnabled);
    bool AreMultipleContactsEnabled() const { return bMultipleContactsEnabled; }
    void SetSimulationPaused(bool bPaused);
    bool IsSimulationPaused() const { return bSimulationPaused; }
    void SetContactUpdatesEnabled(bool bEnabled);
    bool AreContactUpdatesEnabled() const { return bContactUpdatesEnabled; }

    /** Shared engine lifecycle/test entry point; foreground requires a fresh Tick sample. */
    void SetApplicationSuspended(bool bSuspended);

    /** Shared world-space viewer pose used by placement, range and the minimap. */
    bool GetViewerWorldPose(FVector& OutPosition, FQuat& OutOrientation) const;

    FString GetTrackingStatus() const;
    EWallhackContactTrackingState GetTrackingState() const { return TrackingState; }

    /** Sample once, three horizontal metres ahead at the current eye height. */
    void PlaceInFrontOfViewer();

    virtual void Tick(float DeltaSeconds) override;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
    UPROPERTY(VisibleAnywhere)
    TObjectPtr<USceneComponent> ContactRoot;

    UPROPERTY(VisibleAnywhere)
    TObjectPtr<UStaticMeshComponent> ContactMesh;

    UPROPERTY(VisibleAnywhere)
    TObjectPtr<UStaticMeshComponent> MovingContactMesh02;

    UPROPERTY(VisibleAnywhere)
    TObjectPtr<UStaticMeshComponent> MovingContactMesh03;

    // Actor owns the dynamically created component; generic type keeps Meta
    // headers and reflected dependencies out of non-Android builds.
    TWeakObjectPtr<UActorComponent> AnchorComponent;

    EWallhackContactTrackingState TrackingState = EWallhackContactTrackingState::Unplaced;
    FString StatusDetail = TEXT("WAITING FOR HEAD TRACKING");
    FString AnchorId;
    uint32 PlacementGeneration = 0;
    double RequestStartSeconds = 0.0;
    double LastLogSeconds = -10.0;
    float StableTrackingSeconds = 0.0f;
    double SimulationTimeSeconds = 0.0;
    double ApplicationSuspendedAtSeconds = 0.0;
    float SampleAgeSeconds = TNumericLimits<float>::Max();
    FDelegateHandle BackgroundDelegateHandle;
    FDelegateHandle ForegroundDelegateHandle;
    bool bAutomaticPlacementAttempted = false;
    bool bCreatePending = false;
    bool bCreateTimedOut = false;
    bool bEndingPlay = false;
    bool bDesktopPreview = false;
    bool bVisualAssetsReady = false;
    bool bMultipleContactsEnabled = true;
    bool bSimulationPaused = false;
    bool bContactUpdatesEnabled = true;
    bool bApplicationSuspended = false;
    bool bHasSample = false;
    bool bNeedsFreshSample = true;
    bool bSkipNextMotionDelta = true;

    bool IsAnchorPoseLocalized() const;
    bool HasUsableTrackingPose() const;
    bool IsContactSampleStale() const;
    void PublishContactSample(float MotionDeltaSeconds);
    void UpdateContactVisuals();
    void OnApplicationBackground();
    void OnApplicationForeground();
    void SetTrackingState(EWallhackContactTrackingState NewState, const FString& Detail);
    void ReleaseAnchor();
    void LogContact() const;
};
