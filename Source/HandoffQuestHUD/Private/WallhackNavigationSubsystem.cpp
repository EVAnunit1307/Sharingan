#include "WallhackNavigationSubsystem.h"
#include "WallhackPeopleSubsystem.h"
#include "WallhackNavigationRenderer.h"
#include "WallhackVRPawn.h"
#include "Async/Async.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "HeadMountedDisplayFunctionLibrary.h"
#include "IXRTrackingSystem.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CoreDelegates.h"
#include "HAL/PlatformMisc.h"
#if PLATFORM_ANDROID
#include "AndroidPermissionFunctionLibrary.h"
#include "AndroidPermissionCallbackProxy.h"
#include "MRUtilityKitSubsystem.h"
#endif

using namespace WallhackNav;
void UWallhackNavigationSubsystem::SetProviders(TSharedPtr<ISceneProvider> S,TSharedPtr<IDepthProvider> D)
{ check(!bActive);SceneProvider=MoveTemp(S);DepthProvider=MoveTemp(D); }
void UWallhackNavigationSubsystem::Start()
{
    if(bActive)return;bActive=true;
    BackgroundHandle=FCoreDelegates::ApplicationWillEnterBackgroundDelegate.AddUObject(this,&UWallhackNavigationSubsystem::Suspend);
    ForegroundHandle=FCoreDelegates::ApplicationHasEnteredForegroundDelegate.AddUObject(this,&UWallhackNavigationSubsystem::Resume);
    RecenterHandle=FCoreDelegates::VRHeadsetRecenter.AddUObject(this,&UWallhackNavigationSubsystem::CoordinateDiscontinuity);
    Renderer=GetWorld()->SpawnActor<AWallhackNavigationRenderer>();
#if PLATFORM_ANDROID
    if(auto* M=GetWorld()->GetGameInstance()->GetSubsystem<UMRUKSubsystem>())M->OnCaptureComplete.AddDynamic(this,&UWallhackNavigationSubsystem::OnCaptureComplete);
    if(!UAndroidPermissionFunctionLibrary::CheckPermission(TEXT("com.oculus.permission.USE_SCENE")))
    {
        PermissionRequest=UAndroidPermissionFunctionLibrary::AcquirePermissions({TEXT("com.oculus.permission.USE_SCENE")});
        PermissionRequest->OnPermissionsGrantedDynamicDelegate.AddDynamic(this,&UWallhackNavigationSubsystem::OnPermissions);
        Display.Tracking=TEXT("ALLOW ROOM PERMISSION");return;
    }
#endif
    StartProviders();
}
void UWallhackNavigationSubsystem::OnPermissions(const TArray<FString>& P,const TArray<bool>& G)
{
    const int32 I=P.IndexOfByKey(TEXT("com.oculus.permission.USE_SCENE"));
    bPermissionDenied=!G.IsValidIndex(I)||!G[I];
    if(bPermissionDenied){bProvidersStarted=false;Display.Tracking=TEXT("ROOM PERMISSION DENIED");HideGuidance();}else StartProviders();
}
void UWallhackNavigationSubsystem::StartProviders()
{
    if(bProvidersStarted)return;
    if(!SceneProvider)SceneProvider=MakeSceneProvider(GetWorld());
    if(!DepthProvider)DepthProvider=MakeDepthProvider(GetWorld());
    SceneProvider->Start();DepthProvider->Start();bProvidersStarted=true;
}
void UWallhackNavigationSubsystem::OnCaptureComplete(bool Success)
{
    bCaptureInProgress=false;
    UE_LOG(LogTemp,Display,TEXT("Wallhack navigation: room capture completed success=%d"),Success);
    if(Success&&SceneProvider)
    {
        SceneProvider->Start();bSceneLoaded=false;Map.Reset();ClearanceColumns.Reset();ProbePhase=0;
        MapOutline.Reset();OutlineRevision=Map.Revision; // Reject any outline from the previous scan.
    }
    bNeedsCapture=!Success;
}
void UWallhackNavigationSubsystem::Deinitialize()
{
    bActive=false;FCoreDelegates::ApplicationWillEnterBackgroundDelegate.Remove(BackgroundHandle);
    FCoreDelegates::ApplicationHasEnteredForegroundDelegate.Remove(ForegroundHandle);
    FCoreDelegates::VRHeadsetRecenter.Remove(RecenterHandle);
#if PLATFORM_ANDROID
    if(PermissionRequest)PermissionRequest->OnPermissionsGrantedDynamicDelegate.RemoveAll(this);
    if(GetWorld()&&GetWorld()->GetGameInstance())if(auto* M=GetWorld()->GetGameInstance()->GetSubsystem<UMRUKSubsystem>())M->OnCaptureComplete.RemoveAll(this);
#endif
    // Jobs own only immutable tiles and value poses. They never reference UObjects.
    if(Pending.IsValid())Pending.Wait();
    if(PendingOutline.IsValid())PendingOutline.Wait();
    DepthProvider.Reset();SceneProvider.Reset();
    if(Renderer)Renderer->Destroy();
    Super::Deinitialize();
}
void UWallhackNavigationSubsystem::HideGuidance()
{
    Display.bGuidance=false;Display.bPreviewValid=false;StableSeconds=0;ProbePhase=0;
    ClearanceColumns.Reset();
    FreshAfter=Now;Display.State=Display.bHasTarget?ERouteState::Relocalizing:ERouteState::Idle;
    if(Renderer)Renderer->SetActorHiddenInGame(true);
    if(auto* People=GetWorld()->GetSubsystem<UWallhackPeopleSubsystem>()) People->RefreshPresentation();
}
void UWallhackNavigationSubsystem::Suspend(){bSuspended=true;HideGuidance();}
void UWallhackNavigationSubsystem::Resume(){bSuspended=false;bPreviousPose=false;HideGuidance();bNeedsPlan=Display.bHasTarget;}
void UWallhackNavigationSubsystem::CoordinateDiscontinuity()
{
    HideGuidance();bCoordinatePending=true;bPreviousPose=false;++DestinationVersion;
    // MRUK corrects the pawn into the existing room frame. Forgetting depth
    // here erased unseen walls on a recenter or a quick head turn. Retain the
    // same anchored memory as person poses; pause until localization and a new
    // observation, then plan again from the corrected wearer position.
    Display.Route={};bNeedsPlan=Display.bHasTarget;
    if(Renderer)Renderer->InvalidateTrail();
    UE_LOG(LogTemp,Display,TEXT("Wallhack navigation: relocalizing with %d memory tiles retained"),Map.Tiles.Num());
}
void UWallhackNavigationSubsystem::UpdatePose(float Dt)
{
    auto* PC=UGameplayStatics::GetPlayerController(this,0);
    bPose=PC&&PC->PlayerCameraManager&&!bSuspended;
#if PLATFORM_ANDROID
    bPose=bPose&&GEngine&&GEngine->XRSystem.IsValid()&&GEngine->XRSystem->IsTracking(IXRTrackingSystem::HMDDeviceId)
        &&GEngine->XRSystem->HasValidTrackingPosition()&&UHeadMountedDisplayFunctionLibrary::GetHMDWornState()!=EHMDWornState::NotWorn;
#endif
    if(bSceneLoaded&&SceneProvider&&!SceneProvider->IsLocalized())bPose=false;
    if(!bPose){HideGuidance();bPreviousPose=false;return;}
    const float Scale=GetWorld()->GetWorldSettings()->WorldToMeters;
    if(!FMath::IsFinite(Scale)||Scale<=0){bPose=false;HideGuidance();return;}
    const FVector P=PC->PlayerCameraManager->GetCameraLocation()/Scale;
    const FQuat Q=PC->PlayerCameraManager->GetCameraRotation().Quaternion();
    if(bPreviousPose&&(FVector::Dist(P,PreviousViewer)>FMath::Max(.75f,Dt*4.f)
        || Q.AngularDistance(PreviousOrientation)>FMath::DegreesToRadians(FMath::Max(55.f,Dt*400.f))))CoordinateDiscontinuity();
    Display.Viewer=P;Display.Orientation=Q;PreviousViewer=P;PreviousOrientation=Q;bPreviousPose=true;
    WearerMask.Head=P;WearerMask.TrackedHands.Reset();
    if(auto* Pawn=Cast<AWallhackVRPawn>(PC->GetPawn()))
    {
        Pawn->GetTrackedNavigationHands(WearerMask.TrackedHands);
        for(FVector& Hand:WearerMask.TrackedHands)Hand/=Scale;
    }
    StableSeconds+=FMath::Min(Dt,.1f);
    if(StableSeconds>.5f&&LastLiveHit>FreshAfter&&bSceneLoaded&&!bSeeding)
    {Display.bGuidance=true;bCoordinatePending=false;}
}
void UWallhackNavigationSubsystem::Fuse(const FCell& C,FVector P)
{
    const FIntPoint Key=FMapSnapshot::Key(P);
    if(C.Occupancy==EOccupancy::Occupied||C.Occupancy==EOccupancy::Unsupported)ClearanceColumns.Remove(Key);
    if(Map.Observe(Key,C))bNeedsPlan=Display.bHasTarget;
}
EDepthResult UWallhackNavigationSubsystem::QueryDepth(FVector O,FVector D,float Distance,FSurfaceHit& Hit)
{
    if(!DepthProvider)return EDepthResult::Unavailable;
    const auto Result=DepthProvider->Query(O,D,Distance,Hit);
    WearerMask.Floor=Map.Floor;
    if(Result==EDepthResult::Hit && WearerMask.Contains(Hit.Point))
    {
        ++WearerOccludedQueries;
        return EDepthResult::Unknown;
    }
    return Result;
}
void UWallhackNavigationSubsystem::SampleDepth(double Deadline)
{
    if(!DepthProvider||!bPose)return;
    // Maximum eight SDK calls AND a 1ms aggregate CPU deadline. One slow native
    // call cannot be preempted; its measured cost is published in the HUD/log.
    for(int32 Budget=0;Budget<8&&FPlatformTime::Seconds()<Deadline;++Budget)
    {
        if(ProbePhase==0)
        {
            ++ProbeIndex;
            FIntPoint Blocking(0,0);
            bool bRevisitBlocker=false;
            if(Display.bHasTarget&&ProbeIndex%2==0)
            {
                bRevisitBlocker=Map.WalkabilityAt(Display.Viewer,&Blocking)==EOccupancy::Occupied;
                if(!bRevisitBlocker&&!Display.Route.bComplete)
                {
                    // After furniture moves, clearing only the start can leave
                    // it surrounded by old hits. Recheck the blocked frontier
                    // as well; the exploration sweep starts 30cm away and
                    // otherwise never revisits these immediate neighbours.
                    const FVector End=Display.Route.Points.IsEmpty()?Display.Viewer:Display.Route.Points.Last().Position;
                    const FIntPoint K=FMapSnapshot::Key(End);
                    static const FIntPoint Neighbours[]={{1,0},{1,1},{0,1},{-1,1},{-1,0},{-1,-1},{0,-1},{1,-1}};
                    for(int32 I=0;I<8;++I)
                    {
                        Blocking=K+Neighbours[(I+ProbeIndex/2)%8];
                        if(Map.Walkability(Blocking)==EOccupancy::Occupied){bRevisitBlocker=true;break;}
                    }
                }
            }
            if(bRevisitBlocker)
            {
                // Revisit the actual blocker so looking down and then up can
                // positively disprove it. No start-cell clearance is assumed.
                Probe=FMapSnapshot::Center(Blocking,Map.Floor);
            }
            else if(Display.Route.Points.Num()>1&&ProbeIndex%4!=0)
            {
                int32 Nearest=0;float Best=FLT_MAX;
                for(int32 I=0;I<Display.Route.Points.Num();++I){float D=FVector::DistSquared2D(Display.Viewer,Display.Route.Points[I].Position);if(D<Best){Best=D;Nearest=I;}}
                const int32 Index=FMath::Min(Nearest+(ProbeIndex%30),Display.Route.Points.Num()-1);
                Probe=Display.Route.Points[Index].Position;
                // Prioritize the route itself; surrounding space is sampled below.
            }
            else
            {
                const float A=Display.Orientation.Rotator().Yaw*UE_PI/180+((ProbeIndex*17)%101-50)*UE_PI/180;
                const float R=.3f+((ProbeIndex*11)%35)*CellSize;
                Probe=Display.Viewer+FVector(FMath::Cos(A)*R,FMath::Sin(A)*R,0);
            }
            Probe=FMapSnapshot::Center(FMapSnapshot::Key(Probe),Map.Floor);
        }
        // Positive depth behind each standing-column sample proves the ray
        // passed it. No-hit, occluded, and outside-FOV results prove nothing.
        static const float Heights[]={0,.15f,.55f,1.1f,StandingHeight};
        const int32 Phase=ProbePhase;
        ProbePhase=(ProbePhase+1)%UE_ARRAY_COUNT(Heights);
        const FIntPoint Key=FMapSnapshot::Key(Probe);
        const FColumnEvidence* Previous=ClearanceColumns.Find(Key);
        const bool Supported=Previous&&Previous->Floor.bFloor&&Now-Previous->Seen[0]<=5;
        const float SampleFloor=Phase>0&&Supported?Previous->Floor.Floor:Map.Floor;
        const FVector Target(Probe.X,Probe.Y,SampleFloor+Heights[Phase]);
        const FVector Direction=(Target-Display.Viewer).GetSafeNormal();
        FSurfaceHit Hit;
        const double Begin=FPlatformTime::Seconds();
        const auto Result=QueryDepth(Display.Viewer,Direction,8,Hit);
        QueryAverage=FMath::Lerp(QueryAverage,float((FPlatformTime::Seconds()-Begin)*1000),.1f);
        if(Result!=EDepthResult::Unavailable)LastDepth=Now;
        // Misses never clear/refresh evidence. Keep other recent positive
        // samples so a later camera view can complete this standing column.
        if(Result!=EDepthResult::Hit)continue;
        LastLiveHit=Now;
        const float HitDistance=FVector::Dist(Hit.Point,Display.Viewer),TargetDistance=FVector::Dist(Target,Display.Viewer);
        const bool SameFloor=Hit.Normal.Z>=.85f&&FMath::Abs(Hit.Point.Z-Map.Floor)<=MaxStep;
        if(!SameFloor&&Hit.Point.Z>Map.Floor+.1f&&Hit.Point.Z<Map.Floor+StandingHeight)
        {
            FCell Block;Block.Floor=Map.Floor;Block.Occupancy=EOccupancy::Occupied;Block.Evidence=EEvidence::Depth;Block.ObservedAt=Now;
            Block.ObservedHeight=Hit.Point.Z;Block.SurfaceNormalZ=Hit.Normal.Z;
            Block.ObserverOffset=FVector3f(Hit.Point-Display.Viewer);Block.ObserverEyeHeight=Display.Viewer.Z-Map.Floor;
            Fuse(Block,Hit.Point);
        }
        if(Phase==0)
        {
            // A table/arm intercepted before the requested floor column is an
            // occluder, not evidence that this column's FLOOR changed height.
            // It was already fused as a temporary obstacle above. Only a hit
            // at the intended column can establish a persistent level change.
            if(FVector::Dist2D(Hit.Point,Probe)>.09f)continue;
            if(Hit.Normal.Z<.85f)continue;
            if(FMath::Abs(Hit.Point.Z-Map.Floor)>MaxStep)
            {
                FCell Level;Level.Floor=Hit.Point.Z;Level.bFloor=true;Level.Evidence=EEvidence::Depth;
                Level.Occupancy=EOccupancy::Unsupported;Level.ObservedAt=Now;Level.ObservedHeight=Hit.Point.Z;Level.SurfaceNormalZ=Hit.Normal.Z;
                Level.ObserverOffset=FVector3f(Hit.Point-Display.Viewer);Level.ObserverEyeHeight=Display.Viewer.Z-Map.Floor;
                Fuse(Level,Hit.Point);continue;
            }
        }
        else if(!Supported||HitDistance<TargetDistance+.08f)continue;
        if(ClearanceColumns.Num()>=1024&&!ClearanceColumns.Contains(Key))
        {
            for(auto It=ClearanceColumns.CreateIterator();It;++It)if(Now-It->Value.LastSample>5)It.RemoveCurrent();
            if(ClearanceColumns.Num()>=1024)continue;
        }
        auto& Column=ClearanceColumns.FindOrAdd(Key);
        if(Phase==0)
        {
            if(Column.Floor.bFloor&&FMath::Abs(Column.Floor.Floor-Hit.Point.Z)>.02f)Column={};
            Column.Floor={};Column.Floor.Floor=Hit.Point.Z;Column.Floor.bFloor=true;Column.Floor.Evidence=EEvidence::Depth;
        }
        else if(!Column.Floor.bFloor||FMath::Abs(Column.Floor.Floor-SampleFloor)>.02f)continue;
        Column.Seen[Phase]=Now;Column.LastSample=Now;
        bool Complete=Column.Floor.bFloor;
        for(double Seen:Column.Seen)Complete&=Now-Seen<=5;
        if(Complete)
        {
            FCell Clear=Column.Floor;Clear.bClearance=true;Clear.Occupancy=EOccupancy::Free;Clear.ObservedAt=Now;
            // Each vote requires a new, independently completed observation set.
            ClearanceColumns.Remove(Key);++CompletedClearanceChecks;Fuse(Clear,Probe);
        }
    }
}
void UWallhackNavigationSubsystem::Tick(float Dt)
{
    if(!bActive)return;Now+=FMath::Max(0.f,Dt);Map.Now=Now;UpdatePose(Dt);
    if(Dt>0&&Dt<.25f)FPSAverage=FPSAverage<=0?1/Dt:FMath::Lerp(FPSAverage,1/Dt,.1f);
    if(!bProvidersStarted)return;
    const double Begin=FPlatformTime::Seconds(),Deadline=Begin+.001;
    if(!bSceneLoaded&&!bCaptureInProgress&&Now-LastPoll>.5)
    {
        LastPoll=Now;const auto Status=SceneProvider->Poll(Scene);
        bNeedsCapture=Status==ESceneStatus::Missing;
        if(Status==ESceneStatus::Ready&&Scene.Bounds.IsValid)
        {
            Scene.LoadedAt=Now;bSceneLoaded=true;bSeeding=true;
            SeedX=SeedMinX=FMath::FloorToInt(Scene.Bounds.Min.X/CellSize)-5;SeedY=FMath::FloorToInt(Scene.Bounds.Min.Y/CellSize)-5;
            SeedMaxX=FMath::CeilToInt(Scene.Bounds.Max.X/CellSize)+5;SeedMaxY=FMath::CeilToInt(Scene.Bounds.Max.Y/CellSize)+5;
            float Best=FLT_MAX;
            for(const auto& F:Scene.Floors){float D=FMath::Abs(Display.Viewer.Z-1.6f-F.Z);if(D<Best){Best=D;Map.Floor=F.Z;}}
        }
    }
    int32 SeedBudget=256;
    while(bSeeding&&SeedBudget-->0&&FPlatformTime::Seconds()<Deadline)
    {
        const FVector P=FMapSnapshot::Center({SeedX,SeedY},Map.Floor);FCell C;
        if(Scene.Sample(P,C)){if(FMath::Abs(C.Floor-Map.Floor)>MaxStep)C.Occupancy=EOccupancy::Unsupported;Fuse(C,P);}
        if(++SeedX>SeedMaxX){SeedX=SeedMinX;if(++SeedY>SeedMaxY)bSeeding=false;}
    }
    // Depth is in MRUK world coordinates. Until the scan establishes the floor
    // reference, a valid floor hit could otherwise become a persistent step.
    if(bSceneLoaded&&!bSeeding&&StableSeconds>.5f)SampleDepth(Deadline);
    MappingAverage=FMath::Lerp(MappingAverage,float((FPlatformTime::Seconds()-Begin)*1000),.1f);
    Display.bDepthAvailable=Now-LastDepth<2&&Now-LastLiveHit<2;
    if(!bPose)Display.Tracking=TEXT("TRACKING LOST");
    else if(bCaptureInProgress)Display.Tracking=TEXT("FINISH ROOM SETUP");
    else if(bNeedsCapture)Display.Tracking=TEXT("NO ROOM / TRIGGER TO SCAN");
    else if(!bSceneLoaded||bSeeding)Display.Tracking=TEXT("LOADING ROOM");
    else if(!Display.bGuidance)Display.Tracking=TEXT("LOCALIZING / LOOK AROUND");
    else if(!Display.bDepthAvailable)Display.Tracking=TEXT("LIVE CHECKS UNAVAILABLE / CACHED MAP");
    else Display.Tracking=TEXT("ROOM LOCK / LIVE CHECKS");
    UpdateAim();UpdateRoute();
    if(Now-LastMetrics>=.2){LastMetrics=Now;UpdateMetrics();}
    if(Now-LastLog>=5)
    {
        LastLog=Now;UE_LOG(LogTemp,Display,TEXT("Wallhack navigation perf: mapping_ms=%.3f query_ms=%.3f planner_ms=%.3f tiles=%d state=%s live=%d tracking=%s aiming=%d preview=%d aim_status=%s"),Display.MappingMs,Display.QueryMs,Display.Route.PlannerMs,Map.Tiles.Num(),StateLabel(Display.State),Display.bDepthAvailable,*Display.Tracking,Display.bAiming,Display.bPreviewValid,*Display.AimStatus);
        UE_LOG(LogTemp,Display,TEXT("Wallhack navigation wearer: self_occluded_queries=%u tracked_hands=%d eye_height=%.3f"),WearerOccludedQueries,WearerMask.TrackedHands.Num(),Display.Viewer.Z-Map.Floor);
        WearerOccludedQueries=0;
        UE_LOG(LogTemp,Display,TEXT("Wallhack navigation memory: revision=%llu outline_edges=%d outline_worker_ms=%.3f"),Map.Revision,MapOutline.Num(),OutlineMs);
        UE_LOG(LogTemp,Display,TEXT("Wallhack navigation clearance: partial_columns=%d completed=%u"),ClearanceColumns.Num(),CompletedClearanceChecks);
        CompletedClearanceChecks=0;
        if(Display.bHasTarget)
        {
            FIntPoint Blocking(0,0);const auto StartState=Map.WalkabilityAt(Display.Viewer,&Blocking);
            const FCell* Cell=StartState==EOccupancy::Occupied?Map.Find(Blocking):nullptr;
            UE_LOG(LogTemp,Display,TEXT("Wallhack navigation start: viewer=(%.2f,%.2f,%.2f) floor=%.3f walk=%d blocker=(%d,%d) source=%d occupancy=%d cell_floor=%.3f observed_z=%.3f normal_z=%.3f age=%.2f points=%d complete=%d expanded=%d"),
                Display.Viewer.X,Display.Viewer.Y,Display.Viewer.Z,Map.Floor,int32(StartState),Blocking.X,Blocking.Y,
                Cell?int32(Cell->Evidence):-1,Cell?int32(Cell->Occupancy):-1,Cell?Cell->Floor:0,Cell?Cell->ObservedHeight:0,
                Cell?Cell->SurfaceNormalZ:0,Cell?Now-Cell->ObservedAt:-1,Display.Route.Points.Num(),Display.Route.bComplete,Display.Route.Expanded);
            if(Cell)UE_LOG(LogTemp,Display,TEXT("Wallhack navigation blocker origin: hit_offset=(%.3f,%.3f,%.3f) hit_eye=%.3f clear_votes=%d"),
                Cell->ObserverOffset.X,Cell->ObserverOffset.Y,Cell->ObserverOffset.Z,Cell->ObserverEyeHeight,Cell->ClearVotes);
        }
    }
}
void UWallhackNavigationSubsystem::UpdateRoute()
{
    if(!Display.bHasTarget)return;
    if(Display.bGuidance&&Display.Target.PersonId!=INDEX_NONE&&FMath::Abs(Display.Target.Standing.Z-Map.Floor)>MaxStep)
    {
        // A supplied person pose on another level must never become a route
        // to its projection onto this floor. Manual floor placement stays local.
        Display.Route={};Display.State=ERouteState::Incomplete;bNeedsPlan=true;
        if(Renderer)Renderer->InvalidateTrail();
        return;
    }
    // Furniture/feet behind the wearer cannot invalidate the remaining route.
    TrimTraversedRoute(Display.Route,Display.Viewer,&Map);
    int32 Blocked=0;
    if(!ValidateRoute(Map,Display.Route,&Blocked))
    {
        // Suppress from the first unsafe cell immediately; preserve only the
        // reachable prefix while the asynchronous replacement is running.
        Display.Route.Points.SetNum(FMath::Max(0,Blocked));Display.Route.bComplete=false;
        if(Renderer)Renderer->InvalidateTrail();
        Display.State=ERouteState::Blocked;bNeedsPlan=true;
    }
    // Cached floor stays mapped but is visibly estimated until refreshed.
    for(auto& P:Display.Route.Points)if(!P.bEstimated&&Map.IsEstimated(FMapSnapshot::Key(P.Position)))
    {P.bEstimated=true;bNeedsPlan=true;if(Renderer)Renderer->InvalidateTrail();}
    if(!Display.bGuidance)return;
    FVector Start=Display.Viewer;Start.Z=Map.Floor;
    float Deviation=FLT_MAX;
    for(const auto& P:Display.Route.Points)Deviation=FMath::Min(Deviation,float(FVector::Dist2D(Start,P.Position)));
    if(Deviation>.5f)bNeedsPlan=true;
    if(Pending.IsValid()&&Pending.IsReady())
    {
        FRoute New=Pending.Consume();
        TrimTraversedRoute(New,Display.Viewer,&Map);
        if(PendingDestination==DestinationVersion&&ValidateRoute(Map,New))
        {
            for(auto& P:New.Points)if(Map.IsEstimated(FMapSnapshot::Key(P.Position)))P.bEstimated=true;
            const bool Prefer=!Display.Route.bComplete||Deviation>.5f||New.EstimatedMeters<Display.Route.EstimatedMeters-.2f
                || New.Length()<Display.Route.Length()*.9f||Display.State==ERouteState::Relocalizing||Display.State==ERouteState::Planning;
            if(Prefer)Display.Route=MoveTemp(New);
            Display.State=Display.Route.bStartBlocked?ERouteState::StartBlocked:!Display.Route.bComplete?ERouteState::Incomplete:Display.Route.EstimatedMeters>0?ERouteState::Estimated:ERouteState::Mapped;
            // Changes during a job require a fresh job, even when this route was valid.
            bNeedsPlan=Map.Revision!=LastPlanRevision;
        }
        else bNeedsPlan=true;
    }
    if(bNeedsPlan&&!Pending.IsValid()&&Now-LastPlan>=.5)
    {
        LastPlan=Now;LastPlanRevision=Map.Revision;PendingDestination=DestinationVersion;bNeedsPlan=false;
        FMapSnapshot Snapshot=Map;const FVector Goal=Display.Target.Standing;
        Pending=Async(EAsyncExecution::ThreadPool,[Snapshot=MoveTemp(Snapshot),Start,Goal](){return Plan(Snapshot,Start,Goal);});
    }
    if(Display.Route.bComplete&&HasArrived(Map,Display.Target,Start))Display.State=ERouteState::Arrived;
}
void UWallhackNavigationSubsystem::UpdateMetrics()
{
    if(PendingOutline.IsValid()&&PendingOutline.IsReady())
    {
        auto Result=PendingOutline.Consume();
        if(Result.Revision>=OutlineRevision)
        {MapOutline=MoveTemp(Result.Edges);OutlineRevision=Result.Revision;OutlineMs=Result.Milliseconds;}
    }
    if(!bSeeding&&OutlineRevision!=Map.Revision&&!PendingOutline.IsValid())
    {
        FMapSnapshot Snapshot=Map;
        PendingOutline=Async(EAsyncExecution::ThreadPool,[Snapshot=MoveTemp(Snapshot)]()
        {
            const double Begin=FPlatformTime::Seconds();
            FOutlineResult Result;Result.Revision=Snapshot.Revision;Result.Edges=BuildMapOutline(Snapshot);
            Result.Milliseconds=(FPlatformTime::Seconds()-Begin)*1000;return Result;
        });
    }
    Display.MappingMs=MappingAverage;Display.QueryMs=QueryAverage;Display.FPS=FPSAverage;Display.Battery=FPlatformMisc::GetBatteryLevel();
    Display.ObservationAge=LastLiveHit>=0?Now-LastLiveHit:(bSceneLoaded?Now-Scene.LoadedAt:-1);
    Display.CellCount=Map.Tiles.Num()*TileSize*TileSize;
    if(!Display.bHasTarget)return;
    const FVector Delta=Display.Target.Surface-Display.Viewer;
    Display.Range=Delta.Size();Display.Height=Delta.Z;
    Display.Bearing=FMath::UnwindDegrees(FMath::RadiansToDegrees(FMath::Atan2(Delta.Y,Delta.X))-Display.Orientation.Rotator().Yaw);
    int32 Closest=0;float Best=FLT_MAX;
    for(int32 I=0;I<Display.Route.Points.Num();++I){float D=FVector::DistSquared2D(Display.Viewer,Display.Route.Points[I].Position);if(D<Best){Best=D;Closest=I;}}
    Display.Walking=0;Display.Route.ObservedMeters=0;Display.Route.EstimatedMeters=0;Display.NextTurn=TEXT("--");
    for(int32 I=Closest+1;I<Display.Route.Points.Num();++I)
    {
        const auto& A=Display.Route.Points[I-1];const auto& B=Display.Route.Points[I];
        const float D=FVector::Dist(A.Position,B.Position);Display.Walking+=D;
        (A.bEstimated||B.bEstimated?Display.Route.EstimatedMeters:Display.Route.ObservedMeters)+=D;
        if(Display.NextTurn==TEXT("--")&&!A.bEstimated&&!B.bEstimated)
        {
            // Curve samples are uneven: measure lookahead in metres.
            int32 Ahead=I;float Lookahead=0;
            while(Ahead+1<Display.Route.Points.Num()&&Lookahead<.8f&&!Display.Route.Points[Ahead+1].bEstimated)
            {Lookahead+=FVector::Dist2D(Display.Route.Points[Ahead].Position,Display.Route.Points[Ahead+1].Position);++Ahead;}
            const FVector Before=(B.Position-A.Position).GetSafeNormal(),After=(Display.Route.Points[Ahead].Position-B.Position).GetSafeNormal();
            const float Cross=FVector::CrossProduct(Before,After).Z;
            if(FMath::Abs(Cross)>.6f)Display.NextTurn=FString::Printf(TEXT("%s %.1f m"),Cross>0?TEXT("RIGHT"):TEXT("LEFT"),Display.Walking);
        }
    }
    if(Display.State==ERouteState::Mapped&&Display.Route.EstimatedMeters>0)Display.State=ERouteState::Estimated;
}
bool UWallhackNavigationSubsystem::SetDestination(const FVector& Surface,const FVector& Standing)
{
    if(!Display.bGuidance||Surface.ContainsNaN()||Standing.ContainsNaN()||Map.WalkabilityAt(Standing)!=EOccupancy::Free)return false;
    Display.Target={Surface,Standing};Display.bHasTarget=true;Display.Route={};Display.State=ERouteState::Planning;bNeedsPlan=true;++DestinationVersion;LastPlan=-100;
    return true;
}
bool UWallhackNavigationSubsystem::SetPersonDestination(int32 PersonId,const FVector& Feet)
{
    if(PersonId<=0||Feet.ContainsNaN())return false;
    if(Display.bHasTarget&&Display.Target.PersonId==PersonId&&Display.Target.Standing.Equals(Feet,.001))return true;
    const bool NewPerson=Display.Target.PersonId!=PersonId;
    Display.Target={Feet,Feet,PersonId};Display.bHasTarget=true;Display.Route={};
    Display.State=Display.bGuidance?ERouteState::Planning:ERouteState::Relocalizing;
    bNeedsPlan=true;++DestinationVersion;
    if(NewPerson)LastPlan=-100;
    // Clear the old route even if the new feet position is currently blocked.
    // The normal planner returns a reachable prefix and never bypasses walls.
    // Retain this target during localization; plan once fresh observations return.
    if(Renderer)Renderer->InvalidateTrail();
    UpdateMetrics();
    UE_LOG(LogTemp,Display,TEXT("Wallhack navigation: person target id=%d feet=%s"),PersonId,*Feet.ToString());
    return true;
}
void UWallhackNavigationSubsystem::CancelNavigation()
{ Display.bHasTarget=false;Display.Target={};Display.Route={};Display.State=ERouteState::Idle;bNeedsPlan=false;++DestinationVersion;if(Renderer)Renderer->InvalidateTrail(); }
void UWallhackNavigationSubsystem::BeginAim(){Display.bAiming=true;}
void UWallhackNavigationSubsystem::EndAim(){Display.bAiming=false;Display.bAimTracked=false;Display.bPreviewValid=false;Display.AimStatus=TEXT("NOT AIMING");}
FString UWallhackNavigationSubsystem::GetInteractionHint() const
{
    if(bPermissionDenied)return TEXT("ALLOW ROOM PERMISSION IN SETTINGS");
    if(!bProvidersStarted)return TEXT("ACCEPT ROOM PERMISSION");
    if(bCaptureInProgress)return TEXT("FINISH AND SAVE ROOM SETUP");
    if(bNeedsCapture)return TEXT("PRESS INDEX TRIGGER / SET UP ROOM");
    if(!Display.bGuidance)return Display.Tracking;
    if(Display.bAiming)return Display.bPreviewValid?TEXT("INDEX TRIGGER / NAVIGATE TO TARGET"):Display.AimStatus;
    if(Display.bHasTarget)return TEXT("HOLD GRIP / CHANGE TARGET / A CANCELS");
    return TEXT("HOLD SIDE GRIP / POINT / INDEX TRIGGER");
}
void UWallhackNavigationSubsystem::Confirm()
{
    if(bCaptureInProgress)return;
    // Setup takes priority over aiming: holding the grip must not trap a new
    // wearer in endpoint selection before any room exists.
    if(bNeedsCapture&&SceneProvider)
    {
        if(SceneProvider->RequestScan())
        {
            bCaptureInProgress=true;bNeedsCapture=false;EndAim();HideGuidance();
            Display.Tracking=TEXT("FINISH ROOM SETUP");
            UE_LOG(LogTemp,Display,TEXT("Wallhack navigation: room setup requested"));
        }
        return;
    }
    if(Display.bAiming){if(Display.bPreviewValid)SetDestination(Display.Preview.Surface,Display.Preview.Standing);}
}
void UWallhackNavigationSubsystem::SetHidden(bool H)
{
    Display.bHidden=H;
    if(Renderer)Renderer->SetActorHiddenInGame(H||!Display.bGuidance);
    if(auto* People=GetWorld()->GetSubsystem<UWallhackPeopleSubsystem>()) People->RefreshPresentation();
}
bool UWallhackNavigationSubsystem::IsSceneOccluded(FVector Origin,FVector Point) const
{
    const FVector Direction=(Point-Origin).GetSafeNormal();float Remaining=FVector::Dist(Origin,Point)-.06f;
    for(int32 I=0;I<16&&Remaining>0;++I)
    {
        FSurfaceHit Hit;if(!Scene.Raycast(Origin,Direction,Remaining,Hit))return false;
        const auto* Cell=Map.Find(FMapSnapshot::Key(Hit.Point));
        if(!Cell||Cell->Evidence!=EEvidence::Depth||Map.State(FMapSnapshot::Key(Hit.Point))!=EOccupancy::Free)return true;
        // The fused map has positive clearance that supersedes this cached shape.
        const float Advance=FVector::Dist(Origin,Hit.Point)+.11f;Origin+=Direction*Advance;Remaining-=Advance;
    }
    return Remaining>0;
}
void UWallhackNavigationSubsystem::UpdateAim()
{
    Display.bPreviewValid=false;Display.bAimTracked=false;
    Display.AimStatus=Display.bAiming?TEXT("WAITING FOR ROOM / TRACKING"):TEXT("NOT AIMING");
    if(!Display.bAiming||!Display.bGuidance)return;
    FVector O=Display.Viewer,D=Display.Orientation.GetForwardVector();
    if(auto* P=Cast<AWallhackVRPawn>(UGameplayStatics::GetPlayerPawn(this,0)))
    { FVector WorldOrigin,Direction;if(!P->GetNavigationAim(WorldOrigin,Direction)){Display.AimStatus=TEXT("KEEP RIGHT CONTROLLER IN VIEW");return;}O=WorldOrigin/GetWorld()->GetWorldSettings()->WorldToMeters;D=Direction; }
    Display.bAimTracked=true;Display.AimStatus=TEXT("POINT WITHIN 4 m / FLOOR OR OBJECT");
    // A near-horizontal ray used to select floors up to eight metres away.
    // Keep marking local and always respect the nearest scanned/live surface.
    constexpr float AimRange=4;
    Display.AimOrigin=O;Display.AimEnd=O+D*AimRange;
    FSurfaceHit H,Live;bool Hit=SceneProvider&&SceneProvider->Raycast(O,D,AimRange,H);
    if(QueryDepth(O,D,AimRange,Live)==EDepthResult::Hit&&(!Hit||FVector::Dist(O,Live.Point)<FVector::Dist(O,H.Point))) {H=Live;Hit=true;}
    if(!Hit)return;Display.AimEnd=H.Point;
    FVector Standing;
    if(SelectStanding(Map,H.Point,H.bFloor&&FMath::Abs(H.Point.Z-Map.Floor)<MaxStep,Display.Viewer,Standing))
    {
        // A stand-off can be walkable in a different room yet hidden behind the
        // selected object or a wall. Require its preview to be visible here.
        if(IsSceneOccluded(Display.Viewer,Standing+FVector(0,0,.08)))
        {Display.AimStatus=TEXT("POINT AT VISIBLE FLOOR BESIDE OBJECT");return;}
        Display.Preview.Surface=H.Point;Display.Preview.Standing=Standing;Display.bPreviewValid=true;Display.AimStatus=TEXT("READY TO NAVIGATE");
    }
    else if(H.bFloor&&FMath::Abs(H.Point.Z-Map.Floor)>=MaxStep)Display.AimStatus=TEXT("CHOOSE FLOOR ON THIS LEVEL");
    else if(Map.WalkabilityAt(H.Point)==EOccupancy::Unknown)Display.AimStatus=TEXT("LOOK AROUND TO CHECK FLOOR CLEARANCE");
    else Display.AimStatus=TEXT("CHOOSE FLOOR FARTHER FROM OBSTACLES");
}
