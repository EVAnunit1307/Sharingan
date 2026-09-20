#include "WallhackControllerPose.h"
#include "WallhackMountedController.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FControllerPoseValidityTest,"Wallhack.SensorPeople.PhysicalControllerPoseValidity",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FControllerPoseValidityTest::RunTest(const FString&)
{
    FXRMotionControllerState State;
    FTransform Out;
    TestFalse(TEXT("Missing pose is rejected"), WallhackControllerPose::Resolve(State,Out));
    State.bValid=true; State.DeviceName=TEXT("/interaction_profiles/oculus/touch_controller");
    State.ControllerLocation={100,200,130}; State.ControllerRotation=FRotator(15,72,-20).Quaternion();
    TestFalse(TEXT("Previously valid but now untracked is rejected"), WallhackControllerPose::Resolve(State,Out));
    State.TrackingStatus=ETrackingStatus::InertialOnly;
    TestFalse(TEXT("Orientation-only pose is rejected"), WallhackControllerPose::Resolve(State,Out));
    State.TrackingStatus=ETrackingStatus::Tracked;
    TestTrue(TEXT("Current six-degree-of-freedom controller accepted"), WallhackControllerPose::Resolve(State,Out));
    TestTrue(TEXT("World transform is not corrected a second time"), Out.GetLocation().Equals(State.ControllerLocation));
    TestTrue(TEXT("Aim direction and roll retained"), Out.GetRotation().Equals(State.ControllerRotation));
    State.DeviceName=TEXT("/interaction_profiles/ext/hand_interaction_ext");
    TestFalse(TEXT("Hand pose cannot substitute for mounted controller"), WallhackControllerPose::Resolve(State,Out));
    State.DeviceName=TEXT("/interaction_profiles/oculus/touch_controller");
    State.ControllerRotation=FQuat(0,0,0,0);
    TestFalse(TEXT("Unset quaternion is rejected"), WallhackControllerPose::Resolve(State,Out));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMountedControllerPoseTest,"Wallhack.SensorPeople.MountedControllerPoseContinuity",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FMountedControllerPoseTest::RunTest(const FString&)
{
    using namespace WallhackMountedController;
    FAimSample Held;
    Held.bFocused=Held.bActive=Held.bPositionValid=Held.bOrientationValid=true;
    Held.bPositionTracked=Held.bOrientationTracked=true;
    Held.TrackingPose=FTransform(FRotator(12,30,-9),FVector(80,25,140));
    const FTransform WorldFrame(FRotator(0,70,0),FVector(100,-40,200));
    FTransform HeldWorld,DetachedWorld;
    TestTrue(TEXT("Held controller has a usable physical aim"),Resolve(Held,WorldFrame,HeldWorld));
    // Classification switches input paths, not aim origins. The same mount
    // calibration must continue to place the radar without a jump/re-alignment.
    FAimSample Detached=Held;
    TestTrue(TEXT("Unheld controller remains usable"),Resolve(Detached,WorldFrame,DetachedWorld));
    const FTransform RadarOffset(FRotator(0,0,8),FVector(3.2,12,1.4));
    TestTrue(TEXT("Held/unheld switch preserves calibrated radar transform"),
        (RadarOffset*HeldWorld).Equals(RadarOffset*DetachedWorld,0.001));
    TestTrue(TEXT("Room transform applied once"),HeldWorld.GetLocation().Equals(
        WorldFrame.TransformPosition(Held.TrackingPose.GetLocation()),0.001));
    Detached.TrackingPose.AddToTranslation(FVector(15,-5,7));
    TestTrue(TEXT("Unheld motion remains live"),Resolve(Detached,WorldFrame,DetachedWorld));
    TestTrue(TEXT("Motion follows rotated room axes"),(DetachedWorld.GetLocation()-HeldWorld.GetLocation()).Equals(
        WorldFrame.TransformVector(FVector(15,-5,7)),0.001));

    for(bool FAimSample::* Flag : {&FAimSample::bFocused,&FAimSample::bActive,
        &FAimSample::bPositionValid,&FAimSample::bOrientationValid,
        &FAimSample::bPositionTracked,&FAimSample::bOrientationTracked})
    {
        FAimSample Lost=Detached; Lost.*Flag=false;
        TestFalse(TEXT("Each missing tracking/focus condition suppresses the pose"),Resolve(Lost,WorldFrame,DetachedWorld));
        TestTrue(TEXT("A missing pose cannot return the preceding frame"),DetachedWorld.Equals(FTransform::Identity));
    }
    Detached.TrackingPose.SetRotation(FQuat(0,0,0,0));
    TestFalse(TEXT("Invalid detached rotation rejected"),Resolve(Detached,WorldFrame,DetachedWorld));
    TestTrue(TEXT("Recovery requires a fresh fully tracked pose"),Resolve(Held,WorldFrame,HeldWorld));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMountedAvailablePoseTest,"Wallhack.SensorPeople.MountedRuntimePoseDoesNotRequireHeldOrOpticalFlag",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FMountedAvailablePoseTest::RunTest(const FString&)
{
    using namespace WallhackMountedController;
    FAimSample Sample;Sample.bFocused=Sample.bActive=Sample.bPositionValid=Sample.bOrientationValid=true;
    Sample.bOrientationTracked=true;
    const FTransform Frame(FRotator(0,70,0),FVector(100,200,0));
    FTransform Out;bool Estimated=false;
    for(int32 I=0;I<240;++I)
    {
        // Replay the reported 15 -> 7 -> 15 quality changes. The same physical
        // pose is available from held or detached bindings; no grip/button gate.
        Sample.bPositionTracked=I<20||I>220;
        Sample.TrackingPose=FTransform(FRotator(0,I*.1,0),FVector(80+I*.1,25,140));
        TestTrue(TEXT("Every valid runtime sample remains available"),ResolveAvailable(Sample,Frame,Out,Estimated));
        TestEqual(TEXT("Estimate classification is independent of pose availability"),Estimated,!Sample.bPositionTracked);
        TestTrue(TEXT("Use each new runtime position, never an app-held last-good pose"),
            Out.GetLocation().Equals(Frame.TransformPosition(Sample.TrackingPose.GetLocation()),.00001));
    }
    Sample.bOrientationTracked=false;
    TestTrue(TEXT("Valid inferred orientation remains explicitly estimated"),ResolveAvailable(Sample,Frame,Out,Estimated)&&Estimated);
    for(bool FAimSample::* Flag:{&FAimSample::bFocused,&FAimSample::bActive,&FAimSample::bPositionValid,&FAimSample::bOrientationValid})
    {
        auto Missing=Sample;Missing.*Flag=false;
        TestFalse(TEXT("Missing/focus-lost runtime pose is never fabricated"),ResolveAvailable(Missing,Frame,Out,Estimated));
        TestTrue(TEXT("Failure clears pose and estimate flag"),Out.Equals(FTransform::Identity)&&!Estimated);
    }
    Sample.TrackingPose.SetRotation(FQuat(0,0,0,0));
    TestFalse(TEXT("Corrupt pose is rejected even in continuity mode"),ResolveAvailable(Sample,Frame,Out,Estimated));
    return true;
}
#endif
