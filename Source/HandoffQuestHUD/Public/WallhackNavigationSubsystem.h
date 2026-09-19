#pragma once
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Async/Future.h"
#include "WallhackNavigationProviders.h"
#include "WallhackNavigationSubsystem.generated.h"

class AWallhackNavigationRenderer;
class UAndroidPermissionCallbackProxy;

UCLASS()
class HANDOFFQUESTHUD_API UWallhackNavigationSubsystem : public UTickableWorldSubsystem
{
    GENERATED_BODY()
public:
    virtual void Deinitialize() override;
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(UWallhackNavigationSubsystem,STATGROUP_Tickables); }
    virtual bool IsTickable() const override { return bActive && !IsTemplate(); }
    void Start();
    bool SetDestination(const FVector& SurfaceMeters,const FVector& StandingMeters);
    bool SetPersonDestination(int32 PersonId,const FVector& FeetMeters);
    void CancelNavigation();
    void BeginAim();
    void EndAim();
    void Confirm();
    void SetHidden(bool Hidden);
    const WallhackNav::FDisplaySnapshot& GetDisplaySnapshot() const { return Display; }
    FString GetInteractionHint() const;
    const WallhackNav::FScene& GetScene() const { return Scene; }
    bool IsSceneOccluded(FVector OriginMeters,FVector PointMeters) const;
    // Deterministic providers also exercise the actual subsystem in automation.
    void SetProviders(TSharedPtr<WallhackNav::ISceneProvider> InScene,TSharedPtr<WallhackNav::IDepthProvider> InDepth);
    void Suspend();
    void Resume();
    void CoordinateDiscontinuity();
private:
    UFUNCTION() void OnPermissions(const TArray<FString>& Permissions,const TArray<bool>& Granted);
    UFUNCTION() void OnCaptureComplete(bool Success);
    void StartProviders();
    void UpdatePose(float DeltaTime);
    void UpdateAim();
    void SampleDepth(double Deadline);
    WallhackNav::EDepthResult QueryDepth(FVector Origin,FVector Direction,float Distance,WallhackNav::FSurfaceHit& Hit);
    void Fuse(const WallhackNav::FCell& Cell,FVector Point);
    void UpdateRoute();
    void UpdateMetrics();
    void HideGuidance();
    WallhackNav::FMap Map;
    WallhackNav::FScene Scene;
    WallhackNav::FDisplaySnapshot Display;
    TSharedPtr<WallhackNav::ISceneProvider> SceneProvider;
    TSharedPtr<WallhackNav::IDepthProvider> DepthProvider;
    TFuture<WallhackNav::FRoute> Pending;
    uint64 DestinationVersion=0, PendingDestination=0;
    uint64 LastPlanRevision=0;
    int32 SeedX=0, SeedY=0, SeedMaxX=0, SeedMaxY=0, SeedMinX=0;
    int32 ProbeIndex=0, ProbePhase=0;
    FVector Probe=FVector::ZeroVector;
    struct FColumnEvidence
    {
        WallhackNav::FCell Floor;
        double Seen[5]={-100,-100,-100,-100,-100};
        double LastSample=-100;
    };
    TMap<FIntPoint,FColumnEvidence> ClearanceColumns;
    uint32 CompletedClearanceChecks=0;
    WallhackNav::FWearerDepthMask WearerMask;
    uint32 WearerOccludedQueries=0;
    FVector PreviousViewer=FVector::ZeroVector;
    FQuat PreviousOrientation=FQuat::Identity;
    double Now=0, LastPlan=-100, LastMetrics=-100, LastDepth=-100, LastLiveHit=-100, LastPoll=-100, FreshAfter=0, LastLog=0;
    float StableSeconds=0;
    float MappingAverage=0,QueryAverage=0,FPSAverage=0;
    bool bActive=false,bProvidersStarted=false,bSceneLoaded=false,bSeeding=false,bSuspended=false,bPose=false,bPreviousPose=false;
    bool bNeedsPlan=false,bNeedsCapture=false,bCaptureInProgress=false,bPermissionDenied=false,bCoordinatePending=false;
    FDelegateHandle BackgroundHandle,ForegroundHandle,RecenterHandle;
    UPROPERTY(Transient) TObjectPtr<AWallhackNavigationRenderer> Renderer;
    UPROPERTY(Transient) TObjectPtr<UAndroidPermissionCallbackProxy> PermissionRequest;
};
