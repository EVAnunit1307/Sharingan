#include "WallhackNavigationPresentation.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMinimapProjectionTest,"Wallhack.Navigation.Minimap.HeadingRangeAndCircularClipping",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FMinimapProjectionTest::RunTest(const FString&)
{
    using namespace WallhackNavPresentation;
    FLocalMap M;M.Viewer={10,-4,1.7};M.Yaw=90;M.Range=5;M.Center={0,0};M.Radius=100;
    TestTrue(TEXT("Forward remains up after a quarter turn"),M.Project({10,1,0}).Equals({0,-100},.001));
    TestTrue(TEXT("World north moves left after turning east"),M.Project({15,-4,0}).Equals({-100,0},.001));
    TestTrue(TEXT("Map ignores floor elevation"),M.Project({10,1,30}).Equals(M.Project({10,1,0}),.001));
    TestTrue(TEXT("Relative N rotates to the left"),M.Compass(0,0,100).Equals({-100,0},.001));
    TestTrue(TEXT("Rezeroing compass puts N above without moving geometry"),M.Compass(0,-90,100).Equals({0,-100},.001));
    const auto Near=ProjectMarker(M,{10,-1.5,0}),Far=ProjectMarker(M,{10,6,0});
    TestFalse(TEXT("Nearby person is a dot"),Near.bOutside);
    TestTrue(TEXT("Distant person is an outward rim arrow"),Far.bOutside&&Far.Direction.Equals({0,-1},.001)&&Far.Position.Size()<=88.001);
    const auto Coincident=ProjectMarker(M,M.Viewer);
    TestTrue(TEXT("Coincident markers remain finite at self"),Coincident.Position.IsNearlyZero()&&!Coincident.bOutside);
    FVector2D A(-200,0),B(200,0);
    TestTrue(TEXT("Wall crossing circle survives even with both endpoints outside"),ClipToCircle(A,B,{0,0},100));
    TestTrue(TEXT("Crossing wall clips exactly to rim"),A.Equals({-100,0},.001)&&B.Equals({100,0},.001));
    A={-200,120};B={200,120};TestFalse(TEXT("Outside wall is discarded"),ClipToCircle(A,B,{0,0},100));
    A={0,0};B={200,200};TestTrue(TEXT("Route ending outside is clipped"),ClipToCircle(A,B,{0,0},100));
    TestTrue(TEXT("Diagonal route remains inside circle"),FMath::IsNearlyEqual(B.Size(),100.,.001));
    A={0,0};B=A;TestTrue(TEXT("Zero-length interior segment is finite"),ClipToCircle(A,B,{0,0},100));
    A.X=std::numeric_limits<double>::quiet_NaN();TestFalse(TEXT("Invalid geometry is rejected"),ClipToCircle(A,B,{0,0},100));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavigationDirectionTest,"Wallhack.Navigation.Guidance.RouteDirectionAndUnavailableState",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FNavigationDirectionTest::RunTest(const FString&)
{
    using namespace WallhackNavPresentation;
    using namespace WallhackNav;
    FDisplaySnapshot D;D.bGuidance=D.bHasTarget=true;D.Viewer={0,0,1.7};D.Target.Standing={2,0,0};
    D.Route.Points={{{0,0,0},false},{{0,2,0},false},{{2,2,0},true},{{2,0,0},true}};
    D.Route.bComplete=true;D.State=ERouteState::Estimated;
    auto Cue=BuildDirectionCue(D);
    TestTrue(TEXT("Direction follows the first reachable leg, not a shortcut straight to target"),Cue.Kind==EDirectionCue::Route&&Cue.Direction.Equals({1,0},.001));
    TestFalse(TEXT("Distant unknown sections do not recolor the observed next step"),Cue.bEstimated);
    D.Orientation=FRotator(70,90,25).Quaternion();Cue=BuildDirectionCue(D);
    TestTrue(TEXT("Looking down/rolling does not lose the heading cue"),Cue.Direction.Equals({0,-1},.001));
    D.Route.Points[1].bEstimated=true;TestTrue(TEXT("Unknown next step is explicit"),BuildDirectionCue(D).bEstimated);
    D.Route.bComplete=false;TestTrue(TEXT("Reachable prefix is explicitly partial"),BuildDirectionCue(D).bPartial);
    D.Route.Points.Reset();D.State=ERouteState::StartBlocked;
    TestTrue(TEXT("Blocked path retains an explicitly target-only bearing"),BuildDirectionCue(D).Kind==EDirectionCue::TargetDirection);
    D.State=ERouteState::Planning;TestTrue(TEXT("Planning cannot make the target cue vanish"),BuildDirectionCue(D).Kind==EDirectionCue::TargetDirection);
    D.State=ERouteState::Arrived;TestTrue(TEXT("Arrival is not another walking instruction"),BuildDirectionCue(D).Kind==EDirectionCue::Arrived);
    D.bHidden=true;TestTrue(TEXT("Hidden removes the cue"),BuildDirectionCue(D).Kind==EDirectionCue::Hidden);
    D.bHidden=false;D.bGuidance=false;TestTrue(TEXT("Lost tracking removes the cue"),BuildDirectionCue(D).Kind==EDirectionCue::Hidden);
    D.bGuidance=true;D.bHasTarget=false;TestTrue(TEXT("Cancellation removes the cue"),BuildDirectionCue(D).Kind==EDirectionCue::Hidden);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompassTapeTest,"Wallhack.Navigation.Compass.WrapAndRelativeCalibration",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FCompassTapeTest::RunTest(const FString&)
{
    using namespace WallhackNavPresentation;
    TestEqual(TEXT("North is one degree right at 359"),CompassDelta(0,359,0),1.f);
    TestEqual(TEXT("North moves smoothly left through zero"),CompassDelta(0,1,0),-1.f);
    TestEqual(TEXT("Rounded heading wraps to 000 instead of 360"),HeadingNumber(359.8,0),0);
    TestEqual(TEXT("Relative calibration moves north to the centre"),CompassDelta(0,95,-95),0.f);
    TestEqual(TEXT("East remains right of calibrated north"),CompassDelta(90,95,-95),90.f);
    TestEqual(TEXT("West appears to the left"),CompassDelta(270,0,0),-90.f);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompassPeopleTest,"Wallhack.Navigation.Compass.PersonBearingsAndCrowding",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FCompassPeopleTest::RunTest(const FString&)
{
    using namespace WallhackNavPresentation;
    TArray<FWallhackPersonPose> People={{1,{3,0,0},1.75f,0},{2,{-3,0,0},1.75f,0},{3,{0,3,0},1.75f,0}};
    auto Pins=CompassPeople(People,{0,0,1.7},0);
    const auto* Ahead=Pins.FindByPredicate([](const FCompassPerson& P){return P.Id==1;});
    TestTrue(TEXT("Ahead person sits under the fixed pointer"),Ahead&&!Ahead->bOutside&&FMath::IsNearlyEqual(Ahead->AnchorX,1024.f));
    TestTrue(TEXT("Behind and side contacts become explicit rim arrows"),Pins[1].bOutside&&Pins[2].bOutside);
    Pins=CompassPeople(People,{0,0,20},90);
    Ahead=Pins.FindByPredicate([](const FCompassPerson& P){return P.Id==1;});
    TestTrue(TEXT("Turning east sends the north person to the left rim"),Ahead&&Ahead->bOutside&&Ahead->Delta<0&&Ahead->AnchorX==604);
    Pins=CompassPeople(People,{0,0,1.7},359);
    Ahead=Pins.FindByPredicate([](const FCompassPerson& P){return P.Id==1;});
    TestTrue(TEXT("Person bearing crosses north continuously"),Ahead&&FMath::IsNearlyEqual(Ahead->Delta,1.f));
    People.Reset();for(int32 I=0;I<8;++I)People.Add({I+1,{-3,0,0},1.75f,0});
    Pins=CompassPeople(People,{0,0,1.7},0);
    TestEqual(TEXT("Eight colocated people retain individual IDs"),Pins.Num(),8);
    for(int32 I=0;I<Pins.Num();++I)
    {
        TestTrue(TEXT("Crowded labels remain inside the heading bar"),Pins[I].LabelX>=624&&Pins[I].LabelX<=1424);
        if(I)TestTrue(TEXT("Crowded labels do not overlap"),Pins[I].LabelX-Pins[I-1].LabelX>=39.99f);
        TestEqual(TEXT("Leaders retain the actual rim bearing"),Pins[I].AnchorX,1444.f);
    }
    People={{1,{0,0,0},1.75f,0}};
    TestTrue(TEXT("Coincident people do not invent a compass bearing"),CompassPeople(People,{0,0,1.7},0).IsEmpty());
    return true;
}
#endif
