#include "WallhackNavigationTypes.h"
#include "WallhackNavigationProviders.h"
#include "WallhackNavigationRenderer.h"
#include "WallhackNavigationPresentation.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
using namespace WallhackNav;
namespace
{
FCell Free(double Time=0){FCell C;C.Occupancy=EOccupancy::Free;C.Evidence=EEvidence::SceneFloor;C.bFloor=C.bClearance=true;C.ObservedAt=Time;return C;}
FCell Wall(){FCell C;C.Occupancy=EOccupancy::Occupied;C.Evidence=EEvidence::SceneWall;return C;}
FMap Room(int32 X=70,int32 Y=30)
{
    FMap M;
    for(int32 J=-Y;J<=Y;++J)for(int32 I=-10;I<=X;++I)M.Observe({I,J},I==-10||I==X||J==-Y||J==Y?Wall():Free());
    return M;
}
}
#define NAVTEST(Class,Name) IMPLEMENT_SIMPLE_AUTOMATION_TEST(Class,"Wallhack.Navigation." Name,EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
NAVTEST(FNavWearerMask,"Depth.WearerOcclusionBounds");
bool FNavWearerMask::RunTest(const FString&)
{
    FWearerDepthMask Mask;Mask.Head={1.04,2.33,3.63};Mask.Floor=2.083f;
    TestTrue(TEXT("Recorded torso-height blocker beneath standing headset is self occlusion"),Mask.Contains({1.05,2.45,3.087}));
    TestFalse(TEXT("Floor remains observable"),Mask.Contains({1.05,2.35,2.083}));
    TestTrue(TEXT("Standing lower-body return below old torso cutoff is self occlusion"),Mask.Contains({1.05,2.35,2.4}));
    TestFalse(TEXT("Floor and foot-height obstacles remain observable"),Mask.Contains({1.05,2.35,2.2}));
    TestFalse(TEXT("Obstacle 30cm ahead remains observable"),Mask.Contains({1.34,2.33,3.1}));
    Mask.TrackedHands.Add({1.6,2.33,3.2});
    TestTrue(TEXT("Tracked controller/hand has a small exclusion volume"),Mask.Contains({1.62,2.34,3.2}));
    TestTrue(TEXT("Tracked arm segment is excluded beyond the hand sphere"),Mask.Contains({1.41,2.33,3.265}));
    TestFalse(TEXT("Obstacle beside arm remains observable"),Mask.Contains({1.41,2.53,3.265}));
    TestFalse(TEXT("Furniture beyond controller volume is retained"),Mask.Contains({1.8,2.33,3.2}));
    Mask.TrackedHands.Reset();TestFalse(TEXT("Untracked hand has no stale exclusion"),Mask.Contains({1.62,2.34,3.2}));
    Mask.Head.Z=2.55;TestFalse(TEXT("Put-down headset cannot infer a standing torso"),Mask.Contains({1.05,2.35,2.3}));
    return true;
}
NAVTEST(FNavAvoidance,"Planner.ObstaclesAndDistances");
bool FNavAvoidance::RunTest(const FString&)
{
    auto M=Room();for(int32 X=20;X<=25;++X)for(int32 Y=-8;Y<=8;++Y)M.Observe({X,Y},Wall());
    auto R=Plan(M,{0,0,0},{6,0,0});
    TestTrue(TEXT("Routes around furniture"),R.bComplete);TestTrue(TEXT("Walking exceeds straight range"),R.Length()>6.1f);
    TestEqual(TEXT("No speculative detour when mapped route exists"),R.EstimatedMeters,0.f);
    TestTrue(TEXT("Occupied cells respected"),ValidateRoute(M,R));
    for(const auto& P:R.Points)TestTrue(TEXT("Route point has floor support"),M.WalkabilityAt(P.Position)==EOccupancy::Free);
    M.Observe({0,0},Wall());FIntPoint Blocking;
    TestTrue(TEXT("Blocked start reports its containing cell"),M.Walkability({0,0},&Blocking)==EOccupancy::Occupied&&Blocking==FIntPoint(0,0));
    const auto Blocked=Plan(M,{0,0,0},{6,0,0});
    TestTrue(TEXT("Start obstruction distinguished from disconnected route"),Blocked.bStartBlocked&&!Blocked.bComplete&&Blocked.Points.IsEmpty());
    TestFalse(TEXT("Moving to clear floor removes start obstruction"),Plan(M,{0,1,0},{6,0,0}).bStartBlocked);
    return true;
}
NAVTEST(FNavPassage,"Planner.NarrowPassageAndDisconnected");
bool FNavPassage::RunTest(const FString&)
{
    auto M=Room();for(int32 Y=-30;Y<=30;++Y)if(Y!=0)M.Observe({30,Y},Wall());
    auto R=Plan(M,{.05,.05,0},{6.05,.05,0});TestTrue(TEXT("One clear 10cm cell connects the rooms for a point"),R.bComplete);
    TestTrue(TEXT("Narrow curved route stays clear of walls"),ValidateRoute(M,R));
    M.Observe({30,0},Wall());
    R=Plan(M,{.05,.05,0},{6.05,.05,0});TestFalse(TEXT("Unknown search cannot cross a fully closed wall"),R.bComplete);
    TestTrue(TEXT("Reachable prefix remains"),R.Points.Num()>1);TestTrue(TEXT("Partial prefix stays clear of walls"),ValidateRoute(M,R));return true;
}
NAVTEST(FNavDiscovery,"Planner.DoorwayDiscoveryAndWallInvalidation");
bool FNavDiscovery::RunTest(const FString&)
{
    auto M=Room();
    // Build an unobserved central strip while retaining the enclosing walls.
    FMap Gap;
    for(int32 Y=-30;Y<=30;++Y)for(int32 X=-10;X<=70;++X)
        if(X<20||X>40||FMath::Abs(Y)==30)Gap.Observe({X,Y},X==-10||X==70||FMath::Abs(Y)==30?Wall():Free());
    auto R=Plan(Gap,{0,0,0},{6,0,0});TestTrue(TEXT("Unknown connection offered"),R.bComplete);TestTrue(TEXT("Unknown connection labelled"),R.EstimatedMeters>1);
    for(int32 Y=-30;Y<=30;++Y)Gap.Observe({30,Y},Wall());
    TestFalse(TEXT("Discovered wall invalidates speculative shortcut"),ValidateRoute(Gap,R));
    TestFalse(TEXT("Replan never crosses known wall"),Plan(Gap,{0,0,0},{6,0,0}).bComplete);
    auto Open=Room();for(int32 Y=-30;Y<=30;++Y)if(FMath::Abs(Y)>7)Open.Observe({30,Y},Wall());
    TestTrue(TEXT("Discovered 1.4m doorway reconnects rooms"),Plan(Open,{0,0,0},{6,0,0}).bComplete);return true;
}
NAVTEST(FNavEvidence,"Map.UnknownAgingAndMovedFurniture");
bool FNavEvidence::RunTest(const FString&)
{
    FMap M;FCell C=Wall();C.Evidence=EEvidence::Depth;C.ObservedAt=1;M.Now=1;M.Observe({0,0},C);
    const FMapSnapshot Before=M;
    M.Now=10;TestTrue(TEXT("Looking away retains obstacle memory"),M.State({0,0})==EOccupancy::Occupied);
    TestTrue(TEXT("Immutable snapshot remains occupied"),Before.State({0,0})==EOccupancy::Occupied);
    FCell Clear=Free(11);Clear.Evidence=EEvidence::Depth;
    M.Observe({0,0},Clear);TestTrue(TEXT("First contradictory observation insufficient"),M.Find({0,0})->Occupancy==EOccupancy::Occupied);
    Clear.ObservedAt=11.2;M.Observe({0,0},Clear);Clear.ObservedAt=11.4;M.Observe({0,0},Clear);
    TestTrue(TEXT("Three positive floor/clearance checks clear moved furniture"),M.State({0,0})==EOccupancy::Free);
    M.Observe({0,0},FCell{});TestTrue(TEXT("Miss never clears or refreshes evidence"),M.Find({0,0})->ObservedAt==11.4);
    C=Wall();M.Observe({0,0},C);for(int32 I=0;I<4;++I){Clear.ObservedAt++;M.Observe({0,0},Clear);}
    TestTrue(TEXT("Scanned wall needs stronger proof than a temporary obstacle"),M.State({0,0})==EOccupancy::Occupied);
    Clear.ObservedAt++;M.Observe({0,0},Clear);
    TestTrue(TEXT("Five positive floor and clearance checks discover an opening"),M.State({0,0})==EOccupancy::Free);
    return true;
}
NAVTEST(FNavScanConfirmation,"Map.ScanConfirmationDoesNotExpire");
bool FNavScanConfirmation::RunTest(const FString&)
{
    auto M=Room();FVector Standing;const FVector Endpoint(2.05,.05,0);
    FCell Clear=Free(1);Clear.Evidence=EEvidence::Depth;Clear.Floor=.015f;
    M.Observe({20,0},Clear);M.Now=10;
    TestTrue(TEXT("Expired confirming depth retains independent scanned floor"),SelectStanding(M,Endpoint,true,{0,0,1.7},Standing));
    TestTrue(TEXT("Stable floor elevation comes from room scan"),FMath::IsNearlyZero(M.Find({20,0})->Floor));
    FCell Block=Wall();Block.Evidence=EEvidence::Depth;Block.ObservedAt=11;M.Now=11;
    M.Observe({20,0},Block);
    TestFalse(TEXT("New obstruction immediately rejects the target point"),SelectStanding(M,Endpoint,true,{0,0,1.7},Standing));
    M.Now=20;
    TestTrue(TEXT("Expired obstruction stays blocked and never resurrects old scan"),M.State({20,0})==EOccupancy::Occupied);
    TestFalse(TEXT("Remembered obstruction cannot retain a preview"),SelectStanding(M,Endpoint,true,{0,0,1.7},Standing));
    for(int32 I=0;I<3;++I){Clear.ObservedAt=20+I*.2;M.Observe({20,0},Clear);}
    M.Now=20.4;TestTrue(TEXT("Repeated positive clearance can restore endpoint"),SelectStanding(M,Endpoint,true,{0,0,1.7},Standing));
    M.Now=30;TestTrue(TEXT("Cleared floor is remembered but guidance needs fresh evidence"),M.State({20,0})==EOccupancy::Free&&M.IsEstimated({20,0}));
    Clear.Occupancy=EOccupancy::Unsupported;Clear.Floor=.3f;Clear.ObservedAt=31;M.Observe({20,0},Clear);M.Now=100;
    TestTrue(TEXT("Detected level changes never fall back to scanned floor"),M.State({20,0})==EOccupancy::Unsupported);
    return true;
}
NAVTEST(FNavFloor,"Map.FloorSupportClearanceAndSameLevel");
bool FNavFloor::RunTest(const FString&)
{
    FMap M;FCell C=Free();C.bClearance=false;M.Observe({0,0},C);
    TestTrue(TEXT("Floor alone does not establish walkability"),M.State({0,0})==EOccupancy::Unknown);
    C=Free();C.bFloor=false;M.Observe({1,0},C);TestTrue(TEXT("Clearance without floor is unknown"),M.State({1,0})==EOccupancy::Unknown);
    M=Room();for(int32 Y=-29;Y<30;++Y){C=Free();C.Floor=.25f;M.Observe({30,Y},C);}
    TestFalse(TEXT("Unsupported step cannot be a speculative route"),Plan(M,{0,0,0},{6,0,0}).bComplete);
    return true;
}
NAVTEST(FNavLimit,"Planner.SearchBudgetAndPartialRoute");
bool FNavLimit::RunTest(const FString&)
{
    auto M=Room();auto R=Plan(M,{0,0,0},{6,0,0},12);
    TestTrue(TEXT("One expansion budget shared across passes"),R.Expanded<=12);TestFalse(TEXT("Limited route explicit incomplete"),R.bComplete);
    TestTrue(TEXT("Returns reachable portion"),R.Points.Num()>1);return true;
}
NAVTEST(FNavSelection,"Interaction.StandOffAndArrival");
bool FNavSelection::RunTest(const FString&)
{
    auto M=Room();for(int32 X=20;X<=25;++X)for(int32 Y=-5;Y<=5;++Y)M.Observe({X,Y},Wall());
    FVector Stand;TestTrue(TEXT("Object gets a reachable stand-off"),SelectStanding(M,{2,0,1},false,{0,0,1.7},Stand));
    TestTrue(TEXT("Stand-off about 0.6m beside hit"),FMath::IsNearlyEqual(float(FVector::Dist2D(Stand,{2,0,1})),.6f,.01f));
    TestFalse(TEXT("Occupied endpoint rejected"),SelectStanding(M,{2.2,0,0},true,{0,0,1.7},Stand));
    FTarget T;T.Standing={1,0,0};T.Surface={1,0,1};
    TestTrue(TEXT("Mapped proximity arrives"),HasArrived(M,T,{.5,0,1.7}));
    FMap Empty;TestFalse(TEXT("Speculative proximity cannot arrive"),HasArrived(Empty,T,{.5,0,1.7}));return true;
}
NAVTEST(FNavProviders,"Providers.PositiveHitsAndFailure");
bool FNavProviders::RunTest(const FString&)
{
    auto P=FDeterministicProvider::MakeRoom();FSurfaceHit H;
    TestTrue(TEXT("Floor query has positive hit"),P->Query({0,0,1.7},{0,0,-1},3,H)==EDepthResult::Hit&&H.bFloor);
    TestTrue(TEXT("Miss is unknown"),P->Query({0,0,1.7},{0,0,1},3,H)==EDepthResult::Unknown);
    P->bAvailable=false;TestTrue(TEXT("Depth outage explicit"),P->Query({0,0,1.7},{0,0,-1},3,H)==EDepthResult::Unavailable);
    WallhackNav::FScene Scene;TestTrue(TEXT("Cached scene retained independently"),P->Poll(Scene)==ESceneStatus::Ready&&Scene.Floors.Num()==1);return true;
}
NAVTEST(FNavTrail,"Render.FloorGeometryAndEvidenceStyling");
bool FNavTrail::RunTest(const FString&)
{
    FDisplaySnapshot D;D.bGuidance=true;D.Route.Points={{{0,0,0},false},{{2,0,0},false},{{4,0,0},true}};
    auto G=BuildTrail(D);TestTrue(TEXT("World geometry exists"),G.Vertices.Num()>0);
    for(auto V:G.Vertices)TestTrue(TEXT("Trail and contrast edging stay attached to floor"),V.Z>=.0279&&V.Z<=.0301);
    bool Mint=false,Amber=false;for(auto C:G.Colors){Mint|=C.G>C.R;Amber|=C.R>C.G;}
    TestTrue(TEXT("Observed mint and estimated amber coexist"),Mint&&Amber);
    D.bHidden=true;TestTrue(TEXT("Hidden removes every trail vertex"),BuildTrail(D).Vertices.IsEmpty());
    D.bHidden=false;D.bGuidance=false;TestTrue(TEXT("Lost tracking removes every trail vertex"),BuildTrail(D).Vertices.IsEmpty());return true;
}
NAVTEST(FNavOrientedScene,"Providers.OrientedWallAndLowCeiling");
bool FNavOrientedScene::RunTest(const FString&)
{
    WallhackNav::FScene S;S.Floors.Add({{{-4,-4},{4,-4},{4,4},{-4,4}},0});
    FObstacle O;O.bWall=true;O.LocalBox=FBox(FVector(-.025,-2,0),FVector(.025,2,2.5));
    O.LocalToWorld=FTransform(FRotator(0,45,0));
    for(int32 I=0;I<8;++I)O.Box+=O.LocalToWorld.TransformPosition(FVector(I&1?.025:-.025,I&2?2:-2,I&4?2.5:0));
    S.Obstacles.Add(O);FCell C;
    TestTrue(TEXT("Diagonal wall does not fill its world bounding box"),S.Sample({1,1,0},C)&&C.Occupancy==EOccupancy::Free);
    TestTrue(TEXT("Actual diagonal wall occupied"),S.Sample({1,-1,0},C)&&C.Occupancy==EOccupancy::Occupied);
    S.Obstacles.Reset();S.Obstacles.Add({FBox(FVector(-2,-2,1.6),FVector(2,2,1.7)),true});
    TestTrue(TEXT("Low ceiling denies standing clearance"),S.Sample({0,0,0},C)&&C.Occupancy==EOccupancy::Occupied);return true;
}
NAVTEST(FNavPreciseClearance,"Planner.PointNavigationWithoutBodyRadius");
bool FNavPreciseClearance::RunTest(const FString&)
{
    auto M=Room();for(int32 Y=-30;Y<=30;++Y)M.Observe({10,Y},Wall());
    const FVector Near(.999,.05,0);
    TestTrue(TEXT("Point 1mm beside occupied cell remains free"),M.WalkabilityAt(Near)==EOccupancy::Free);
    TestTrue(TEXT("Point inside wall remains blocked"),M.WalkabilityAt({1.001,.05,0})==EOccupancy::Occupied);
    TestTrue(TEXT("Start beside furniture can route away without snapping"),Plan(M,Near,{-.1,1.5,0}).bComplete);
    FVector Selected;
    TestTrue(TEXT("Floor target beside a wall needs no body-radius clearance"),SelectStanding(M,Near,true,{0,0,1.7},Selected));
    FTarget Target;Target.Standing=Near;Target.Surface=Near;
    TestTrue(TEXT("Arrival beside a wall uses point clearance too"),HasArrived(M,Target,{.95,.05,1.7}));
    FMap Single;Single.Observe({0,0},Free());
    TestTrue(TEXT("Adjacent unknown cells do not downgrade known route point"),Single.WalkabilityAt({.05,.05,0})==EOccupancy::Free);
    TestTrue(TEXT("Negative positions use their containing cell"),Single.WalkabilityAt({-.001,.05,0})==EOccupancy::Unknown);
    return true;
}
NAVTEST(FNavRememberedWall,"Map.LookingAwayNeverOpensAWall");
bool FNavRememberedWall::RunTest(const FString&)
{
    auto M=Room();FCell C=Wall();C.Evidence=EEvidence::Depth;C.ObservedAt=1;
    for(int32 Y=-30;Y<=30;++Y)M.Observe({30,Y},C);
    M.Now=120;
    auto R=Plan(M,{0,0,0},{6,0,0});
    TestFalse(TEXT("Two minutes without seeing a live-only wall cannot open a shortcut"),R.bComplete);
    TestTrue(TEXT("Partial route respects remembered wall"),ValidateRoute(M,R));
    M.Observe({30,0},FCell{});
    TestFalse(TEXT("Misses cannot open a doorway"),Plan(M,{0,0,0},{6,0,0}).bComplete);
    FCell Clear=Free();Clear.Evidence=EEvidence::Depth;
    for(int32 I=0;I<3;++I){Clear.ObservedAt=120+I*.2;M.Observe({30,0},Clear);}
    M.Now=140;R=Plan(M,{0,0,0},{6,0,0});
    TestTrue(TEXT("Positively observed doorway stays connected after looking away"),R.bComplete&&ValidateRoute(M,R));
    TestTrue(TEXT("Remembered live-only doorway is explicitly estimated"),R.EstimatedMeters>0);
    FTarget T;T.Standing={3.05,.05,0};
    TestFalse(TEXT("Cached floor alone cannot declare arrival"),HasArrived(M,T,{3.05,.05,1.7}));
    return true;
}
NAVTEST(FNavCachedCorridor,"Map.CachedFloorKeepsItsShapeAndElevation");
bool FNavCachedCorridor::RunTest(const FString&)
{
    FMap M;M.Floor=2.08f;M.Now=100;
    FCell C=Free(1);C.Floor=M.Floor;C.Evidence=EEvidence::Depth;
    for(int32 X=0;X<=20;++X)M.Observe({X,0},C);
    for(int32 Y=1;Y<=20;++Y)M.Observe({20,Y},C);
    auto R=Plan(M,{.05,.05,M.Floor},{2.05,2.05,M.Floor});
    TestTrue(TEXT("Observed corridor remains connected in session memory"),R.bComplete);
    TestTrue(TEXT("Stale observations are still distinguished from fresh ones"),R.EstimatedMeters>3.5&&R.ObservedMeters==0);
    for(const auto& P:R.Points)
    {
        TestTrue(TEXT("Smoothing cached floor never cuts across unseen corner"),M.State(FMapSnapshot::Key(P.Position))==EOccupancy::Free);
        TestTrue(TEXT("Cached floor keeps measured elevation"),FMath::IsNearlyEqual(float(P.Position.Z),M.Floor));
    }
    return true;
}
NAVTEST(FNavElevatedBoundary,"Providers.WallsOutsideElevatedFloorPolygon");
bool FNavElevatedBoundary::RunTest(const FString&)
{
    for(float Z:{2.08f,-2.08f})
    {
        WallhackNav::FScene S;S.Floors.Add({{{0,0},{2,0},{2,2},{0,2}},Z});
        S.Obstacles.Add({FBox(FVector(-.1,-.1,Z),FVector(0,2.1,Z+2.5)),true});
        S.Obstacles.Add({FBox(FVector(2,-.1,Z),FVector(2.1,2.1,Z+2.5)),true});
        S.Obstacles.Add({FBox(FVector(-.1,-.1,Z),FVector(2.1,0,Z+2.5)),true});
        S.Obstacles.Add({FBox(FVector(-.1,2,Z),FVector(2.1,2.1,Z+2.5)),true});
        FMap M;M.Floor=Z;
        for(int32 Y=-3;Y<24;++Y)for(int32 X=-3;X<24;++X)
        {FCell C;if(S.Sample(FMapSnapshot::Center({X,Y},Z),C))M.Observe({X,Y},C);}
        TestTrue(TEXT("Outer side of scanned wall survives nonzero floor elevation"),M.State({20,10})==EOccupancy::Occupied);
        TestFalse(TEXT("Unknown search cannot escape an elevated closed room"),Plan(M,{1,1,Z},{3,1,Z}).bComplete);
    }
    return true;
}
NAVTEST(FNavMemoryOutline,"Map.MinimapSharesFusedGeometry");
bool FNavMemoryOutline::RunTest(const FString&)
{
    FMap M;FCell C=Free(1);C.Evidence=EEvidence::Depth;
    for(int32 Y=0;Y<10;++Y)for(int32 X=0;X<10;++X)M.Observe({X,Y},C);
    auto Outline=BuildMapOutline(M);
    TestEqual(TEXT("A hundred floor cells merge into four contour strokes"),Outline.Num(),4);
    C=Wall();C.Evidence=EEvidence::Depth;C.ObservedAt=2;
    M.Observe({5,5},C);M.Now=200;Outline=BuildMapOutline(M);
    int32 Obstacles=0;for(const auto& E:Outline)Obstacles+=E.bObstacle;
    TestEqual(TEXT("Live-only obstacle stays visible outside the camera view"),Obstacles,4);
    const FMapSnapshot Before=M;C=Free();C.Evidence=EEvidence::Depth;
    for(int32 I=0;I<3;++I){C.ObservedAt=201+I*.2;M.Observe({5,5},C);}
    TestEqual(TEXT("Positive clearance removes the same obstacle from the map"),BuildMapOutline(M).Num(),4);
    TestTrue(TEXT("Planner snapshot is not mutated by map updates"),Before.State({5,5})==EOccupancy::Occupied);
    return true;
}
NAVTEST(FNavPointSegments,"Planner.PointSegmentsCannotClipWalls");
bool FNavPointSegments::RunTest(const FString&)
{
    auto M=Room();M.Observe({10,10},Wall());
    FRoute R;R.Points={{{.997,1.011,0},false},{{1.011,.997,0},false}};
    TestFalse(TEXT("Sub-2cm segment cannot slip through wall corner between samples"),ValidateRoute(M,R));
    Swap(R.Points[0],R.Points[1]);
    TestFalse(TEXT("Corner crossing rejected in reverse direction too"),ValidateRoute(M,R));
    R.Points={{{.99,.95,0},false},{{.99,1.15,0},false}};
    TestTrue(TEXT("Segment alongside wall has no artificial clearance margin"),ValidateRoute(M,R));
    R.Points={{{.95,.95,0},false},{{1.15,1.15,0},false}};
    TestFalse(TEXT("Exact diagonal crossing still sees the wall"),ValidateRoute(M,R));
    return true;
}
NAVTEST(FNavCurves,"Planner.CurvesRespectClearanceAndEvidence");
bool FNavCurves::RunTest(const FString&)
{
    auto M=Room();FRoute R;R.bComplete=true;
    R.Points={{{.05,.05,0},false},{{1.55,.05,0},false},{{1.55,1.55,0},false}};
    // A short zigzag-free corner is rounded by the same postprocessor as A*.
    M.Observe({7,7},Wall()); // Prevent straight line-of-sight removal of the turn.
    SmoothRoute(M,R);
    TestTrue(TEXT("Rounded corner remains a complete safe route"),R.bComplete&&ValidateRoute(M,R));
    bool Curved=false;
    for(int32 I=1;I<R.Points.Num();++I)
    {
        const FVector D=R.Points[I].Position-R.Points[I-1].Position;
        Curved|=FMath::Abs(D.X)>.001&&FMath::Abs(D.Y)>.001;
        TestTrue(TEXT("Dense geometry supports smooth arrows and live invalidation"),D.Size()<=.051);
    }
    TestTrue(TEXT("Turn contains non-axis-aligned curve segments"),Curved);
    TestEqual(TEXT("Start remains fixed"),R.Points[0].Position,FVector(.05,.05,0));
    TestEqual(TEXT("Destination remains fixed"),R.Points.Last().Position,FVector(1.55,1.55,0));
    FMap Unknown;FRoute E;E.Points={{{0,0,0},true},{{1,0,0},true},{{1,1,0},true}};SmoothRoute(Unknown,E);
    TestTrue(TEXT("Unknown curve remains estimated"),E.EstimatedMeters>0&&E.ObservedMeters==0);
    M.Observe({15,8},Wall());TestFalse(TEXT("New wall immediately invalidates the curve"),ValidateRoute(M,R));
    return true;
}
NAVTEST(FNavRemainingRoute,"Planner.OnlyRemainingRouteInvalidates");
bool FNavRemainingRoute::RunTest(const FString&)
{
    auto M=Room();auto R=Plan(M,{0,0,0},{6,0,0});
    M.Observe({0,0},Wall());TestFalse(TEXT("Original start really is obstructed"),ValidateRoute(M,R));
    TrimTraversedRoute(R,{2,0,1.7});
    TestTrue(TEXT("Obstacle already passed cannot erase route ahead"),R.bComplete&&ValidateRoute(M,R));
    M.Observe({40,0},Wall());TestFalse(TEXT("Obstacle ahead still suppresses remaining route"),ValidateRoute(M,R));
    FRoute Long;Long.Points={{{2,0,0},false},{{6,0,0},false}};
    TestFalse(TEXT("Validation checks between endpoints too"),ValidateRoute(M,Long));
    return true;
}
NAVTEST(FNavVisibleSelection,"Interaction.NoFarSideStandOff");
bool FNavVisibleSelection::RunTest(const FString&)
{
    auto M=Room();for(int32 X=-10;X<=20;++X)for(int32 Y=-30;Y<=30;++Y)M.Observe({X,Y},Wall());
    FVector Out;
    TestFalse(TEXT("Cannot silently move marker to far side of object/wall"),SelectStanding(M,{2,0,1},false,{0,0,1.7},Out));
    return true;
}
NAVTEST(FNavMinimalArrows,"Render.ClearArrowOnlyGuidance");
bool FNavMinimalArrows::RunTest(const FString&)
{
    FDisplaySnapshot D;D.bGuidance=true;D.Route.Points={{{0,0,0},false},{{3,0,0},false}};
    auto G=BuildTrail(D);
    TestTrue(TEXT("Frequent arrows across the three-metre route"),G.Vertices.Num()>=9*16);
    FBox Bounds(ForceInit);
    int32 Foreground=0,Contrast=0;
    for(int32 I=0;I+3<G.Vertices.Num();I+=4)
    {
        const FVector A=(G.Vertices[I]+G.Vertices[I+1])*.5,B=(G.Vertices[I+2]+G.Vertices[I+3])*.5;
        TestTrue(TEXT("Only short diagonal wings, no connecting ribbon or shaft"),FVector::Dist(A,B)<.23&&FMath::Abs(B.Y-A.Y)>.16);
        if(G.Colors[I].G>.5f)
        {
            ++Foreground;
            TestTrue(TEXT("Readable floor strokes exceed two centimetres"),FVector::Dist(G.Vertices[I],G.Vertices[I+1])>.02);
            TestTrue(TEXT("Near arrows remain prominent in passthrough"),G.Colors[I].A>.8f);
        }
        else {++Contrast;TestTrue(TEXT("Dark edging contrasts with light floors"),G.Colors[I].R<.02f&&G.Colors[I].A>.4f);}
        for(int32 J=0;J<4;++J)Bounds+=G.Vertices[I+J];
    }
    TestTrue(TEXT("Chevrons have a clear wide profile"),Bounds.GetSize().Y>.35&&Bounds.GetSize().Y<.42);
    TestTrue(TEXT("First marker is within a comfortable downward glance"),Bounds.Min.X<.35);
    TestEqual(TEXT("Each stroke has contrast edging"),Foreground,Contrast);
    const int32 ObservedVertices=G.Vertices.Num();
    D.Route.Points[0].bEstimated=D.Route.Points[1].bEstimated=true;G=BuildTrail(D);
    TestEqual(TEXT("Estimated sections use the same arrow-only geometry"),G.Vertices.Num(),ObservedVertices);
    for(auto C:G.Colors)if(C.R>.5f)TestTrue(TEXT("Unknown chevrons remain amber and distinct"),C.R>C.G&&C.A<.7f&&C.A>.5f);
    return true;
}
NAVTEST(FNavAcrossWallProgress,"Planner.ProgressAndArrivalCannotJumpAcrossWall");
bool FNavAcrossWallProgress::RunTest(const FString&)
{
    auto M=Room();for(int32 Y=0;Y<=10;++Y)M.Observe({10,Y},Wall());
    const FVector Viewer(.999,.55,1.7);
    FRoute R;R.bComplete=true;
    R.Points={{{.65,.55,0},false},{{.65,-.15,0},false},{{1.25,-.15,0},false},{{1.15,.55,0},false}};
    TestTrue(TEXT("Original route goes around the end of the wall"),ValidateRoute(M,R));
    TrimTraversedRoute(R,Viewer,&M);
    TestTrue(TEXT("Progress retains the detour despite a closer point on the far side"),R.Points.Num()>=4&&R.bComplete&&ValidateRoute(M,R));
    const auto Cursor=WallhackNavPresentation::ProjectRoute(R,Viewer);
    TestEqual(TEXT("Arrow rendering follows the checked connection from the wearer"),Cursor.Segment,0);
    TestTrue(TEXT("Remaining route includes the trip around the wall"),Cursor.Remaining>1.5);
    FTarget T;T.Standing={1.15,.55,0};
    TestFalse(TEXT("A person fifteen centimetres away through a wall is not reached"),HasArrived(M,T,Viewer));
    TestTrue(TEXT("Reaching the same side still arrives"),HasArrived(M,T,{1.2,.55,1.7}));
    return true;
}
NAVTEST(FNavNearArrows,"Render.ShortAndRemainingRouteVisibility");
bool FNavNearArrows::RunTest(const FString&)
{
    FDisplaySnapshot D;D.bGuidance=true;D.Viewer={2.2,0,1.7};
    D.Route.Points={{{0,0,0},false},{{3,0,0},false}};
    auto G=BuildTrail(D);
    TestTrue(TEXT("Passing the midpoint of a sparse segment cannot erase its remaining arrows"),!G.Vertices.IsEmpty());
    for(auto V:G.Vertices)TestTrue(TEXT("No arrows behind the viewer or beyond the route endpoint"),V.X>2.2&&V.X<3.02);
    D.Viewer={0,0,1.7};D.Route.Points.Last().Position={.2,0,0};G=BuildTrail(D);
    TestTrue(TEXT("Short reachable prefix still has an arrow"),!G.Vertices.IsEmpty());
    for(auto V:G.Vertices)TestTrue(TEXT("Short prefix does not fabricate a connection to the destination"),V.X>=0&&V.X<=.2);
    D.State=ERouteState::Arrived;TestTrue(TEXT("Arrival removes walking arrows"),BuildTrail(D).Vertices.IsEmpty());
    D.bHasTarget=true;D.Target.Standing={.2,0,0};D.Route.bComplete=true;
    TestTrue(TEXT("Arrival retains the destination ring"),!BuildTrail(D).Vertices.IsEmpty());
    D.bHidden=true;TestTrue(TEXT("Hidden clears the whole trail"),BuildTrail(D).Vertices.IsEmpty());
    D.bHidden=false;D.bGuidance=false;TestTrue(TEXT("Tracking loss clears the whole trail"),BuildTrail(D).Vertices.IsEmpty());
    return true;
}
#endif
