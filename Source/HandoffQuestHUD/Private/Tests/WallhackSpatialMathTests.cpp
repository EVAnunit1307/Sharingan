#include "WallhackSpatialMath.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include <limits>

using namespace WallhackSpatialMath;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWallhackSpatialDirectionsTest,
    "Wallhack.Spatial.CardinalDirections", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWallhackSpatialDirectionsTest::RunTest(const FString& Parameters)
{
    struct FCase
    {
        FVector World;
        float Forward;
        float Right;
        float Bearing;
        FVector2D Pixel;
    };
    const FCase Cases[] =
    {
        { FVector(300., 0., 160.), 3.f, 0.f, 0.f, FVector2D(0., -30.) },
        { FVector(0., 300., 160.), 0.f, 3.f, 90.f, FVector2D(30., 0.) },
        { FVector(-300., 0., 160.), -3.f, 0.f, 180.f, FVector2D(0., 30.) },
        { FVector(0., -300., 160.), 0.f, -3.f, -90.f, FVector2D(-30., 0.) }
    };
    for (int32 Index = 0; Index < UE_ARRAY_COUNT(Cases); ++Index)
    {
        const FCase& Expected = Cases[Index];
        const FString Label = FString::Printf(TEXT("Direction %d"), Index);
        FContactView Contact;
        TestTrue(Label + TEXT(" is valid"), ProjectContact(Expected.World, FVector(0., 0., 160.), 0.f, 100.f, Contact));
        TestTrue(Label + TEXT(" forward metres"), FMath::IsNearlyEqual(Contact.ForwardMeters, Expected.Forward));
        TestTrue(Label + TEXT(" right metres"), FMath::IsNearlyEqual(Contact.RightMeters, Expected.Right));
        TestTrue(Label + TEXT(" range is 3 m"), FMath::IsNearlyEqual(Contact.RangeMeters, 3.f));
        TestTrue(Label + TEXT(" signed bearing"), FMath::IsNearlyZero(FMath::FindDeltaAngleDegrees(Contact.BearingDegrees, Expected.Bearing)));
        TestTrue(Label + TEXT(" map location"), MapOffset(Contact, 100.f, 10.f).Equals(Expected.Pixel, 0.001));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWallhackSpatialWorldAnchorTest,
    "Wallhack.Spatial.WorldAnchorAndViewerMotion", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWallhackSpatialWorldAnchorTest::RunTest(const FString& Parameters)
{
    const FVector InitialEye(125., -240., 170.);
    const FVector Anchor = PlaceAhead(InitialEye, FRotator(60., 90., 15.).Quaternion());
    TestTrue(TEXT("Pitch does not shorten the default 3 m horizontal placement"), Anchor.Equals(FVector(125., 60., 170.), 0.001));

    FContactView Contact;
    TestTrue(TEXT("Initial anchor projects"), ProjectContact(Anchor, InitialEye, 90.f, 100.f, Contact));
    TestTrue(TEXT("Anchor initially 3 m ahead"), FMath::IsNearlyEqual(Contact.ForwardMeters, 3.f, 0.001f));
    const FVector MovedEye = InitialEye + FVector(0., 100., 0.);
    TestTrue(TEXT("Moved viewer projects against the same world anchor"), ProjectContact(Anchor, MovedEye, 90.f, 100.f, Contact));
    TestTrue(TEXT("Walking 1 m toward the anchor reduces range to 2 m"), FMath::IsNearlyEqual(Contact.RangeMeters, 2.f, 0.001f));
    TestTrue(TEXT("Turning around leaves the same anchor behind"), ProjectContact(Anchor, MovedEye, 270.f, 100.f, Contact));
    TestTrue(TEXT("World anchor is 2 m behind after turning"), FMath::IsNearlyEqual(Contact.ForwardMeters, -2.f, 0.001f));
    TestTrue(TEXT("Behind marker is below map centre"), MapOffset(Contact, 100.f, 10.f).Equals(FVector2D(0., 20.), 0.001));

    const FVector VerticalLookAnchor = PlaceAhead(InitialEye, FRotator(90., 90., 0.).Quaternion());
    TestTrue(TEXT("Vertical look still produces a horizontal 3 m placement"), FMath::IsNearlyEqual((VerticalLookAnchor - InitialEye).Size(), 300., 0.001));
    TestTrue(TEXT("Vertical look preserves eye height"), FMath::IsNearlyEqual(VerticalLookAnchor.Z, InitialEye.Z, 0.001));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWallhackSpatialReferenceFrameTest,
    "Wallhack.Spatial.YawWrapAndCompassIsolation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWallhackSpatialReferenceFrameTest::RunTest(const FString& Parameters)
{
    const float EquivalentYaws[] = { 90.f, 450.f, -270.f, 810.f };
    for (const float WorldYaw : EquivalentYaws)
    {
        FContactView Contact;
        TestTrue(TEXT("Wrapped world yaw projects"), ProjectContact(FVector(0., 300., 0.), FVector::ZeroVector, WorldYaw, 100.f, Contact));
        TestTrue(TEXT("Whole turns preserve forward range"), FMath::IsNearlyEqual(Contact.ForwardMeters, 3.f, 0.001f));
        TestTrue(TEXT("Whole turns preserve centred marker"), MapOffset(Contact, 100.f, 10.f).Equals(FVector2D(0., -30.), 0.001));
    }

    // The north label is presentation calibration. Geometry is invariant under a
    // common rotation of contact and viewer; callers must supply physical world yaw.
    const float WorldFrameRotations[] = { 0.f, 37.f, -123.f, 180.f };
    for (const float Rotation : WorldFrameRotations)
    {
        const FVector RotatedContact = FRotator(0.f, Rotation, 0.f).RotateVector(FVector(300., 400., 120.));
        FContactView Contact;
        TestTrue(TEXT("Rotated world frame projects without a compass calibration input"),
            ProjectContact(RotatedContact, FVector::ZeroVector, Rotation, 100.f, Contact));
        TestTrue(TEXT("Physical forward component remains 3 m"), FMath::IsNearlyEqual(Contact.ForwardMeters, 3.f, 0.001f));
        TestTrue(TEXT("Physical right component remains 4 m"), FMath::IsNearlyEqual(Contact.RightMeters, 4.f, 0.001f));
        TestTrue(TEXT("Ground marker is independent of world north label"), MapOffset(Contact, 100.f, 10.f).Equals(FVector2D(40., -30.), 0.001));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWallhackSpatialScaleAndHeightTest,
    "Wallhack.Spatial.WorldScaleAndHeight", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWallhackSpatialScaleAndHeightTest::RunTest(const FString& Parameters)
{
    const float Scales[] = { 50.f, 100.f, 200.f };
    for (const float WorldToMeters : Scales)
    {
        const FVector Eye(10., 20., 30.);
        FContactView Contact;
        const FVector Anchor = PlaceAhead(Eye, FQuat::Identity, 3.f, WorldToMeters);
        TestTrue(TEXT("Placement honors world-to-metres scale"), Anchor.Equals(Eye + FVector(3. * WorldToMeters, 0., 0.), 0.001));
        TestTrue(TEXT("3-4-12 triangle projects at each world scale"),
            ProjectContact(Eye + FVector(3., 4., 12.) * WorldToMeters, Eye, 0.f, WorldToMeters, Contact));
        TestTrue(TEXT("Ground range is 5 m"), FMath::IsNearlyEqual(Contact.GroundRangeMeters, 5.f, 0.001f));
        TestTrue(TEXT("Slant range is 13 m"), FMath::IsNearlyEqual(Contact.RangeMeters, 13.f, 0.001f));
        TestTrue(TEXT("Height is 12 m above eye"), FMath::IsNearlyEqual(Contact.HeightMeters, 12.f, 0.001f));
        TestTrue(TEXT("Height does not push marker beyond its ground position"), MapOffset(Contact, 100.f, 10.f).Equals(FVector2D(40., -30.), 0.001));
    }

    FContactView Below;
    TestTrue(TEXT("Contact directly below projects"), ProjectContact(FVector(0., 0., -200.), FVector::ZeroVector, 45.f, 100.f, Below));
    TestTrue(TEXT("Below is negative height"), FMath::IsNearlyEqual(Below.HeightMeters, -2.f));
    TestTrue(TEXT("Vertical contact has zero ground range"), FMath::IsNearlyZero(Below.GroundRangeMeters));
    TestTrue(TEXT("Vertical contact remains at map centre"), MapOffset(Below, 100.f, 10.f).IsNearlyZero());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWallhackSpatialMapClampTest,
    "Wallhack.Spatial.MinimapClamping", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWallhackSpatialMapClampTest::RunTest(const FString& Parameters)
{
    FContactView Far;
    TestTrue(TEXT("Distant contact projects"), ProjectContact(FVector(3000., 4000., 0.), FVector::ZeroVector, 0.f, 100.f, Far));
    const FVector2D Pixel = MapOffset(Far, 100.f, 10.f);
    TestTrue(TEXT("Distant marker clamps to edge"), FMath::IsNearlyEqual(Pixel.Size(), 100., 0.001));
    TestTrue(TEXT("Clamping preserves 3:4 bearing"), Pixel.Equals(FVector2D(80., -60.), 0.001));
    TestTrue(TEXT("Zero map range is rejected"), MapOffset(Far, 100.f, 0.f).IsNearlyZero());
    TestTrue(TEXT("Negative map radius is rejected"), MapOffset(Far, -100.f, 10.f).IsNearlyZero());
    TestTrue(TEXT("NaN map range is rejected"), MapOffset(Far, 100.f, std::numeric_limits<float>::quiet_NaN()).IsNearlyZero());
    Far.RightMeters = std::numeric_limits<float>::infinity();
    TestTrue(TEXT("Nonfinite contact cannot poison map drawing"), MapOffset(Far, 100.f, 10.f).IsNearlyZero());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWallhackSpatialInvalidDataTest,
    "Wallhack.Spatial.InvalidData", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWallhackSpatialInvalidDataTest::RunTest(const FString& Parameters)
{
    const float NaN = std::numeric_limits<float>::quiet_NaN();
    const float Infinity = std::numeric_limits<float>::infinity();
    const float InvalidScales[] = { 0.f, -100.f, NaN, Infinity };
    const FVector Eye(25., 50., 160.);
    for (const float InvalidScale : InvalidScales)
    {
        FContactView Contact;
        Contact.RangeMeters = 99.f;
        TestFalse(TEXT("Invalid world scale fails projection"), ProjectContact(FVector(300., 0., 0.), Eye, 0.f, InvalidScale, Contact));
        TestTrue(TEXT("Failed projection clears stale range"), FMath::IsNearlyZero(Contact.RangeMeters));
        TestTrue(TEXT("Failed projection clears stale map position"), MapOffset(Contact, 100.f, 10.f).IsNearlyZero());
        TestTrue(TEXT("Invalid placement scale preserves viewer"), PlaceAhead(Eye, FQuat::Identity, 3.f, InvalidScale).Equals(Eye));
    }
    FContactView Contact;
    // Set malformed components after construction to exercise our input guard
    // without triggering the engine constructors' own diagnostic NaN checks.
    FVector NaNContact = FVector::ZeroVector;
    NaNContact.X = NaN;
    FVector InfiniteViewer = FVector::ZeroVector;
    InfiniteViewer.Y = Infinity;
    FQuat InvalidOrientation = FQuat::Identity;
    InvalidOrientation.X = NaN;
    TestFalse(TEXT("NaN world contact rejected"), ProjectContact(NaNContact, Eye, 0.f, 100.f, Contact));
    TestFalse(TEXT("Infinite viewer rejected"), ProjectContact(FVector::ZeroVector, InfiniteViewer, 0.f, 100.f, Contact));
    TestFalse(TEXT("NaN viewer yaw rejected"), ProjectContact(FVector::ZeroVector, Eye, NaN, 100.f, Contact));
    TestTrue(TEXT("Invalid orientation preserves viewer"), PlaceAhead(Eye, InvalidOrientation).Equals(Eye));
    TestTrue(TEXT("Zero quaternion preserves viewer"), PlaceAhead(Eye, FQuat(0., 0., 0., 0.)).Equals(Eye));
    TestTrue(TEXT("Invalid range preserves viewer"), PlaceAhead(Eye, FQuat::Identity, Infinity).Equals(Eye));
    TestTrue(TEXT("Invalid viewer produces finite fallback"), PlaceAhead(NaNContact, FQuat::Identity).IsNearlyZero());
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
