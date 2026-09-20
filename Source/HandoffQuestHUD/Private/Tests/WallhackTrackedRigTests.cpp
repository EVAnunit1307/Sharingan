#include "WallhackTrackedRig.h"
#include "WallhackSpatialMath.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRigAlignmentTest,"Wallhack.SensorPeople.ControllerMountAlignment",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FRigAlignmentTest::RunTest(const FString&)
{
    FWallhackRigAlignment Alignment;FString Error;
    const FTransform Controller(FRotator(17,83,-24),FVector(1,2,1));
    const FVector RadarCentre=Controller.TransformPosition({.04,.07,-.03});
    TestTrue(TEXT("One upright origin mark aligns the laser axis"),Alignment.Align(RadarCentre,Controller,Error));
    TestTrue(TEXT("Mount calibrated"),Alignment.IsReady());
    const FTransform Initial=Alignment.Resolve(Controller);
    TestTrue(TEXT("Sensor origin uses the position mark"),Initial.GetLocation().Equals(RadarCentre,.0001));
    TestTrue(TEXT("Sensor follows aim ray rather than grip angle"),Initial.GetUnitAxis(EAxis::X).Equals(Controller.GetUnitAxis(EAxis::X),.0001));
    TestTrue(TEXT("Upright calibration removes arbitrary controller roll"),FMath::Abs(Initial.Rotator().Roll)<.001);
    const FTransform Motion(FRotator(-20,40,30),FVector(2,3,1));
    TestTrue(TEXT("Subsequent tilt and motion retain mount offset"),Alignment.Resolve(Controller*Motion).Equals(Initial*Motion,.0001));
    Alignment.Reset();TestFalse(TEXT("Recenter clears calibration"),Alignment.IsReady());
    TestFalse(TEXT("Impossible offset rejected"),Alignment.Align({2,0,0},FTransform::Identity,Error));
    TestFalse(TEXT("Vertical aim cannot resolve roll from gravity"),Alignment.Align({0,0,0},FTransform(FRotator(90,0,0)),Error));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRigHistoryTest,"Wallhack.SensorPeople.ControllerCaptureTimeHistory",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FRigHistoryTest::RunTest(const FString&)
{
    FWallhackRigHistory History;FTransform Out;
    History.Add(1,FTransform(FRotator(0,0,0),FVector(0,0,1)));
    History.Add(1.05,FTransform(FRotator(0,90,0),FVector(1,0,1)));
    TestTrue(TEXT("Capture-time interpolation"),History.Sample(1.025,Out));
    TestTrue(TEXT("Translation interpolated"),Out.GetLocation().Equals({.5,0,1},.0001));
    TestTrue(TEXT("Rotation interpolated"),FMath::Abs(Out.Rotator().Yaw-45)<.001);
    const FVector Stationary(3,2,1);
    const FVector Local=Out.InverseTransformPosition(Stationary);
    TestTrue(TEXT("Static person survives rig motion"),Out.TransformPosition(Local).Equals(Stationary,.0001));
    TestFalse(TEXT("No future extrapolation"),History.Sample(1.06,Out));
    TestFalse(TEXT("No pre-alignment samples"),History.Sample(.99,Out));
    History.Add(1.3,FTransform::Identity);
    TestFalse(TEXT("Tracking gap drops old history"),History.Sample(1.025,Out));
    History.Reset();TestFalse(TEXT("Loss clears placement immediately"),History.Sample(1.3,Out));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRigStableCalibrationTest,"Wallhack.SensorPeople.StableOriginCaptureAndReview",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FRigStableCalibrationTest::RunTest(const FString&)
{
    FWallhackRigCalibrationCapture Capture;FWallhackRigAlignment Alignment;FString Status;
    const FTransform Left(FRotator(10,83,-24),FVector(1,2,1));
    const FVector Offset(.04,.07,-.03);
    TestFalse(TEXT("No implicit startup capture"),Capture.Observe(1,Left,Left.TransformPosition(Offset),Status));
    Capture.Start();
    for(int32 I=0;I<34;++I)
    {
        const FVector Jitter(0,I%2?.002:-.002,0);
        Capture.Observe(1+I*.02,Left,Left.TransformPosition(Offset+Jitter),Status);
        if(I<33)TestFalse(TEXT("Must collect a complete stable window"),Capture.IsReady());
    }
    TestTrue(TEXT("Stable samples ready for review"),Capture.IsReady());
    TestFalse(TEXT("Capture stops when complete"),Capture.IsActive());
    TestTrue(TEXT("Millimetre probe jitter averages in mount coordinates"),Capture.GetMount().GetLocation().Equals(Offset,.00001));
    TestFalse(TEXT("Capture cannot silently accept calibration"),Alignment.IsReady());
    const FTransform Captured=Capture.GetMount();
    Capture.Observe(3,FTransform::Identity,{.9,0,0},Status);
    Capture.LoseTracking();
    TestTrue(TEXT("Review retains captured local offset without fresh samples"),Capture.GetMount().Equals(Captured));
    TestTrue(TEXT("Explicit acceptance commits offset"),Alignment.SetMount(Capture.GetMount(),Status));
    const FTransform Motion(FRotator(-16,40,30),FVector(2,3,1));
    TestTrue(TEXT("Review and accepted origin follow mounted LEFT aim through tilt and motion"),
        Alignment.Resolve(Left*Motion).Equals((Captured*Left)*Motion,.00001));
    Capture.Reset();Alignment.Reset();
    TestFalse(TEXT("Recalibration clears accepted origin"),Alignment.IsReady());
    TestFalse(TEXT("Recalibration clears pending review"),Capture.IsReady());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRigCaptureRejectionTest,"Wallhack.SensorPeople.OriginCaptureRejectsMotionAndGaps",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FRigCaptureRejectionTest::RunTest(const FString&)
{
    FWallhackRigCalibrationCapture Capture;FString Status;
    const FTransform Left(FRotator::ZeroRotator,FVector(0,0,1));
    const FVector Probe(.1,0,1);
    Capture.Start();
    for(int32 I=0;I<25;++I)Capture.Observe(1+I*.02,Left,Probe,Status);
    TestTrue(TEXT("Partial capture accumulates"),Capture.Progress()>.7f);
    Capture.Observe(1.5,Left,Probe+FVector(.02,0,0),Status);
    TestEqual(TEXT("Moving probe starts a fresh stable window"),Capture.Progress(),0.f);
    for(int32 I=1;I<25;++I)Capture.Observe(1.5+I*.02,Left,Probe+FVector(.02,0,0),Status);
    Capture.LoseTracking();
    TestEqual(TEXT("Tracking loss drops incomplete window"),Capture.Progress(),0.f);
    for(int32 I=0;I<25;++I)Capture.Observe(3+I*.02,Left,Probe,Status);
    Capture.Observe(4,Left,Probe,Status);
    TestEqual(TEXT("Unreported gap over 100 ms resets window"),Capture.Progress(),0.f);
    Capture.Observe(4,Left,Probe,Status);
    TestFalse(TEXT("Duplicate timestamps cannot finish capture"),Capture.IsReady());
    Capture.Observe(4.02,FTransform(FRotator(0,8,0),FVector(0,0,1)),Probe,Status);
    TestEqual(TEXT("Rig rotation restarts window"),Capture.Progress(),0.f);
    Capture.Observe(4.04,Left,{2,0,1},Status);
    TestEqual(TEXT("Impossible offset clears window"),Capture.Progress(),0.f);
    TestFalse(TEXT("No rejected sample can produce an accepted result"),Capture.IsReady());
    FWallhackRigAlignment Alignment;
    TestFalse(TEXT("Scaled poses rejected"),Alignment.Align(Probe,FTransform(FQuat::Identity,{0,0,1},{2,2,2}),Status));
    TestFalse(TEXT("Corrupt mount quaternion rejected"),Alignment.SetMount(FTransform(FQuat(0,0,0,0)),Status));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRigGuideFrameTest,"Wallhack.SensorPeople.CalibratedOriginAndHeadsetFrame",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FRigGuideFrameTest::RunTest(const FString&)
{
    const FTransform Left(FRotator(0,90,-25),FVector(5,1,1));
    FWallhackRigAlignment Alignment;FString Error;
    TestTrue(TEXT("Left mount accepts radar origin"),Alignment.Align({5,1.1,1},Left,Error));
    const FTransform Sensor=Alignment.Resolve(Left);
    const auto Local=BuildWallhackRigCalibrationGuides(FTransform::Identity);
    const auto World=BuildWallhackRigCalibrationGuides(Sensor);
    TestTrue(TEXT("Guide has renderable triangles"),World.Indices.Num()>0&&World.Indices.Num()%3==0);
    TestEqual(TEXT("Every guide vertex has evidence colour"),World.Colors.Num(),World.Vertices.Num());
    for(int32 I=0;I<World.Vertices.Num();++I)
        TestTrue(TEXT("Guide uses the same rig origin and forward as contacts"),World.Vertices[I].Equals(Sensor.TransformPosition(Local.Vertices[I]),.00001));
    for(int32 I:World.Indices)TestTrue(TEXT("Valid mesh index"),World.Vertices.IsValidIndex(I));
    TestTrue(TEXT("Invalid transform draws no guide"),BuildWallhackRigCalibrationGuides(FTransform(FQuat(0,0,0,0))).Vertices.IsEmpty());
    const FVector Person=Sensor.TransformPosition({2,0,-1});
    for(float Scale:{50.f,100.f,200.f})
    {
        WallhackSpatialMath::FContactView View;
        WallhackSpatialMath::ProjectContact(Person*Scale,FVector(5,2.1,1.7)*Scale,90,Scale,View);
        TestTrue(TEXT("Map remains at headset, one metre from person, not at rig"),FMath::IsNearlyEqual(View.GroundRangeMeters,1.f,.001f));
        WallhackSpatialMath::ProjectContact(Person*Scale,FVector(5,2.1,1.7)*Scale,0,Scale,View);
        TestTrue(TEXT("Turning headset rotates map independently of mounted controller"),FMath::IsNearlyEqual(View.RightMeters,1.f,.001f));
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRigEstimatedCaptureTest,"Wallhack.SensorPeople.UnheldCalibrationRetainsEstimateEvidence",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FRigEstimatedCaptureTest::RunTest(const FString&)
{
    FWallhackRigCalibrationCapture Capture;FString Status;
    const FTransform Left(FRotator::ZeroRotator,{0,0,1});
    Capture.Start();
    for(int32 I=0;I<34;++I)Capture.Observe(1+I*.02,Left,{.1,0,1},Status,I>=10&&I<=25);
    TestTrue(TEXT("Quality transition does not interrupt otherwise stable capture"),Capture.IsReady());
    TestTrue(TEXT("Calibration remembers the estimated input after optical recovery"),Capture.IsEstimated());
    Capture.LoseTracking();
    TestTrue(TEXT("Review cannot erase estimate evidence"),Capture.IsEstimated());
    Capture.Start();
    for(int32 I=0;I<20;++I)Capture.Observe(3+I*.02,Left,{.1,0,1},Status,true);
    Capture.LoseTracking();
    for(int32 I=0;I<34;++I)Capture.Observe(4+I*.02,Left,{.1,0,1},Status,false);
    TestTrue(TEXT("A completely new measured window can calibrate normally"),Capture.IsReady()&&!Capture.IsEstimated());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRigEstimatedHistoryTest,"Wallhack.SensorPeople.RigHistoryPreservesRuntimePoseQuality",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FRigEstimatedHistoryTest::RunTest(const FString&)
{
    FWallhackRigHistory History;FTransform Out;bool Estimated=false;
    History.Add(1,FTransform::Identity,false);
    History.Add(1.05,FTransform(FVector(.1,0,0)),true);
    History.Add(1.1,FTransform(FVector(.2,0,0)),false);
    TestTrue(TEXT("Exact measured sample stays measured"),History.Sample(1,Out,&Estimated)&&!Estimated);
    TestTrue(TEXT("Interpolation into estimate retains uncertainty"),History.Sample(1.025,Out,&Estimated)&&Estimated);
    TestTrue(TEXT("Exact estimated sample stays estimated"),History.Sample(1.05,Out,&Estimated)&&Estimated);
    TestTrue(TEXT("Interpolation out of estimate retains uncertainty"),History.Sample(1.075,Out,&Estimated)&&Estimated);
    TestTrue(TEXT("Later measured sample recovers quality"),History.Sample(1.1,Out,&Estimated)&&!Estimated);
    History.Reset();
    TestFalse(TEXT("No estimate survives true pose loss in app cache"),History.Sample(1.05,Out,&Estimated));
    return true;
}
#endif
