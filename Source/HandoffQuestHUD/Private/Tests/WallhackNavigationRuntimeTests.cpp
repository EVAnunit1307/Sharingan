#include "WallhackRuntimeTestWorld.h"
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR && !PLATFORM_ANDROID
#include "WallhackNavigationSubsystem.h"
#include "WallhackNavigationRenderer.h"
#include "WallhackPeopleSubsystem.h"
#include "WallhackPeopleRenderer.h"
#include "WallhackPeopleStyle.h"
#include "WallhackNavigationPresentation.h"
#include "StaticMeshResources.h"
#include "Engine/StaticMesh.h"
#include <limits>
#include "AssetCompilingManager.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "ImageUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Materials/Material.h"
#include "ProceduralMeshComponent.h"
#include "RenderingThread.h"

namespace
{
struct FNavigationWorld : FScopedWallhackRuntimeWorld
{
    UWallhackNavigationSubsystem* Nav=nullptr;
    TSharedPtr<WallhackNav::FDeterministicProvider> Provider;
    explicit FNavigationWorld(bool bUnmappedConnection=false,TSharedPtr<WallhackNav::FDeterministicProvider> InProvider=nullptr)
    {
        FCommandLine::Set(TEXT("-WallhackNavigationPreview -nohmd"));
        if(!World)return;
        if(Contact)Contact->SetActorHiddenInGame(true);
        Nav=World->GetSubsystem<UWallhackNavigationSubsystem>();
        Provider=InProvider?InProvider:WallhackNav::FDeterministicProvider::MakeRoom();
        if(bUnmappedConnection)
        {
            Provider->Scanned.Floors={
                {{{-3,-3},{2.5,-3},{2.5,3},{-3,3}},0},
                {{{3.5,-3},{5,-3},{5,3},{3.5,3}},0}};
            Provider->Live=Provider->Scanned; // Unknown gap has no supporting samples.
        }
        Nav->SetProviders(Provider,Provider);Nav->Start();
        Step(240);
    }
    void Step(int32 Count)
    {
        for(int32 I=0;I<Count;++I)
        {
            if(Controller)Controller->UpdateCameraManager(.016f);
            Nav->Tick(.016f);
            if(Nav->GetDisplaySnapshot().bHasTarget)FPlatformProcess::Sleep(.001f);
        }
    }
    AWallhackVRHUDActor* NavigationHUD()
    {
        HUD=World->SpawnActor<AWallhackVRHUDActor>();HUD->DispatchBeginPlay();return HUD;
    }
    bool Action(const TCHAR* Name,ETriggerEvent Event=ETriggerEvent::Started)
    {
        if(!HUD)return false;
        auto* Input=HUD->FindComponentByClass<UEnhancedInputComponent>();if(!Input)return false;
        for(const auto& B:Input->GetActionEventBindings())if(B->GetAction()&&B->GetAction()->GetFName()==FName(*FString::Printf(TEXT("IA_%s"),Name))&&B->GetTriggerEvent()==Event)
        {B->Execute(FInputActionInstance(B->GetAction()));return true;}
        return false;
    }
};
bool SaveNavigationImage(const TArray<FColor>& Pixels,int32 W,int32 H,const TCHAR* Name)
{
    const FString Dir=FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("NavigationVerification/Render"));IFileManager::Get().MakeDirectory(*Dir,true);
    TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(W,H,Pixels,PNG);
    return FFileHelper::SaveArrayToFile(PNG,*FPaths::Combine(Dir,FString(Name)+TEXT(".png")));
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavLifecycleTest,"Wallhack.Navigation.Runtime.LifecycleAndSingleTarget",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FNavLifecycleTest::RunTest(const FString&)
{
    FNavigationWorld F;if(!TestNotNull(TEXT("Navigation subsystem"),F.Nav))return false;
    TestTrue(TEXT("Seeded room and fresh observations enable guidance"),F.Nav->GetDisplaySnapshot().bGuidance);
    TestTrue(TEXT("Mapped endpoint accepted"),F.Nav->SetDestination({4,1,0},{4,1,0}));F.Step(180);
    TestTrue(TEXT("Asynchronous plan returned"),F.Nav->GetDisplaySnapshot().Route.bComplete);
    TestTrue(TEXT("New target replaces the previous target"),F.Nav->SetDestination({3,2,1},{3,2,0}));
    TestEqual(TEXT("Separate surface point retained"),F.Nav->GetDisplaySnapshot().Target.Surface.Z,1.0);
    F.Nav->Confirm();TestEqual(TEXT("Trigger outside aim keeps current target"),F.Nav->GetDisplaySnapshot().Target.Standing,FVector(3,2,0));
    F.Nav->Suspend();TestFalse(TEXT("Suspension hides guidance immediately"),F.Nav->GetDisplaySnapshot().bGuidance);
    const auto Saved=F.Nav->GetDisplaySnapshot().Target;
    F.Provider->bAvailable=false;F.Nav->Resume();F.Step(180);
    TestFalse(TEXT("Resume without new observations stays hidden"),F.Nav->GetDisplaySnapshot().bGuidance);
    TestEqual(TEXT("Brief removal preserves current target"),F.Nav->GetDisplaySnapshot().Target.Surface,Saved.Surface);
    F.Provider->bAvailable=true;F.Step(180);TestTrue(TEXT("Fresh localized observations restore guidance"),F.Nav->GetDisplaySnapshot().bGuidance);
    F.Provider->bLocalized=false;F.Step(30);
    TestFalse(TEXT("Valid headset pose cannot override lost scene-anchor localization"),F.Nav->GetDisplaySnapshot().bGuidance);
    F.Provider->bLocalized=true;F.Step(120);
    TestTrue(TEXT("Scene-anchor localization plus new observations restores guidance"),F.Nav->GetDisplaySnapshot().bGuidance);
    F.Nav->CoordinateDiscontinuity();TestFalse(TEXT("Recenter hides guidance immediately"),F.Nav->GetDisplaySnapshot().bGuidance);
    F.Step(240);TestTrue(TEXT("Recenter requires fresh localization before reusing memory"),F.Nav->GetDisplaySnapshot().bGuidance);
    TestEqual(TEXT("MRUK-frame destination never independently counter-shifted"),F.Nav->GetDisplaySnapshot().Target.Standing,Saved.Standing);
    F.Nav->CancelNavigation();TestFalse(TEXT("Cancel clears active route"),F.Nav->GetDisplaySnapshot().bHasTarget);
    F.Nav->Confirm();TestFalse(TEXT("Trigger cannot resurrect a cancelled target"),F.Nav->GetDisplaySnapshot().bHasTarget);return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavSessionMemoryTest,"Wallhack.Navigation.Runtime.MemoryAcrossLookingAwayAndRecenter",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FNavSessionMemoryTest::RunTest(const FString&)
{
    using namespace WallhackNav;
    FNavigationWorld F;auto* HUD=F.NavigationHUD();
    F.SetViewerPose({0,0,170},FRotator::ZeroRotator);F.Step(40);
    auto* People=F.World->GetSubsystem<UWallhackPeopleSubsystem>();
    const int32 PersonId=People->AddPerson({4,2,0},1.8f,90);
    F.Provider->Live.Obstacles.Add({FBox(FVector(2.6,-3,0),FVector(2.7,3,2.5)),true});
    F.Step(800);
    TArray<FIntPoint> Seen;
    const auto Memory=F.Nav->GetMap();
    for(const auto& T:Memory.Tiles)for(int32 Y=0;Y<TileSize;++Y)for(int32 X=0;X<TileSize;++X)
    {
        const FIntPoint K=T.Key*TileSize+FIntPoint(X,Y);const auto* C=Memory.Find(K);
        if(C&&C->Evidence==EEvidence::Depth&&C->Occupancy==EOccupancy::Occupied&&K.X>=25&&K.X<=27)Seen.Add(K);
    }
    if(!TestTrue(TEXT("Actual runtime depth queries discover the unscanned wall"),Seen.Num()>5))return false;
    F.Provider->bAvailable=false;F.Step(500);
    for(auto K:Seen)TestTrue(TEXT("Depth outage does not forget observed wall cells"),F.Nav->GetMap().State(K)==EOccupancy::Occupied);
    TestFalse(TEXT("Cached map is not advertised as current live checks"),F.Nav->GetDisplaySnapshot().bDepthAvailable);
    // A rapid head turn used to reset every live tile, despite MRUK retaining
    // the same world frame for both the scan and manually placed people.
    F.SetViewerPose({0,0,170},FRotator(0,180,0));F.Step(1);
    TestFalse(TEXT("Pose discontinuity suppresses guidance"),F.Nav->GetDisplaySnapshot().bGuidance);
    F.Nav->CoordinateDiscontinuity();F.Nav->Suspend();F.Nav->Resume();F.Step(120);
    for(auto K:Seen)TestTrue(TEXT("Recenter and headset removal retain anchored wall memory"),F.Nav->GetMap().State(K)==EOccupancy::Occupied);
    TestEqual(TEXT("Manual person's position stays in that same room frame"),People->GetPeople()[0].Feet,FVector(4,2,0));
    TestEqual(TEXT("Manual person's identity survives relocalization"),People->GetPeople()[0].Id,PersonId);
    F.Provider->bAvailable=true;F.Step(180);
    TestTrue(TEXT("Fresh localized observation resumes the remembered map"),F.Nav->GetDisplaySnapshot().bGuidance);
    const auto& Edges=F.Nav->GetMapOutline();int32 LiveEdges=0;
    for(const auto& E:Edges)LiveEdges+=E.bObstacle&&E.A.X>2.49&&E.A.X<2.81&&E.B.X>2.49&&E.B.X<2.81;
    TestTrue(TEXT("Minimap includes the remembered live wall absent from the room scan"),LiveEdges>0);
    HUD->Tick(.25f);F.World->SendAllEndOfFrameUpdates();FlushRenderingCommands();
    auto* RT=HUD->GetHUDRenderTarget();TArray<FColor> Pixels;RT->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
    SaveNavigationImage(Pixels,RT->SizeX,RT->SizeY,TEXT("minimap-session-memory"));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavInputTest,"Wallhack.Navigation.Runtime.ControllerActions",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FNavInputTest::RunTest(const FString&)
{
    FNavigationWorld F;F.NavigationHUD();F.SetViewerPose({0,0,170},FRotator(-35,35,0));F.Step(50);
    TestTrue(TEXT("Hold-grip action bound"),F.Action(TEXT("NavigationAim")));F.Step(30);
    TestTrue(TEXT("Aim previews a mapped standing endpoint"),F.Nav->GetDisplaySnapshot().bPreviewValid);
    TestTrue(TEXT("Trigger confirm action bound"),F.Action(TEXT("NavigationConfirm")));
    TestTrue(TEXT("Confirm starts navigation to target"),F.Nav->GetDisplaySnapshot().bHasTarget);
    TestTrue(TEXT("Grip release bound"),F.Action(TEXT("NavigationAim"),ETriggerEvent::Completed));
    TestFalse(TEXT("Release hides preview"),F.Nav->GetDisplaySnapshot().bAiming);
    F.SetHUDDensity(EWallhackHUDDensity::Hidden);TestTrue(TEXT("B also hides floor guidance"),F.Nav->GetDisplaySnapshot().bHidden);
    F.SetHUDDensity(EWallhackHUDDensity::Full);TestFalse(TEXT("Full restores floor visibility gate"),F.Nav->GetDisplaySnapshot().bHidden);
    F.Action(TEXT("NavigationCancel"));TestFalse(TEXT("A cancels navigation"),F.Nav->GetDisplaySnapshot().bHasTarget);
    TestFalse(TEXT("Waypoint cycling is not bound in navigation"),F.Action(TEXT("NavigationCycle")));
    F.Action(TEXT("NavigationConfirm"));TestFalse(TEXT("Trigger outside aiming does not recall old targets"),F.Nav->GetDisplaySnapshot().bHasTarget);return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavRoomSetupTest,"Wallhack.Navigation.Runtime.RoomSetupWhileHoldingGrip",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FNavRoomSetupTest::RunTest(const FString&)
{
    struct FScanProvider : WallhackNav::FDeterministicProvider
    {
        WallhackNav::ESceneStatus Status=WallhackNav::ESceneStatus::Missing;
        int32 Requests=0,Starts=0;
        virtual void Start() override {++Starts;}
        virtual WallhackNav::ESceneStatus Poll(WallhackNav::FScene& Scene) override {Scene=Scanned;return Status;}
        virtual bool RequestScan() override {++Requests;return true;}
    };
    auto Provider=MakeShared<FScanProvider>();const auto Geometry=WallhackNav::FDeterministicProvider::MakeRoom();
    Provider->Scanned=Geometry->Scanned;Provider->Live=Geometry->Live;
    FNavigationWorld F(false,Provider);F.NavigationHUD();const int32 InitialStarts=Provider->Starts;
    F.Action(TEXT("NavigationAim"));F.Step(2);
    TestFalse(TEXT("Missing scan cannot show a standing preview"),F.Nav->GetDisplaySnapshot().bPreviewValid);
    TestEqual(TEXT("Grip explains the missing setup instead of rejecting an endpoint"),F.Nav->GetInteractionHint(),FString(TEXT("PRESS INDEX TRIGGER / SET UP ROOM")));
    F.Action(TEXT("NavigationConfirm"));
    TestEqual(TEXT("Trigger opens room setup even while grip is held"),Provider->Requests,1);
    TestFalse(TEXT("Setup clears aiming"),F.Nav->GetDisplaySnapshot().bAiming);
    F.Step(90);F.Action(TEXT("NavigationConfirm"));
    TestEqual(TEXT("Polling and repeated trigger cannot reopen active setup"),Provider->Requests,1);
    TestEqual(TEXT("Active capture has an actionable prompt"),F.Nav->GetInteractionHint(),FString(TEXT("FINISH AND SAVE ROOM SETUP")));
    struct FCaptureResult {bool Success;};FCaptureResult Result{false};
    auto* Callback=F.Nav->FindFunctionChecked(TEXT("OnCaptureComplete"));F.Nav->ProcessEvent(Callback,&Result);
    TestEqual(TEXT("Cancelled setup can be retried"),F.Nav->GetInteractionHint(),FString(TEXT("PRESS INDEX TRIGGER / SET UP ROOM")));
    F.Action(TEXT("NavigationConfirm"));TestEqual(TEXT("Retry opens a new capture"),Provider->Requests,2);
    Provider->Status=WallhackNav::ESceneStatus::Ready;Result.Success=true;F.Nav->ProcessEvent(Callback,&Result);
    F.Step(240);
    TestEqual(TEXT("Successful capture reloads scene provider"),Provider->Starts,InitialStarts+1);
    TestTrue(TEXT("Saved room and fresh observations restore navigation"),F.Nav->GetDisplaySnapshot().bGuidance);
    TestEqual(TEXT("Ready prompt identifies the side grip and index trigger"),F.Nav->GetInteractionHint(),FString(TEXT("HOLD SIDE GRIP / POINT / INDEX TRIGGER")));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavHeldPreviewTest,"Wallhack.Navigation.Runtime.HeldPreviewAcrossDepthAging",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FNavHeldPreviewTest::RunTest(const FString&)
{
    FNavigationWorld F;F.NavigationHUD();F.SetViewerPose({0,0,170},FRotator(-35,35,0));F.Step(50);
    F.Action(TEXT("NavigationAim"));int32 MissingPreviews=0;
    // Hold a fixed clear-floor aim for 32 simulated seconds while route-priority
    // sampling visits other cells and individual live samples exceed their TTL.
    for(int32 I=0;I<2000;++I){F.Step(1);MissingPreviews+=!F.Nav->GetDisplaySnapshot().bPreviewValid;}
    TestEqual(TEXT("Confirmed scanned floor does not flicker as depth samples age"),MissingPreviews,0);
    TestTrue(TEXT("A tracked aiming ray remains available"),F.Nav->GetDisplaySnapshot().bAimTracked);
    F.Nav->Suspend();F.Step(1);
    TestFalse(TEXT("Tracking loss still removes the preview immediately"),F.Nav->GetDisplaySnapshot().bPreviewValid);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavWearerDepthTest,"Wallhack.Navigation.Runtime.WearerDepthDoesNotBlockRoute",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FNavWearerDepthTest::RunTest(const FString&)
{
    using namespace WallhackNav;
    FNavigationWorld F;F.SetViewerPose({0,0,170},FRotator(-35,0,0));F.Step(30);
    TestTrue(TEXT("Initial clear route accepted"),F.Nav->SetDestination({4,2,0},{4,2,0}));F.Step(180);
    TestTrue(TEXT("Initial route complete"),F.Nav->GetDisplaySnapshot().Route.bComplete);
    // A real ray intersection with a torso-sized surface, absent from the room
    // scan, reproduces positive depth hits immediately below the headset.
    const int32 Torso=F.Provider->Live.Obstacles.Add({FBox(FVector(-.16,-.16,.91),FVector(.16,.16,1.5)),false});
    int32 Interrupted=0;
    for(int32 I=0;I<700;++I){F.Step(1);Interrupted+=!F.Nav->GetDisplaySnapshot().Route.bComplete;}
    TestEqual(TEXT("Torso hits do not repeatedly invalidate the starting point"),Interrupted,0);
    F.Provider->Live.Obstacles.RemoveAt(Torso);
    F.Provider->Live.Obstacles.Add({FBox(FVector(.28,-.25,0),FVector(.4,.25,1.2)),false});F.Step(360);
    TestTrue(TEXT("Nearby obstacle does not inflate into a clear starting point"),F.Nav->GetDisplaySnapshot().Route.bComplete&&!F.Nav->GetDisplaySnapshot().Route.bStartBlocked);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavWearerScanTest,"Wallhack.Navigation.Runtime.WearerMaskPreservesScannedObstacles",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FNavWearerScanTest::RunTest(const FString&)
{
    using namespace WallhackNav;
    auto Provider=FDeterministicProvider::MakeRoom();
    Provider->Scanned.Obstacles.Add({FBox(FVector(-.1,-.1,.8),FVector(.1,.1,1.1)),false});
    Provider->Live=Provider->Scanned;
    FNavigationWorld F(false,Provider);F.SetViewerPose({0,0,170},FRotator(-35,0,0));F.Step(30);
    TestTrue(TEXT("Distant destination is still valid"),F.Nav->SetDestination({4,2,0},{4,2,0}));F.Step(700);
    TestTrue(TEXT("Self-occluded queries cannot erase scanned furniture under the headset"),F.Nav->GetDisplaySnapshot().Route.bStartBlocked);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavClearanceViewsTest,"Wallhack.Navigation.Runtime.ClearanceAcrossSeparateViews",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FNavClearanceViewsTest::RunTest(const FString&)
{
    using namespace WallhackNav;
    struct FViewLimitedProvider : FDeterministicProvider
    {
        bool bLookDown=true;
        virtual EDepthResult Query(FVector O,FVector D,float L,FSurfaceHit& H) override
        {
            // Floor and head-height clearance next to the wearer cannot both
            // be seen in this camera view. Turning the head changes the view.
            if((D.Z<-.2)!=bLookDown)return EDepthResult::Unknown;
            return FDeterministicProvider::Query(O,D,L,H);
        }
    };
    auto Provider=MakeShared<FViewLimitedProvider>();auto Geometry=FDeterministicProvider::MakeRoom();
    Provider->Scanned=Geometry->Scanned;Provider->Live=Geometry->Live;
    Provider->Live.Obstacles.Add({FBox(FVector(-3,-3,2.6),FVector(5,3,2.7)),true});
    // The occupied cell containing the start, whose obstruction has since gone.
    Provider->Scanned.Obstacles.Add({FBox(FVector(.049,.049,0),FVector(.051,.051,.4)),false});
    FNavigationWorld F(false,Provider);F.SetViewerPose({0,0,170},FRotator::ZeroRotator);F.Step(30);
    TestTrue(TEXT("Mapped destination accepted"),F.Nav->SetDestination({4,2,0},{4,2,0}));F.Step(180);
    TestTrue(TEXT("Floor-only view cannot clear the starting obstacle"),F.Nav->GetDisplaySnapshot().Route.bStartBlocked);
    Provider->bAvailable=false;F.Step(360);
    TestTrue(TEXT("Missing depth cannot clear the starting obstacle"),F.Nav->GetDisplaySnapshot().Route.bStartBlocked);
    Provider->bAvailable=true;Provider->bLookDown=false;F.Step(180);
    TestTrue(TEXT("New upper clearance cannot combine with expired floor evidence"),F.Nav->GetDisplaySnapshot().Route.bStartBlocked);
    for(int32 View=0;View<16;++View){Provider->bLookDown=(View%2)==0;F.Step(90);}
    TestTrue(TEXT("Repeated fresh floor and clearance observations across views restore a complete route"),F.Nav->GetDisplaySnapshot().Route.bComplete);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavFloorToleranceTest,"Wallhack.Navigation.Runtime.FloorToleranceDoesNotBecomeObstacle",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FNavFloorToleranceTest::RunTest(const FString&)
{
    using namespace WallhackNav;
    auto Provider=FDeterministicProvider::MakeRoom();
    Provider->Live.Floors[0].Z=.12f;
    FNavigationWorld F(false,Provider);F.SetViewerPose({0,0,170},FRotator(-35,0,0));F.Step(30);
    TestTrue(TEXT("Scanned endpoint remains available within same-floor tolerance"),F.Nav->SetDestination({4,2,0},{4,2,0}));F.Step(600);
    TestTrue(TEXT("Positive same-floor hits are not simultaneously inserted as obstacles"),F.Nav->GetDisplaySnapshot().Route.bComplete);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavDelayedFloorTest,"Wallhack.Navigation.Runtime.DelayedRoomDoesNotPoisonFloor",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FNavDelayedFloorTest::RunTest(const FString&)
{
    using namespace WallhackNav;
    struct FDelayedProvider : FDeterministicProvider
    {
        bool bReady=false;
        int32 DepthQueries=0;
        virtual ESceneStatus Poll(WallhackNav::FScene& Scene) override {Scene=Scanned;return bReady?ESceneStatus::Ready:ESceneStatus::Loading;}
        virtual EDepthResult Query(FVector O,FVector D,float L,FSurfaceHit& H) override
        {++DepthQueries;return FDeterministicProvider::Query(O,D,L,H);}
    };
    auto Provider=MakeShared<FDelayedProvider>();const auto Geometry=FDeterministicProvider::MakeRoom();
    Provider->Scanned=Geometry->Scanned;Provider->Live=Geometry->Live;
    for(auto& Floor:Provider->Scanned.Floors)Floor.Z=-.3f;
    for(auto& Floor:Provider->Live.Floors)Floor.Z=-.3f;
    FNavigationWorld F(false,Provider);
    TestFalse(TEXT("Guidance waits for the asynchronous room"),F.Nav->GetDisplaySnapshot().bGuidance);
    TestEqual(TEXT("Depth is not fused against an uninitialized floor reference"),Provider->DepthQueries,0);
    Provider->bReady=true;F.Step(240);
    TestTrue(TEXT("Depth starts after the floor frame is established"),Provider->DepthQueries>0);
    TestTrue(TEXT("Clear endpoint remains usable after delayed room loading"),F.Nav->SetDestination({4,2,-.3},{4,2,-.3}));F.Step(180);
    TestTrue(TEXT("Early depth cannot leave persistent false level changes on the loaded floor"),F.Nav->GetDisplaySnapshot().Route.bComplete);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavRenderTest,"Wallhack.Navigation.Render.ActualHUDAndFloorTrail",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FNavRenderTest::RunTest(const FString&)
{
    FNavigationWorld F(true);auto* HUD=F.NavigationHUD();
    TestTrue(TEXT("Render destination accepted"),F.Nav->SetDestination({4,2,0},{4,2,0}));F.Step(180);
    TestTrue(TEXT("Render route includes an explicitly estimated connection"),F.Nav->GetDisplaySnapshot().Route.EstimatedMeters>.5f);
    FAssetCompilingManager::Get().FinishAllCompilation();
    auto ReadHUD=[&](const TCHAR* Name)
    {
        HUD->Tick(.016f);FlushRenderingCommands();TArray<FColor> Pixels;
        auto* RT=HUD->GetHUDRenderTarget();RT->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
        SaveNavigationImage(Pixels,RT->SizeX,RT->SizeY,Name);return Pixels;
    };
    auto Minimal=ReadHUD(TEXT("navigation-minimal"));
    int32 CenterInk=0,Details=0,DirectionInk=0;
    for(int32 Y=0;Y<1152;++Y)for(int32 X=0;X<2048;++X)
    {if(Minimal[Y*2048+X].A>30){if(X>800&&X<1200&&Y>300&&Y<800)++CenterInk;if(X>1250&&Y>770&&Y<960)++Details;if(X>820&&X<1220&&Y>830&&Y<980)++DirectionInk;}}
    TestEqual(TEXT("Center is unobstructed"),CenterInk,0);TestTrue(TEXT("Readable target/route metrics rendered"),Details>500);
    TestTrue(TEXT("Lower-center direction cue is visible without looking at floor"),DirectionInk>700);
    F.SetHUDDensity(EWallhackHUDDensity::Full);ReadHUD(TEXT("navigation-full"));
    F.SetHUDDensity(EWallhackHUDDensity::Hidden);const auto Hidden=ReadHUD(TEXT("navigation-hidden"));
    int32 Alpha=0;for(auto P:Hidden)Alpha+=P.A>0;TestEqual(TEXT("Hidden compositor is fully transparent"),Alpha,0);
    F.SetHUDDensity(EWallhackHUDDensity::Minimal);
    auto* Camera=F.World->SpawnActor<ASceneCapture2D>();auto* Capture=Camera->GetCaptureComponent2D();
    Capture->bCaptureEveryFrame=false;Capture->bCaptureOnMovement=false;Capture->CaptureSource=SCS_SceneColorHDR;
    Capture->PrimitiveRenderMode=ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
    for(TActorIterator<AWallhackNavigationRenderer> It(F.World);It;++It){It->Tick(.2);Capture->ShowOnlyActorComponents(*It);}
    Capture->SetWorldLocationAndRotation(FVector(0,0,170),FRotator(-25,10,0));Capture->FOVAngle=90;
    auto* RT=NewObject<UTextureRenderTarget2D>(Camera);RT->RenderTargetFormat=RTF_RGBA16f;RT->ClearColor=FLinearColor::Black;
    RT->InitAutoFormat(1024,576);RT->UpdateResourceImmediate(true);Capture->TextureTarget=RT;
    for(int32 I=0;I<3;++I){Capture->CaptureScene();FlushRenderingCommands();FAssetCompilingManager::Get().FinishAllCompilation();}
    TArray<FColor> Pixels;RT->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
    int32 Mint=0,Amber=0;for(auto P:Pixels){Mint+=P.G>P.R*1.15&&P.G>20;Amber+=P.R>P.G*1.15&&P.G>P.B*1.1&&P.R>20;}
    TestTrue(TEXT("Actual world mesh produces prominent mint chevrons on GPU"),Mint>100);
    TestTrue(TEXT("Actual world mesh produces amber estimated guidance on GPU"),Amber>10);
    SaveNavigationImage(Pixels,1024,576,TEXT("navigation-floor"));
    Capture->SetWorldLocationAndRotation(FVector(210,70,500),FRotator(-90,0,0));
    Capture->CaptureScene();FlushRenderingCommands();
    RT->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
    SaveNavigationImage(Pixels,1024,576,TEXT("navigation-floor-overview"));
    Capture->SetWorldLocationAndRotation(FVector(0,0,170),FRotator(-70,10,0));
    Capture->CaptureScene();FlushRenderingCommands();RT->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
    Mint=0;for(auto P:Pixels)Mint+=P.G>P.R*1.15&&P.G>20;
    TestTrue(TEXT("Nearby arrows remain visible when looking down"),Mint>100);
    SaveNavigationImage(Pixels,1024,576,TEXT("navigation-looking-down"));return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavThroughWallsTest,"Wallhack.Navigation.Render.TrailVisibleThroughWalls",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FNavThroughWallsTest::RunTest(const FString&)
{
    FNavigationWorld F(true);
    TestTrue(TEXT("Target behind scanned furniture accepted"),F.Nav->SetDestination({4,0,0},{4,0,0}));F.Step(180);
    const auto& Display=F.Nav->GetDisplaySnapshot();
    TestTrue(TEXT("Visibility does not let the planner cross furniture"),Display.Route.bComplete&&Display.Route.Length()>4.1f);
    AWallhackNavigationRenderer* Renderer=nullptr;
    for(TActorIterator<AWallhackNavigationRenderer> It(F.World);It;++It){Renderer=*It;break;}
    if(!TestNotNull(TEXT("Actual guidance renderer exists"),Renderer))return false;
    Renderer->Tick(.2f);
    auto* Mesh=Renderer->FindComponentByClass<UProceduralMeshComponent>();
    if(!TestNotNull(TEXT("Actual trail mesh exists"),Mesh))return false;
    const auto* Material=Mesh->GetMaterial(0)?Mesh->GetMaterial(0)->GetMaterial():nullptr;
    TestTrue(TEXT("Cooked trail material bypasses scene and environment depth"),Material&&Material->bDisableDepthTest);
    const auto* Section=Mesh->GetProcMeshSection(0);
    if(!TestNotNull(TEXT("Trail mesh has geometry"),Section))return false;
    int32 Occluded=0,Suppressed=0;
    for(int32 I=0;I+3<Section->ProcVertexBuffer.Num();I+=4)
    {
        const FVector P=(Section->ProcVertexBuffer[I].Position+Section->ProcVertexBuffer[I+2].Position)/200;
        if(F.Nav->IsSceneOccluded(Display.Viewer,P))
        {
            ++Occluded;
            for(int32 J=0;J<4;++J)Suppressed+=Section->ProcVertexBuffer[I+J].Color.A==0;
        }
    }
    TestTrue(TEXT("Fixture includes guidance hidden behind scanned geometry"),Occluded>0);
    TestEqual(TEXT("Scanned geometry cannot erase through-wall guidance"),Suppressed,0);

    auto* Camera=F.World->SpawnActor<ASceneCapture2D>();auto* Capture=Camera->GetCaptureComponent2D();
    Capture->bCaptureEveryFrame=false;Capture->bCaptureOnMovement=false;Capture->CaptureSource=SCS_SceneColorHDR;
    Capture->PrimitiveRenderMode=ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
    Capture->ShowOnlyActorComponents(Renderer);
    const FVector Eye(0,0,170);const FRotator View(-25,0,0);
    Capture->SetWorldLocationAndRotation(Eye,View);Capture->FOVAngle=90;
    auto* RT=NewObject<UTextureRenderTarget2D>(Camera);RT->RenderTargetFormat=RTF_RGBA16f;RT->ClearColor=FLinearColor::Black;
    RT->InitAutoFormat(1024,576);RT->UpdateResourceImmediate(true);Capture->TextureTarget=RT;
    FAssetCompilingManager::Get().FinishAllCompilation();
    auto Read=[&](const TCHAR* Name)
    {
        for(int32 I=0;I<3;++I){Capture->CaptureScene();FlushRenderingCommands();}
        TArray<FColor> Pixels;RT->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
        SaveNavigationImage(Pixels,1024,576,Name);FIntPoint Ink(0,0);
        for(auto P:Pixels){Ink.X+=P.G>P.R*1.15&&P.G>20;Ink.Y+=P.R>P.G*1.15&&P.G>P.B*1.1&&P.R>20;}
        return Ink;
    };
    const FIntPoint Before=Read(TEXT("navigation-through-walls-baseline"));
    TestTrue(TEXT("Mapped arrows and estimated guidance are visible before the wall"),Before.X>25&&Before.Y>10);
    // An opaque slab fills the entire view between camera and floor guidance.
    // It exercises GPU depth independently of the scanned-geometry culling above.
    auto* Wall=F.World->SpawnActor<AStaticMeshActor>();auto* WallMesh=Wall->GetStaticMeshComponent();
    WallMesh->SetMobility(EComponentMobility::Movable);
    WallMesh->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube")));
    WallMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);WallMesh->SetCastShadow(false);
    Wall->SetActorLocationAndRotation(Eye+View.Vector()*20,View);Wall->SetActorScale3D({.02,100,100});
    Capture->ShowOnlyActorComponents(Wall);
    FAssetCompilingManager::Get().FinishAllCompilation();
    const FIntPoint Behind=Read(TEXT("navigation-through-walls"));
    TestTrue(TEXT("Mint trail and arrows survive the opaque depth occluder"),Behind.X>=Before.X*.9);
    TestTrue(TEXT("Amber estimates and label survive the opaque depth occluder"),Behind.Y>=Before.Y*.9);
    F.Nav->SetHidden(true);Renderer->Tick(.2f);
    const FIntPoint Hidden=Read(TEXT("navigation-through-walls-hidden"));
    TestEqual(TEXT("Hidden still removes all mint guidance"),Hidden.X,0);
    TestEqual(TEXT("Hidden still removes all estimated guidance"),Hidden.Y,0);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavPermissionTest,"Wallhack.Navigation.Runtime.PermissionDenialAndRecovery",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FNavPermissionTest::RunTest(const FString&)
{
    FNavigationWorld F;
    struct FPermissionResult {TArray<FString> Permissions;TArray<bool> Granted;};
    FPermissionResult Result{{TEXT("com.oculus.permission.USE_SCENE")},{false}};
    // Execute the actual reflected Android callback with deterministic results.
    auto* Callback=F.Nav->FindFunctionChecked(TEXT("OnPermissions"));F.Nav->ProcessEvent(Callback,&Result);
    F.Step(120);TestFalse(TEXT("Denied permission cannot silently resume guidance"),F.Nav->GetDisplaySnapshot().bGuidance);
    TestEqual(TEXT("Denial is explicit"),F.Nav->GetDisplaySnapshot().Tracking,FString(TEXT("ROOM PERMISSION DENIED")));
    Result.Granted[0]=true;F.Nav->ProcessEvent(Callback,&Result);F.Step(120);
    TestTrue(TEXT("Grant plus fresh observations restores guidance"),F.Nav->GetDisplaySnapshot().bGuidance);return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavLowerBodyTest,"Wallhack.Navigation.Runtime.LowerBodyDoesNotPoisonStart",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FNavLowerBodyTest::RunTest(const FString&)
{
    using namespace WallhackNav;
    FNavigationWorld F;F.SetViewerPose({0,0,170},FRotator(-35,0,0));F.Step(30);
    TestTrue(TEXT("Initial target accepted"),F.Nav->SetDestination({4,2,0},{4,2,0}));F.Step(180);
    const int32 Body=F.Provider->Live.Obstacles.Add({FBox(FVector(-.16,-.16,.2),FVector(.16,.16,.85)),false});
    int32 Interrupted=0;
    for(int32 I=0;I<500;++I){F.Step(1);Interrupted+=!F.Nav->GetDisplaySnapshot().Route.bComplete;}
    TestEqual(TEXT("Standing lower-body depth does not repeatedly block the start"),Interrupted,0);
    F.Provider->Live.Obstacles.RemoveAt(Body);
    F.Provider->Live.Obstacles.Add({FBox(FVector(.28,-.2,.2),FVector(.4,.2,.55)),false});F.Step(360);
    TestTrue(TEXT("Low obstacle beside the start leaves point navigation available"),F.Nav->GetDisplaySnapshot().Route.bComplete&&!F.Nav->GetDisplaySnapshot().Route.bStartBlocked);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavRaisedOccluderTest,"Wallhack.Navigation.Runtime.RaisedOccluderIsNotPermanentFloor",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FNavRaisedOccluderTest::RunTest(const FString&)
{
    using namespace WallhackNav;
    struct FDownwardProvider : FDeterministicProvider
    {
        bool bDownOnly=true;
        virtual EDepthResult Query(FVector O,FVector D,float L,FSurfaceHit& H) override
        {return bDownOnly&&D.Z>-.2?EDepthResult::Unknown:FDeterministicProvider::Query(O,D,L,H);}
    };
    auto P=MakeShared<FDownwardProvider>();auto Geometry=FDeterministicProvider::MakeRoom();
    P->Scanned=Geometry->Scanned;P->Live=P->Scanned;
    const int32 Raised=P->Live.Obstacles.Add({FBox(FVector(.7,-.25,1),FVector(1.1,.25,1.25)),false});
    FNavigationWorld F(false,P);F.SetViewerPose({0,0,170},FRotator(-35,0,0));F.Step(700);
    // The floor ray hits the raised surface well BEFORE the requested floor
    // column. Moving it away must leave obstacle memory, not a permanent step.
    P->Live.Obstacles.RemoveAt(Raised);
    F.SetViewerPose({40,5,170},FRotator(-35,0,0));F.Step(5);
    F.SetViewerPose({85,5,170},FRotator(-35,0,0));F.Step(400);
    TestTrue(TEXT("Target accepted after the transient surface moves"),F.Nav->SetDestination({4,2,0},{4,2,0}));F.Step(180);
    const auto* Start=F.Nav->GetMap().Find({8,0});
    TestTrue(TEXT("Unrelated raised hit is remembered as an obstacle, not a permanent level change"),Start&&Start->Occupancy==EOccupancy::Occupied);
    TestTrue(TEXT("A floor-only view cannot erase an unseen obstruction"),F.Nav->GetDisplaySnapshot().Route.bStartBlocked);
    P->bDownOnly=false;
    P->Live.Obstacles.Add({FBox(FVector(-3,-3,2.6),FVector(5,3,2.7)),true});F.Step(700);
    TestFalse(TEXT("Repeated full-column observations clear the moved obstacle"),F.Nav->GetDisplaySnapshot().Route.bStartBlocked);
    TestTrue(TEXT("Route recovers after positive clearance without a room rescan"),F.Nav->GetDisplaySnapshot().Route.bComplete);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavLocalAimTest,"Wallhack.Navigation.Runtime.LocalAimRespectsNearestSurface",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FNavLocalAimTest::RunTest(const FString&)
{
    using namespace WallhackNav;
    struct FScanAimProvider : FDeterministicProvider
    {
        float AimRange=0;
        virtual bool Raycast(FVector O,FVector D,float L,FSurfaceHit& Hit) override
        {AimRange=L;return Scanned.Raycast(O,D,L,Hit);}
    };
    auto P=MakeShared<FScanAimProvider>();auto Geometry=FDeterministicProvider::MakeRoom();
    P->Scanned=Geometry->Scanned;P->Live=P->Scanned;P->Live.Obstacles.Reset();
    FNavigationWorld F(false,P);F.NavigationHUD();F.SetViewerPose({0,0,170},FRotator(-27,0,0));F.Step(60);
    F.Action(TEXT("NavigationAim"));F.Step(10);const auto Near=F.Nav->GetDisplaySnapshot();
    TestEqual(TEXT("Controller selection is local to four metres"),P->AimRange,4.f);
    TestTrue(TEXT("Farther live floor cannot bypass nearer scanned object"),Near.AimEnd.X<1.6);
    TestTrue(TEXT("Object marker remains on the visible side"),Near.bPreviewValid&&Near.Preview.Standing.X<Near.Preview.Surface.X);
    F.Nav->Confirm();TestTrue(TEXT("Previewed surface is the committed surface"),F.Nav->GetDisplaySnapshot().Target.Surface.Equals(Near.Preview.Surface,.001));
    F.SetViewerPose({0,0,170},FRotator(-10,40,0));F.Step(60);
    const auto Far=F.Nav->GetDisplaySnapshot();
    TestFalse(TEXT("Shallow aim cannot mark remote room floor"),Far.bPreviewValid);
    TestTrue(TEXT("No-hit ray visibly stops at four metres"),FVector::Dist(Far.AimOrigin,Far.AimEnd)<=4.001);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPeopleInputTest,"Wallhack.People.Runtime.ManualControllerWorkflow",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FPeopleInputTest::RunTest(const FString&)
{
    FNavigationWorld F;F.NavigationHUD();
    auto* People=F.World->GetSubsystem<UWallhackPeopleSubsystem>();
    TestTrue(TEXT("Initial floor target"),F.Nav->SetDestination({4,2,0},{4,2,0}));
    F.SetViewerPose({0,0,170},FRotator(-35,35,0));F.Step(50);
    TestTrue(TEXT("Y person mode bound"),F.Action(TEXT("PeopleMode")));
    F.Action(TEXT("NavigationAim"));F.Step(30);
    const FVector First=F.Nav->GetDisplaySnapshot().Preview.Standing;
    TestTrue(TEXT("Floor-supported placement available"),F.Nav->GetDisplaySnapshot().bPreviewValid);
    F.Action(TEXT("NavigationConfirm"));
    if(!TestEqual(TEXT("Trigger places one person"),People->GetPeople().Num(),1))return false;
    TestEqual(TEXT("Feet use previewed standing point"),People->GetSelected()->Feet,First);
    TestEqual(TEXT("Placement immediately links navigation to this person"),F.Nav->GetDisplaySnapshot().Target.PersonId,People->GetSelected()->Id);
    TestEqual(TEXT("Route destination is the person's feet"),F.Nav->GetDisplaySnapshot().Target.Standing,First);
    F.Step(180);
    TestTrue(TEXT("Normal planner reaches the placed person"),F.Nav->GetDisplaySnapshot().Route.bComplete);
    const int32 RoutePoints=F.Nav->GetDisplaySnapshot().Route.Points.Num();
    People->AdjustSelected(.2f,90.f);
    TestTrue(TEXT("Height adjusts proportionally in metres"),FMath::IsNearlyEqual(People->GetSelected()->Height,1.95f));
    const float Facing=People->GetSelected()->Facing;
    TestEqual(TEXT("Height/facing edits do not clear or rebuild the route"),F.Nav->GetDisplaySnapshot().Route.Points.Num(),RoutePoints);
    F.SetViewerPose({0,0,170},FRotator(-40,-35,0));F.Step(40);
    TestTrue(TEXT("Move action bound"),F.Action(TEXT("PeopleMove")));
    TestTrue(TEXT("Left trigger moves the selected body"),People->GetSelected()->Feet.Equals(F.Nav->GetDisplaySnapshot().Preview.Standing,.001));
    TestFalse(TEXT("Moved position differs from the first point"),People->GetSelected()->Feet.Equals(First,.1));
    TestEqual(TEXT("Move retains facing"),People->GetSelected()->Facing,Facing);
    TestEqual(TEXT("Move does not add a duplicate"),People->GetPeople().Num(),1);
    TestEqual(TEXT("Move retargets guidance immediately"),F.Nav->GetDisplaySnapshot().Target.Standing,People->GetSelected()->Feet);
    TestTrue(TEXT("Route to previous feet position is suppressed"),F.Nav->GetDisplaySnapshot().Route.Points.IsEmpty());
    F.Action(TEXT("NavigationConfirm"));TestEqual(TEXT("Another trigger adds another person"),People->GetPeople().Num(),2);
    const int32 Second=People->GetSelected()->Id;
    F.Action(TEXT("PeopleSelect"));TestTrue(TEXT("Stick click cycles selected body"),People->GetSelected()->Id!=Second);
    TestEqual(TEXT("Cycling changes the navigation person"),F.Nav->GetDisplaySnapshot().Target.PersonId,People->GetSelected()->Id);
    F.Action(TEXT("NavigationCancel"));TestEqual(TEXT("A removes selected body in person mode"),People->GetPeople().Num(),1);
    TestEqual(TEXT("Deleting the target navigates to the remaining person"),F.Nav->GetDisplaySnapshot().Target.PersonId,Second);
    F.Action(TEXT("PeopleMode"));TestFalse(TEXT("Y exits edit mode"),People->IsEditing());
    F.Action(TEXT("NavigationConfirm"));TestEqual(TEXT("Trigger outside aim cannot add people"),People->GetPeople().Num(),1);
    F.Action(TEXT("NavigationCancel"));TestFalse(TEXT("A returns to navigation cancellation"),F.Nav->GetDisplaySnapshot().bHasTarget);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPeopleNavigationLifecycleTest,"Wallhack.People.Navigation.MoveCancelAndRelocalize",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FPeopleNavigationLifecycleTest::RunTest(const FString&)
{
    FNavigationWorld F;F.NavigationHUD();auto* People=F.World->GetSubsystem<UWallhackPeopleSubsystem>();
    const int32 First=People->AddPerson({4,1,0},1.75f,180);
    const int32 Second=People->AddPerson({4,2,0},1.9f,90);
    F.Action(TEXT("NavigationConfirm"));F.Step(180);
    TestEqual(TEXT("Trigger without grip navigates to selected person"),F.Nav->GetDisplaySnapshot().Target.PersonId,Second);
    TestTrue(TEXT("Person target gets a complete route"),F.Nav->GetDisplaySnapshot().Route.bComplete);
    F.Action(TEXT("PeopleSelect"));
    TestEqual(TEXT("Stick can switch target outside editing"),F.Nav->GetDisplaySnapshot().Target.PersonId,First);
    F.Step(1); // Start a worker, then move its target before consuming the answer.
    People->UpdatePerson(First,{-2,1,0},1.75f,180);F.Step(180);
    const auto& Moved=F.Nav->GetDisplaySnapshot();
    TestTrue(TEXT("Moved target route completes"),Moved.Route.bComplete);
    if(TestTrue(TEXT("Replacement path exists"),Moved.Route.Points.Num()>1))
        TestTrue(TEXT("Old worker cannot restore the former endpoint"),Moved.Route.Points.Last().Position.Equals({-2,1,0},.001));
    const FVector Linked=Moved.Target.Standing;
    People->UpdatePerson(Second,{3,2,0},2.f,250);
    TestEqual(TEXT("Editing a different person cannot steal the route"),F.Nav->GetDisplaySnapshot().Target.Standing,Linked);
    F.Nav->Suspend();People->UpdatePerson(First,{-2,-1,0},1.8f,270);
    TestFalse(TEXT("Moving while suspended cannot show guidance"),F.Nav->GetDisplaySnapshot().bGuidance);
    TestTrue(TEXT("Suspended target keeps its updated position"),F.Nav->GetDisplaySnapshot().Target.Standing.Equals({-2,-1,0},.001));
    F.Nav->Resume();F.Step(240);
    TestTrue(TEXT("Fresh localization replans to moved person"),F.Nav->GetDisplaySnapshot().Route.bComplete);
    F.Action(TEXT("NavigationCancel"));People->UpdatePerson(First,{-2,0,0},1.85f,0);F.Step(30);
    TestFalse(TEXT("Cancel detaches the person; later edits cannot restart guidance"),F.Nav->GetDisplaySnapshot().bHasTarget);
    F.Action(TEXT("NavigationConfirm"));F.Step(120);
    TestEqual(TEXT("Trigger explicitly resumes person navigation"),F.Nav->GetDisplaySnapshot().Target.PersonId,First);
    F.Nav->SetDestination({4,2,0},{4,2,0});People->UpdatePerson(First,{-2,2,0},1.85f,0);
    TestEqual(TEXT("Explicit floor targeting detaches the person"),F.Nav->GetDisplaySnapshot().Target.PersonId,INDEX_NONE);
    TestEqual(TEXT("Old person's edits do not move a floor target"),F.Nav->GetDisplaySnapshot().Target.Standing,FVector(4,2,0));
    F.Action(TEXT("PeopleMode"));F.Action(TEXT("NavigationCancel"));F.Action(TEXT("NavigationCancel"));
    TestTrue(TEXT("Deleting every person leaves no people"),People->GetPeople().IsEmpty());
    TestFalse(TEXT("Removing the final destination clears its route"),F.Nav->GetDisplaySnapshot().bHasTarget);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPeopleNavigationObstacleTest,"Wallhack.People.Navigation.ObstaclesArrivalAndFloorLimit",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FPeopleNavigationObstacleTest::RunTest(const FString&)
{
    FNavigationWorld F;auto* People=F.World->GetSubsystem<UWallhackPeopleSubsystem>();
    const int32 Id=People->AddPerson({4,0,0},1.75f,180);
    TestTrue(TEXT("Navigate to person"),People->NavigateToSelected());F.Step(180);
    const auto& D=F.Nav->GetDisplaySnapshot();
    TestTrue(TEXT("Person route detours around real furniture"),D.Route.bComplete&&D.Walking>4.1f);
    const auto Trail=WallhackNav::BuildTrail(D);
    TestTrue(TEXT("Person target feeds the existing arrow renderer"),Trail.Vertices.Num()>128);
    for(const auto& P:D.Route.Points)
        TestFalse(TEXT("Route does not cross the furniture"),P.Position.X>1.5&&P.Position.X<2.1&&FMath::Abs(P.Position.Y)<.5);
    People->UpdatePerson(Id,{1.8,0,0},1.75f,180);
    TestTrue(TEXT("Blocked relocation cannot leave the old route visible"),F.Nav->GetDisplaySnapshot().Route.Points.IsEmpty());
    F.Step(400);
    TestFalse(TEXT("Known occupied target cannot produce a complete route"),F.Nav->GetDisplaySnapshot().Route.bComplete);
    TestTrue(TEXT("Blocked target identity is retained"),F.Nav->GetDisplaySnapshot().Target.Standing.Equals({1.8,0,0},.001));
    People->UpdatePerson(Id,{4,1,3},1.75f,0);F.Step(30);
    TestTrue(TEXT("Another level cannot be projected into a same-floor route"),F.Nav->GetDisplaySnapshot().Route.Points.IsEmpty());
    TestTrue(TEXT("Other-floor route is explicitly incomplete"),F.Nav->GetDisplaySnapshot().State==WallhackNav::ERouteState::Incomplete);
    People->UpdatePerson(Id,{4,1,0},1.75f,0);F.Step(240);
    TestTrue(TEXT("Valid same-floor relocation recovers"),F.Nav->GetDisplaySnapshot().Route.bComplete);
    for(int32 I=1;I<=8;++I){F.SetViewerPose({I*45.,100,170},FRotator::ZeroRotator);F.Step(15);}
    F.Step(180);
    TestTrue(TEXT("Within 0.6 m of mapped person feet reports arrival"),F.Nav->GetDisplaySnapshot().State==WallhackNav::ERouteState::Arrived);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPeoplePoseTest,"Wallhack.People.Runtime.MetricBodyAndPoseValidation",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FPeoplePoseTest::RunTest(const FString&)
{
    FNavigationWorld F;auto* People=F.World->GetSubsystem<UWallhackPeopleSubsystem>();
    const FVector Feet(3,1,.2);const int32 Id=People->AddPerson(Feet,1.75f,0);
    auto* Renderer=People->GetRenderer();if(!TestNotNull(TEXT("Anatomical renderer"),Renderer))return false;
    auto* Body=Renderer->GetBodyComponents()[0].Get();UStaticMesh* Mesh=Body->GetStaticMesh();
    auto* Outline=Renderer->GetOutlineComponents()[0].Get();
    FAssetCompilingManager::Get().FinishAllCompilation();
    TestTrue(TEXT("Anatomical topology, not a primitive"),Mesh->GetNumVertices(0)>10000);
    TestTrue(TEXT("Quest mesh bounded to 25000 triangles"),Mesh->GetNumTriangles(0)<25000);
    const auto& V=Mesh->GetRenderData()->LODResources[0].VertexBuffers.PositionVertexBuffer;
    float FrontToe=-10000,Heel=10000;
    for(uint32 I=0;I<V.GetNumVertices();++I){const auto P=V.VertexPosition(I);if(P.Z<4){FrontToe=FMath::Max(FrontToe,P.X);Heel=FMath::Min(Heel,P.X);}}
    TestTrue(TEXT("Imported toes face +X, matching supplied yaw"),FrontToe>FMath::Abs(Heel));
    for(float Units:{100.f,50.f})for(float Height:{1.f,1.75f,2.3f})for(float Yaw:{0.f,90.f,215.f})
    {
        F.World->GetWorldSettings()->WorldToMeters=Units;
        TestTrue(TEXT("Pose update accepted"),People->UpdatePerson(Id,Feet,Height,Yaw));
        const FBox Bounds=Body->CalcBounds(Body->GetComponentTransform()).GetBox();
        TestTrue(TEXT("Feet remain at supplied floor elevation"),FMath::IsNearlyEqual(Bounds.Min.Z,Feet.Z*Units,.05));
        TestTrue(TEXT("World-space anatomical height matches metres"),FMath::IsNearlyEqual(Bounds.GetSize().Z,Height*Units,.05));
        TestTrue(TEXT("Uniform scale avoids distorted limbs"),Body->GetComponentScale().AllComponentsEqual());
        TestTrue(TEXT("Facing changes the body, not just its label"),Body->GetForwardVector().Equals(FRotator(0,Yaw,0).Vector(),.001));
        TestTrue(TEXT("Outline shares body height, facing and floor position"),Outline->GetComponentTransform().Equals(Body->GetComponentTransform()));
        const FBox BoxBounds=Outline->CalcBounds(Outline->GetComponentTransform()).GetBox();
        TestTrue(TEXT("Outline encloses the human mesh"),BoxBounds.ExpandBy(.05).IsInsideOrOn(Bounds.Min)&&BoxBounds.ExpandBy(.05).IsInsideOrOn(Bounds.Max));
    }
    TestFalse(TEXT("Invalid values cannot poison rendering"),People->UpdatePerson(Id,Feet,std::numeric_limits<float>::quiet_NaN(),0));
    TestEqual(TEXT("Invalid input cannot create a person"),People->AddPerson(Feet,1.7f,std::numeric_limits<float>::infinity()),INDEX_NONE);
    People->UpdatePerson(Id,Feet,20.f,-90.f);
    TestEqual(TEXT("Height has a physical upper limit"),People->GetSelected()->Height,2.3f);
    TestEqual(TEXT("Facing wraps"),People->GetSelected()->Facing,270.f);
    for(int32 I=1;I<8;++I)TestTrue(TEXT("Up to eight bodies supported"),People->AddPerson({double(I),0,0},1.75f,0)!=INDEX_NONE);
    TestEqual(TEXT("Bounded marker count limits transparent overdraw"),People->AddPerson({0,0,0},1.75f,0),INDEX_NONE);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPeopleLifecycleTest,"Wallhack.People.Runtime.HiddenTrackingAndSessionLifetime",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FPeopleLifecycleTest::RunTest(const FString&)
{
    {
        FNavigationWorld F;auto* People=F.World->GetSubsystem<UWallhackPeopleSubsystem>();
        People->AddPerson({2,1,0},1.8f,135);auto* Renderer=People->GetRenderer();
        if(!TestNotNull(TEXT("Renderer exists"),Renderer))return false;
        F.Nav->SetHidden(true);TestTrue(TEXT("B hides people immediately"),Renderer->IsHidden());
        F.Nav->SetHidden(false);TestFalse(TEXT("Visible density restores them"),Renderer->IsHidden());
        F.Nav->Suspend();TestTrue(TEXT("Suspension hides people immediately"),Renderer->IsHidden());
        F.Provider->bAvailable=false;F.Nav->Resume();F.Step(180);People->Tick(.016f);
        TestTrue(TEXT("No fresh geometry means no misplaced people"),Renderer->IsHidden());
        F.Provider->bAvailable=true;F.Step(180);People->Tick(.016f);
        TestFalse(TEXT("Fresh localization restores bodies"),Renderer->IsHidden());
        TestEqual(TEXT("Brief removal retains height"),People->GetSelected()->Height,1.8f);
        F.Nav->CoordinateDiscontinuity();TestTrue(TEXT("Recenter suppresses immediately"),Renderer->IsHidden());
        F.Step(240);People->Tick(.016f);TestFalse(TEXT("Relocalization restores shared-frame body"),Renderer->IsHidden());
        TestEqual(TEXT("No independent frame correction"),People->GetSelected()->Feet,FVector(2,1,0));
    }
    FNavigationWorld NewSession;
    TestEqual(TEXT("New application world has no retained people"),NewSession.World->GetSubsystem<UWallhackPeopleSubsystem>()->GetPeople().Num(),0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQuestStereoSettingsTest,"Wallhack.Stereo.QuestRendererAndOverlayPass",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FQuestStereoSettingsTest::RunTest(const FString&)
{
    for (const auto& Setting : TArray<TPair<FString,int32>>{
        {TEXT("r.MobileHDR"),0}, {TEXT("r.Mobile.ShadingPath"),0},
        {TEXT("vr.MobileMultiView"),1}, {TEXT("r.Mobile.AntiAliasing"),3}, {TEXT("r.Mobile.PropagateAlpha"),1}})
    {
        const auto* Value=IConsoleManager::Get().FindConsoleVariable(*Setting.Key);
        if(TestNotNull(*Setting.Key,Value))TestEqual(*Setting.Key,Value->GetInt(),Setting.Value);
    }
    for(const TCHAR* Path:{TEXT("/Game/Materials/M_HumanSilhouette"),TEXT("/Game/Materials/M_WallhackTrail"),TEXT("/Game/Materials/M_PersonLabel")})
    {
        auto* Material=LoadObject<UMaterial>(nullptr,Path);
        if(!TestNotNull(Path,Material))continue;
        TestTrue(TEXT("Overlay renders in the main stereo translucency pass"),Material->TranslucencyPass==MTP_BeforeDOF);
        TestFalse(TEXT("No mobile separate-translucency render target"),Material->IsMobileSeparateTranslucencyEnabled());
        TestTrue(TEXT("Through-wall visibility retained"),Material->bDisableDepthTest);
        TestEqual(TEXT("Opacity retained"),Material->GetBlendMode(),BLEND_Translucent);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPeopleEyeFrustumTest,"Wallhack.Stereo.PeoplePairedEyeFrustums",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FPeopleEyeFrustumTest::RunTest(const FString&)
{
    // Paired perspective captures catch eye-position/culling/billboard mistakes.
    // They do NOT exercise Android multiview array layers; require Quest evidence too.
    FNavigationWorld F;auto* People=F.World->GetSubsystem<UWallhackPeopleSubsystem>();
    const int32 Id=People->AddPerson({3,0,0},1.75f,180);auto* Renderer=People->GetRenderer();
    if(!TestNotNull(TEXT("People renderer"),Renderer))return false;
    auto* Camera=F.World->SpawnActor<ASceneCapture2D>();auto* Capture=Camera->GetCaptureComponent2D();
    Capture->bCaptureEveryFrame=false;Capture->bCaptureOnMovement=false;Capture->CaptureSource=SCS_SceneColorHDR;
    Capture->PrimitiveRenderMode=ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
    Capture->ShowOnlyActorComponents(Renderer);Capture->FOVAngle=75;
    auto* RT=NewObject<UTextureRenderTarget2D>(Camera);RT->RenderTargetFormat=RTF_RGBA16f;
    RT->ClearColor=FLinearColor::Black;RT->InitAutoFormat(768,768);RT->UpdateResourceImmediate(true);Capture->TextureTarget=RT;
    auto* Wall=F.World->SpawnActor<AStaticMeshActor>();auto* WallMesh=Wall->GetStaticMeshComponent();
    WallMesh->SetMobility(EComponentMobility::Movable);WallMesh->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube")));
    WallMesh->SetCastShadow(false);Wall->SetActorLocation({100,0,100});Wall->SetActorScale3D({.02,20,20});
    Capture->ShowOnlyActorComponents(Wall);FAssetCompilingManager::Get().FinishAllCompilation();
    for(float IPD:{.058f,.072f})for(float Facing:{90.f,180.f})
    {
        const FRotator Head(-10,0,8);const FVector Center(0,0,160);
        F.SetViewerPose(Center,Head);People->UpdatePerson(Id,{3,0,0},1.75f,Facing);People->Tick(.25f);
        int32 Red[2]={},White[2]={};
        for(int32 Eye=0;Eye<2;++Eye)
        {
            const FVector EyePosition=Center+Head.RotateVector(FVector(0,(Eye?1:-1)*IPD*50,0));
            Capture->SetWorldLocationAndRotation(EyePosition,Head);
            F.World->SendAllEndOfFrameUpdates();
            for(int32 I=0;I<3;++I){Capture->CaptureScene();FlushRenderingCommands();}
            TArray<FColor> Pixels;RT->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
            const FString Name=FString::Printf(TEXT("person-perspective-%s-ipd%02d-facing%03d"),Eye?TEXT("right"):TEXT("left"),FMath::RoundToInt(IPD*1000),int32(Facing));
            SaveNavigationImage(Pixels,768,768,*Name);
            for(const auto P:Pixels)
            {
                Red[Eye]+=P.R>P.G*1.5&&P.R>P.B*1.5&&P.R>20;
                White[Eye]+=P.R>40&&FMath::Abs(int(P.R)-int(P.G))<25&&FMath::Abs(int(P.G)-int(P.B))<25;
            }
            TestTrue(*FString::Printf(TEXT("%s: silhouette and outline through wall"),*Name),Red[Eye]>1000);
            TestTrue(*FString::Printf(TEXT("%s: corner telemetry through wall"),*Name),White[Eye]>70);
        }
        TestTrue(TEXT("Neither eye loses the body to culling"),FMath::Min(Red[0],Red[1])>FMath::Max(Red[0],Red[1])*.75);
        TestTrue(TEXT("Neither eye loses the billboard glyphs"),FMath::Min(White[0],White[1])>FMath::Max(White[0],White[1])*.7);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPeopleRenderTest,"Wallhack.People.Render.AnatomyFacingAndThroughWalls",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FPeopleRenderTest::RunTest(const FString&)
{
    FNavigationWorld F;auto* People=F.World->GetSubsystem<UWallhackPeopleSubsystem>();
    const int32 Id=People->AddPerson({2.8,0,0},1.75f,180);auto* Renderer=People->GetRenderer();
    if(!TestNotNull(TEXT("Body renderer"),Renderer))return false;
    auto* Camera=F.World->SpawnActor<ASceneCapture2D>();auto* Capture=Camera->GetCaptureComponent2D();
    Capture->bCaptureEveryFrame=false;Capture->bCaptureOnMovement=false;Capture->CaptureSource=SCS_SceneColorHDR;
    Capture->PrimitiveRenderMode=ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
    Capture->ShowOnlyActorComponents(Renderer);Capture->SetWorldLocationAndRotation({0,0,87.5},FRotator::ZeroRotator);Capture->FOVAngle=65;
    auto* RT=NewObject<UTextureRenderTarget2D>(Camera);RT->RenderTargetFormat=RTF_RGBA16f;RT->ClearColor=FLinearColor::Black;
    RT->InitAutoFormat(768,768);RT->UpdateResourceImmediate(true);Capture->TextureTarget=RT;
    FAssetCompilingManager::Get().FinishAllCompilation();
    int32 LastWhite=0;
    auto Read=[&](const TCHAR* Name)
    {
        // This synchronous fixture does not run UWorld's normal end-of-frame
        // transform/material dispatch between edits and capture.
        F.World->SendAllEndOfFrameUpdates();
        for(int32 I=0;I<3;++I){Capture->CaptureScene();FlushRenderingCommands();FAssetCompilingManager::Get().FinishAllCompilation();}
        TArray<FColor> Pixels;RT->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
        SaveNavigationImage(Pixels,768,768,Name);int32 Ink=0;
        LastWhite=0;
        for(const auto P:Pixels)
        {
            Ink+=P.R>P.G*1.5&&P.R>P.B*1.5&&P.R>20;
            LastWhite+=P.R>40&&FMath::Abs(int(P.R)-int(P.G))<25&&FMath::Abs(int(P.G)-int(P.B))<25;
        }
        return Ink;
    };
    auto* Body=Renderer->GetBodyComponents()[0].Get();
    auto* Outline=Renderer->GetOutlineComponents()[0].Get();
    auto* Label=Renderer->GetTelemetryComponents()[0].Get();
    // Measure anatomy independently of the box's perspective-dependent area.
    Outline->SetVisibility(false);
    Label->SetVisibility(false);
    auto* BodyMaterial=Body->GetMaterial(0);
    FLinearColor Tint;float Opacity=0;
    BodyMaterial->GetVectorParameterValue(FMaterialParameterInfo(TEXT("Tint")),Tint);
    BodyMaterial->GetScalarParameterValue(FMaterialParameterInfo(TEXT("Opacity")),Opacity);
    AddInfo(FString::Printf(TEXT("Body registered=%d visible=%d hidden=%d bounds=%s material=%s tint=%s opacity=%.3f showonly=%d"),
        Body->IsRegistered(),Body->IsVisible(),Renderer->IsHidden(),*Body->Bounds.ToString(),*BodyMaterial->GetName(),*Tint.ToString(),Opacity,Capture->ShowOnlyComponents.Num()));
    const int32 Front=Read(TEXT("person-front"));TestTrue(TEXT("Human mesh visible on GPU"),Front>2000);
    People->UpdatePerson(Id,{2.8,0,0},1.75f,90);const int32 Side=Read(TEXT("person-side"));
    TestTrue(TEXT("Profile is visibly narrower than front"),Side>1000&&Side<Front*.8);
    People->UpdatePerson(Id,{2.8,0,0},1.75f,0);TestTrue(TEXT("Back is visible"),Read(TEXT("person-back"))>2000);
    People->UpdatePerson(Id,{2.8,0,0},1.75f,180);
    Outline->SetVisibility(true);Body->SetVisibility(false);
    TestTrue(TEXT("Red wireframe is independently visible"),Read(TEXT("person-box-only"))>300);
    Body->SetVisibility(true);Label->SetVisibility(true);
    const int32 Boxed=Read(TEXT("person-boxed"));
    const int32 LabelWhite=LastWhite;
    TestTrue(TEXT("Corner telemetry produces readable white glyphs on GPU"),LabelWhite>100);
    TArray<FColor> LabelPixels;auto* LabelTexture=Renderer->GetTelemetryTextures()[0].Get();
    LabelTexture->GameThread_GetRenderTargetResource()->ReadPixels(LabelPixels);
    SaveNavigationImage(LabelPixels,LabelTexture->SizeX,LabelTexture->SizeY,TEXT("person-label-texture"));
    TestEqual(TEXT("Smooth-font label uses one quad instead of hundreds of glyph meshes"),Label->GetProcMeshSection(0)->ProcVertexBuffer.Num(),4);
    TestTrue(TEXT("Box surrounds the red silhouette"),Boxed>Front);
    auto* Wall=F.World->SpawnActor<AStaticMeshActor>();auto* WallMesh=Wall->GetStaticMeshComponent();
    WallMesh->SetMobility(EComponentMobility::Movable);WallMesh->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube")));
    WallMesh->SetCastShadow(false);Wall->SetActorLocation({100,0,87.5});Wall->SetActorScale3D({.02,20,20});
    Capture->ShowOnlyActorComponents(Wall);FAssetCompilingManager::Get().FinishAllCompilation();
    TestTrue(TEXT("Silhouette and outline remain visible through an opaque wall"),Read(TEXT("person-through-wall"))>=Boxed*.9);
    TestTrue(TEXT("Corner telemetry also survives depth occlusion"),LastWhite>=LabelWhite*.9);
    F.Nav->SetHidden(true);TestEqual(TEXT("Hidden produces zero silhouette pixels"),Read(TEXT("person-hidden")),0);
    TestEqual(TEXT("Hidden also removes world telemetry"),LastWhite,0);
    F.Nav->SetHidden(false);Wall->SetActorHiddenInGame(true);
    People->UpdatePerson(Id,{4,-1.5,0},1.75f,180);
    People->AddPerson({4,0,0},1.65f,180);People->AddPerson({4,1.5,0},1.9f,180);
    Capture->ShowOnlyActorComponents(Renderer);Capture->SetWorldLocation({0,0,125});Capture->FOVAngle=85;
    Read(TEXT("people-identity-colors"));
    TArray<FColor> PalettePixels;RT->GameThread_GetRenderTargetResource()->ReadPixels(PalettePixels);
    int32 Blue=0,Violet=0;
    for(const auto P:PalettePixels)
    {
        Blue+=P.B>P.R*1.5&&P.B>P.G*1.1&&P.B>25;
        Violet+=P.B>P.R*1.1&&P.R>P.G*1.2&&P.R>25;
    }
    TestTrue(TEXT("Multiple people render distinct blue and violet identities alongside red"),Blue>500&&Violet>500);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavMinimapRenderTest,"Wallhack.Navigation.Minimap.ActualHUDRotationAndLifecycle",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FNavMinimapRenderTest::RunTest(const FString&)
{
    FNavigationWorld F;auto* HUD=F.NavigationHUD();
    auto* People=F.World->GetSubsystem<UWallhackPeopleSubsystem>();
    People->AddPerson({3,0,0},1.75f,180);People->AddPerson({-3,0,0},1.7f,0);
    People->AddPerson({0,3,0},1.8f,90);People->AddPerson({6,-2,0},1.65f,270);
    auto Read=[&](const TCHAR* Name)
    {
        HUD->Tick(.25f);F.World->SendAllEndOfFrameUpdates();FlushRenderingCommands();
        TArray<FColor> Pixels;auto* RT=HUD->GetHUDRenderTarget();
        RT->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
        SaveNavigationImage(Pixels,RT->SizeX,RT->SizeY,Name);return Pixels;
    };
    auto RedAt=[](const TArray<FColor>& Pixels,FVector2D P,int32 Radius=8)
    {
        int32 Count=0;
        for(int32 Y=int(P.Y)-Radius;Y<=int(P.Y)+Radius;++Y)for(int32 X=int(P.X)-Radius;X<=int(P.X)+Radius;++X)
        {
            if(X<0||X>=2048||Y<0||Y>=1152)continue;
            const auto C=Pixels[Y*2048+X];Count+=C.R>C.G*1.5&&C.R>C.B*1.5&&C.R>40;
        }
        return Count;
    };
    WallhackNavPresentation::FLocalMap M;
    const auto First=Read(TEXT("minimap-heading-north"));
    auto CompassInk=[](const TArray<FColor>& Pixels)
    {
        int32 Count=0;
        for(int32 Y=173;Y<235;++Y)for(int32 X=610;X<1438;++X)
        {const auto C=Pixels[Y*2048+X];Count+=C.R>60&&FMath::Abs(int(C.R)-int(C.G))<25&&C.A>20;}
        return Count;
    };
    TestTrue(TEXT("Minimal HUD renders the scrolling compass tape"),CompassInk(First)>500);
    TestTrue(TEXT("Person ahead is a red dot above self in Minimal"),RedAt(First,M.Project({3,0,0}))>12);
    TestTrue(TEXT("Heading bar carries the same red identity as the world/map"),RedAt(First,{1024,251})>12);
    int32 RimInk=0;
    for(int32 Y=240;Y<288;++Y)for(int32 X=1360;X<1456;++X)
    {const auto C=First[Y*2048+X];RimInk+=FMath::Max3(C.R,C.G,C.B)>40&&FMath::Max3(C.R,C.G,C.B)-FMath::Min3(C.R,C.G,C.B)>30;}
    TestTrue(TEXT("Offscreen people retain colored arrows at the heading-bar rim"),RimInk>30);
    auto RegionInk=[](const TArray<FColor>& Pixels,int32 Left,int32 Top,int32 Right,int32 Bottom)
    {
        int32 N=0;for(int32 Y=Top;Y<Bottom;++Y)for(int32 X=Left;X<Right;++X)N+=Pixels[Y*2048+X].A>10;return N;
    };
    TestEqual(TEXT("Idle right panel has no persistent controller/debug text"),RegionInk(First,1280,720,1880,1060),0);
    TestEqual(TEXT("Healthy tracking has no geometry-age/debug footer"),RegionInk(First,260,1020,750,1100),0);
    F.SetHUDDensity(EWallhackHUDDensity::Full);const auto Full=Read(TEXT("clean-hud-full"));
    TestEqual(TEXT("Full view keeps technical debug readouts off the visor"),RegionInk(Full,1250,350,1900,700),0);
    F.SetHUDDensity(EWallhackHUDDensity::Minimal);
    F.SetViewerPose({0,0,170},FRotator(0,90,0));F.Step(240);
    const auto East=Read(TEXT("minimap-heading-east"));M.Yaw=90;
    TestTrue(TEXT("Turning east moves the north contact to the left"),RedAt(East,M.Project({3,0,0}))>12);
    TestTrue(TEXT("Heading marker moves left with that same contact"),RedAt(East,{624,251})>12);
    F.SetViewerPose({0,0,170},FRotator(35,90,20));F.Step(240);
    const auto Tilt=Read(TEXT("minimap-pitch-roll"));
    TestTrue(TEXT("Pitch and roll do not tip the floor plan"),RedAt(Tilt,M.Project({3,0,0}))>12);
    TestTrue(TEXT("Looking down does not displace compass contacts"),RedAt(Tilt,{624,251})>12);
    TestTrue(TEXT("Map range uses a real input binding"),F.Action(TEXT("MapRange")));
    TestEqual(TEXT("Range cycles to ten metres"),HUD->GetMapRangeMeters(),10.f);
    Read(TEXT("minimap-ten-metres"));
    F.Nav->Suspend();const auto Lost=Read(TEXT("minimap-tracking-lost"));
    TestEqual(TEXT("Tracking loss hides stale directional tick labels"),CompassInk(Lost),0);
    int32 Red=0;for(auto C:Lost)Red+=C.R>C.G*1.5&&C.R>C.B*1.5&&C.R>40;
    TestEqual(TEXT("Tracking loss hides all person dots and direction cues"),Red,0);
    F.SetHUDDensity(EWallhackHUDDensity::Hidden);const auto Hidden=Read(TEXT("minimap-hidden"));
    int32 Alpha=0;for(auto C:Hidden)Alpha+=C.A>0;
    TestEqual(TEXT("Hidden remains completely transparent with populated minimap"),Alpha,0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPeopleTelemetryTest,"Wallhack.People.Runtime.CornerTelemetryPoseAndMetricRefresh",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FPeopleTelemetryTest::RunTest(const FString&)
{
    FNavigationWorld F;auto* People=F.World->GetSubsystem<UWallhackPeopleSubsystem>();
    const int32 Id=People->AddPerson({3,0,0},1.75f,0);auto* Renderer=People->GetRenderer();
    auto* Label=Renderer->GetTelemetryComponents()[0].Get();
    TestTrue(TEXT("Distance is true viewer-to-feet range"),Renderer->GetTelemetryText()[0].Contains(TEXT("3.4 M")));
    const auto Initial=Renderer->GetTelemetryText()[0];
    F.SetViewerPose({50,0,170},FRotator(0,25,0));F.Step(1);People->Tick(.1f);
    TestEqual(TEXT("Numerical metrics hold between 5 Hz updates"),Renderer->GetTelemetryText()[0],Initial);
    TestTrue(TEXT("Billboard follows head rotation immediately"),Label->GetComponentQuat().Equals(FRotator(0,25,0).Quaternion(),.001));
    People->Tick(.11f);
    TestTrue(TEXT("Distance refreshes after the next metric interval"),Renderer->GetTelemetryText()[0].Contains(TEXT("3.0 M")));
    People->SetNorthReference(90);People->Tick(.2f);
    TestTrue(TEXT("Facing shares the calibrated compass reference"),Renderer->GetTelemetryText()[0].Contains(TEXT("FACE 090")));
    People->UpdatePerson(Id,{3,0,0},2.f,180);
    TestTrue(TEXT("Height edits immediately update telemetry"),Renderer->GetTelemetryText()[0].Contains(TEXT("H 2.00 M")));
    TestTrue(TEXT("Facing edit immediately updates telemetry"),Renderer->GetTelemetryText()[0].Contains(TEXT("FACE 270")));
    TestTrue(TEXT("Label anchors above the resized top box edge"),Label->GetComponentLocation().Z>200);
    People->AddPerson({2,-1,0},1.6f,0);People->ToggleEditing();People->RemoveSelected();
    TestTrue(TEXT("Removing a person also hides its pooled label"),Renderer->GetTelemetryComponents()[1]->bHiddenInGame);
    F.Nav->Suspend();TestTrue(TEXT("Tracking loss immediately hides labels with the actor"),Renderer->IsHidden());
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPeopleColorIdentityTest,"Wallhack.People.Runtime.StableUniqueColorsAcrossDeletionAndReuse",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FPeopleColorIdentityTest::RunTest(const FString&)
{
    FNavigationWorld F;auto* People=F.World->GetSubsystem<UWallhackPeopleSubsystem>();
    TMap<int32,int32> Initial;
    for(int32 I=0;I<8;++I){const int32 Id=People->AddPerson({3.,I*.2,0},1.75,180);Initial.Add(Id,People->GetSelected()->ColorSlot);}
    People->ToggleEditing();People->SelectNext();People->SelectNext(); // Remove ID 2, not the most recent ID.
    const int32 Removed=People->GetSelected()->Id,FreeSlot=People->GetSelected()->ColorSlot;
    People->RemoveSelected();Initial.Remove(Removed);
    const int32 Replacement=People->AddPerson({2,1,0},1.8,90);
    TestEqual(TEXT("Replacement reuses free color without colliding with surviving IDs"),People->GetSelected()->ColorSlot,FreeSlot);
    TestTrue(TEXT("Replacement ID is new even though color is reused"),Replacement>8);
    People->UpdatePerson(Replacement,{2,2,0},2,270);People->SelectNext();
    TSet<int32> Slots;auto* Renderer=People->GetRenderer();
    for(int32 I=0;I<People->GetPeople().Num();++I)
    {
        const auto& P=People->GetPeople()[I];Slots.Add(P.ColorSlot);
        if(Initial.Contains(P.Id))TestEqual(TEXT("Selection, movement and pool compaction preserve identity"),P.ColorSlot,Initial[P.Id]);
        FLinearColor Tint;Renderer->GetBodyComponents()[I]->GetMaterial(0)->GetVectorParameterValue(FMaterialParameterInfo(TEXT("Tint")),Tint);
        TestTrue(TEXT("Body color follows identity after pool reuse"),Tint.Equals(WallhackPeopleStyle::Color(P),.001));
        const auto Outline=Renderer->GetOutlineComponents()[I]->GetProcMeshSection(0)->ProcVertexBuffer[0].Color;
        const auto Expected=WallhackPeopleStyle::Color(P).ToFColor(false);
        TestTrue(TEXT("Outline color follows the same identity"),Outline.R==Expected.R&&Outline.G==Expected.G&&Outline.B==Expected.B);
        TestTrue(TEXT("Pooled text uses the current numeric ID"),Renderer->GetTelemetryText()[I].StartsWith(FString::Printf(TEXT("PERSON %02d / MANUAL"),P.Id)));
    }
    TestEqual(TEXT("All eight active people retain unique palette slots"),Slots.Num(),8);
    return true;
}
#endif
