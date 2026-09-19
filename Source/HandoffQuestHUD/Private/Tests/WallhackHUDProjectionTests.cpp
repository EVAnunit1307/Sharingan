#include "WallhackHUDProjection.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWallhackOffscreenDirectionsTest,
    "Wallhack.Spatial.Offscreen.CardinalDirections", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWallhackOffscreenDirectionsTest::RunTest(const FString& Parameters)
{
    const FVector Eye(25., -50., 170.);
    struct FCase
    {
        const TCHAR* Name;
        FVector Delta;
        FVector2D ExpectedPosition;
        FVector2D ExpectedDirection;
    };
    const FCase Cases[] =
    {
        { TEXT("Right"), FVector(0., 300., 0.), FVector2D(1874., 500.), FVector2D(1., 0.) },
        { TEXT("Left"), FVector(0., -300., 0.), FVector2D(174., 500.), FVector2D(-1., 0.) },
        { TEXT("Above"), FVector(0., 0., 300.), FVector2D(1024., 290.), FVector2D(0., -1.) },
        { TEXT("Below"), FVector(0., 0., -300.), FVector2D(1024., 710.), FVector2D(0., 1.) }
    };
    for (const FCase& Expected : Cases)
    {
        const WallhackHUDProjection::FEdgeIndicator Indicator = WallhackHUDProjection::ProjectOffscreen(Eye + Expected.Delta, Eye, FQuat::Identity);
        const FString Label(Expected.Name);
        TestTrue(Label + TEXT(" contact needs an edge cue"), Indicator.bOutside);
        TestTrue(Label + TEXT(" cue lands at the correct visor edge"), Indicator.Position.Equals(Expected.ExpectedPosition, 0.01));
        TestTrue(Label + TEXT(" arrow points toward the contact"), Indicator.Direction.Equals(Expected.ExpectedDirection, 0.001));
    }

    const WallhackHUDProjection::FEdgeIndicator Behind = WallhackHUDProjection::ProjectOffscreen(Eye + FVector(-300., 0., 0.), Eye, FQuat::Identity);
    TestTrue(TEXT("Directly behind requires a turn cue"), Behind.bOutside);
    TestTrue(TEXT("Behind cue lands on a horizontal edge"),
        FMath::IsNearlyEqual(FMath::Abs(Behind.Position.X - 1024.), 850., 0.01)
        && FMath::IsNearlyEqual(Behind.Position.Y, 500., 0.01));
    TestTrue(TEXT("Behind cue points horizontally"), FMath::IsNearlyEqual(FMath::Abs(Behind.Direction.X), 1., 0.001) && FMath::IsNearlyZero(Behind.Direction.Y, 0.001));
    TestFalse(TEXT("A point directly in front has no unnecessary cue"),
        WallhackHUDProjection::ProjectOffscreen(Eye + FVector(300., 0., 0.), Eye, FQuat::Identity).bOutside);
    TestFalse(TEXT("Coincident eye/contact has no undefined direction cue"),
        WallhackHUDProjection::ProjectOffscreen(Eye, Eye, FQuat::Identity).bOutside);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWallhackOffscreenPoseAndValidityTest,
    "Wallhack.Spatial.Offscreen.ViewerPoseAndInvalidData", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWallhackOffscreenPoseAndValidityTest::RunTest(const FString& Parameters)
{
    const FVector Eye(100., 200., 170.);
    const FQuat Turned = FRotator(0., 90., 0.).Quaternion();
    TestFalse(TEXT("Turning toward a formerly right-side contact removes its cue"),
        WallhackHUDProjection::ProjectOffscreen(Eye + FVector(0., 300., 0.), Eye, Turned).bOutside);
    const WallhackHUDProjection::FEdgeIndicator Left = WallhackHUDProjection::ProjectOffscreen(Eye + FVector(300., 0., 0.), Eye, Turned);
    TestTrue(TEXT("Formerly front contact is now offscreen left"), Left.bOutside && Left.Direction.X < -0.99);

    const FQuat Pitched = FRotator(60., 45., 0.).Quaternion();
    const FVector LookTarget = Eye + Pitched.RotateVector(FVector(300., 0., 0.));
    TestFalse(TEXT("Full camera pitch and yaw are used for a gaze-aligned contact"),
        WallhackHUDProjection::ProjectOffscreen(LookTarget, Eye, Pitched).bOutside);
    const FVector DiagonalTarget = Eye + FVector(-300., -300., 200.);
    const WallhackHUDProjection::FEdgeIndicator Diagonal = WallhackHUDProjection::ProjectOffscreen(DiagonalTarget, Eye, FQuat::Identity);
    TestTrue(TEXT("Behind-left-above contact has a finite inward-frame cue"), Diagonal.bOutside && !Diagonal.Position.ContainsNaN());
    TestTrue(TEXT("Diagonal cue points left and up"), Diagonal.Direction.X < 0. && Diagonal.Direction.Y < 0.);
    TestTrue(TEXT("Diagonal direction is normalized"), FMath::IsNearlyEqual(Diagonal.Direction.Size(), 1., 0.001));
    TestTrue(TEXT("Cue stays inside the designated visor edge rectangle"),
        Diagonal.Position.X >= 173.99 && Diagonal.Position.X <= 1874.01
        && Diagonal.Position.Y >= 289.99 && Diagonal.Position.Y <= 710.01);

    const float NaN = std::numeric_limits<float>::quiet_NaN();
    const float Infinity = std::numeric_limits<float>::infinity();
    FVector InvalidPoint = FVector::ZeroVector;
    InvalidPoint.X = NaN;
    FVector InvalidViewer = FVector::ZeroVector;
    InvalidViewer.Z = Infinity;
    FQuat InvalidOrientation = FQuat::Identity;
    InvalidOrientation.W = NaN;
    TestFalse(TEXT("Nonfinite contact never emits an arrow"), WallhackHUDProjection::ProjectOffscreen(InvalidPoint, Eye, FQuat::Identity).bOutside);
    TestFalse(TEXT("Nonfinite viewer never emits an arrow"), WallhackHUDProjection::ProjectOffscreen(DiagonalTarget, InvalidViewer, FQuat::Identity).bOutside);
    TestFalse(TEXT("Nonfinite orientation never emits an arrow"), WallhackHUDProjection::ProjectOffscreen(DiagonalTarget, Eye, InvalidOrientation).bOutside);
    TestFalse(TEXT("Non-normalized orientation is rejected"), WallhackHUDProjection::ProjectOffscreen(DiagonalTarget, Eye, FQuat(0., 0., 0., 2.)).bOutside);
    return true;
}

#endif
