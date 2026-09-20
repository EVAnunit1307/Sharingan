#include "WallhackSensorPeopleTypes.h"
#include "WallhackSpatialMath.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Json.h"

namespace
{
FString ContractPacket(int32 Sequence = 1)
{
    FString Text;
    const FString Path = FPaths::Combine(FPaths::ProjectDir(), TEXT("GroundStation/tests/fixtures/spatial_people.json"));
    if (!FFileHelper::LoadFileToString(Text, *Path)) return FString();
    TSharedPtr<FJsonObject> Root;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root)) return FString();
    Root->SetNumberField(TEXT("relay_sequence"), Sequence);
    FString Out;
    FJsonSerializer::Serialize(Root.ToSharedRef(), TJsonWriterFactory<>::Create(&Out));
    return Out;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWallhackSensorPeopleProtocolTest,
    "Wallhack.SensorPeople.ProtocolAndExpiry", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWallhackSensorPeopleProtocolTest::RunTest(const FString&)
{
    FWallhackSensorPeopleStream Stream;
    if (!TestTrue(TEXT("Actual Python relay contract fixture parses"), Stream.Ingest(ContractPacket(), 100))) return false;
    auto Frame = Stream.GetFrame(100);
    if (!TestEqual(TEXT("One person"), Frame.People.Num(), 1)) return false;
    TestTrue(TEXT("Recorded fixture is labelled"), Frame.bReplay);
    TestTrue(TEXT("Starts radar matched"), Frame.People[0].bRadar);
    TestEqual(TEXT("Radar position is 3 m forward"), Frame.People[0].Position.Y, 3.0);
    TestFalse(TEXT("Duplicate relay sequence rejected"), Stream.Ingest(ContractPacket(), 100.2));
    TestTrue(TEXT("A new relay packet can repeat a sensor frame"), Stream.Ingest(ContractPacket(2), 100.4));
    Frame = Stream.GetFrame(100.501);
    TestEqual(TEXT("Fresh camera remains after radar expiry"), Frame.People.Num(), 1);
    if (Frame.People.Num() == 1)
    {
        TestFalse(TEXT("No radar age renewal from repeated frame"), Frame.People[0].bRadar);
        TestEqual(TEXT("Fallback is 2 m forward"), Frame.People[0].Position.Y, 2.0);
        TestEqual(TEXT("Identity survives fallback"), Frame.People[0].Id, 1);
    }
    TestEqual(TEXT("Camera expires even with repeated traffic"), Stream.GetFrame(100.751).People.Num(), 0);
    Stream.Reset();
    TestEqual(TEXT("Disconnect clears all positions"), Stream.GetFrame(100).People.Num(), 0);

    // Radar-only input must work without a camera frame, and relay traffic must
    // not renew the radar observation. Camera and radar IDs are separate domains.
    TSharedPtr<FJsonObject> Root;
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ContractPacket()), Root);
    const auto E = Root->GetObjectField(TEXT("spatial_people"));
    E->SetArrayField(TEXT("people"), {});
    E->SetField(TEXT("camera_age_ms"), MakeShared<FJsonValueNull>());
    auto Radar = MakeShared<FJsonObject>();
    Radar->SetNumberField(TEXT("id"), 1);
    Radar->SetNumberField(TEXT("right_m"), .25);
    Radar->SetNumberField(TEXT("forward_m"), 3);
    Radar->SetNumberField(TEXT("generation"), 1);
    Radar->SetNumberField(TEXT("frame_id"), 1);
    Radar->SetNumberField(TEXT("age_ms"), 100);
    E->SetArrayField(TEXT("radar_targets"), {MakeShared<FJsonValueObject>(Radar)});
    FString RadarPacket;
    FJsonSerializer::Serialize(Root.ToSharedRef(), TJsonWriterFactory<>::Create(&RadarPacket));
    TestTrue(TEXT("Radar-only packet accepts absent camera age"), Stream.Ingest(RadarPacket, 200));
    TestEqual(TEXT("Fresh radar exists independently"), Stream.GetFrame(200).Radar.Num(), 1);
    Root->SetNumberField(TEXT("relay_sequence"), 2);
    RadarPacket.Reset();
    FJsonSerializer::Serialize(Root.ToSharedRef(), TJsonWriterFactory<>::Create(&RadarPacket));
    TestTrue(TEXT("Repeated radar packet accepted"), Stream.Ingest(RadarPacket, 200.3));
    TestEqual(TEXT("Repeated frame cannot extend radar expiry"), Stream.GetFrame(200.401).Radar.Num(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWallhackSensorPeopleInvalidPacketTest,
    "Wallhack.SensorPeople.InvalidAndEmptyFrames", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWallhackSensorPeopleInvalidPacketTest::RunTest(const FString&)
{
    FWallhackSensorPeopleStream Stream;
    if (!TestTrue(TEXT("Fixture received"), Stream.Ingest(ContractPacket(), 0))) return false;
    TSharedPtr<FJsonObject> Root;
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ContractPacket(2)), Root);
    const auto E = Root->GetObjectField(TEXT("spatial_people"));
    E->SetStringField(TEXT("units"), TEXT("mm"));
    FString Invalid;
    FJsonSerializer::Serialize(Root.ToSharedRef(), TJsonWriterFactory<>::Create(&Invalid));
    TestFalse(TEXT("Wrong units rejected"), Stream.Ingest(Invalid, .7));
    TestEqual(TEXT("Invalid packet did not extend expiry"), Stream.GetFrame(.751).People.Num(), 0);
    E->SetStringField(TEXT("units"), TEXT("m"));
    E->SetArrayField(TEXT("people"), TArray<TSharedPtr<FJsonValue>>());
    E->SetNumberField(TEXT("camera_frame_id"), 3);
    FString Empty;
    FJsonSerializer::Serialize(Root.ToSharedRef(), TJsonWriterFactory<>::Create(&Empty));
    TestTrue(TEXT("Empty snapshot accepted"), Stream.Ingest(Empty, .2));
    TestEqual(TEXT("Missing people removed immediately"), Stream.GetFrame(.2).People.Num(), 0);
    TestFalse(TEXT("New relay sequence cannot revive older camera frame"), Stream.Ingest(ContractPacket(3), .3));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWallhackSensorPeopleRegistrationMathTest,
    "Wallhack.SensorPeople.ReferenceAndViewerMotion", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWallhackSensorPeopleRegistrationMathTest::RunTest(const FString&)
{
    for (const float Scale : {50.f, 100.f, 200.f})
    {
        const FTransform Reference(FRotator(0,90,0), FVector(5,1,0) * Scale);
        const FVector Contact = WallhackSensorPeopleMath::ToWorld(FVector2D(0,2), Reference, Scale);
        TestTrue(TEXT("Nonzero reference and 90-degree yaw"), Contact.Equals(FVector(5,3,0) * Scale, .001));
        const FVector Left = WallhackSensorPeopleMath::ToWorld(FVector2D(-.5,2), FTransform::Identity, Scale);
        TestTrue(TEXT("Right/forward axis conversion"), Left.Equals(FVector(2,-.5,0) * Scale, .001));
        WallhackSpatialMath::FContactView View;
        const FVector Fixed = WallhackSensorPeopleMath::ToWorld(FVector2D(0,3), FTransform::Identity, Scale);
        TestTrue(TEXT("Moving wearer projection"), WallhackSpatialMath::ProjectContact(Fixed, FVector(1,0,1.7) * Scale, 0, Scale, View));
        TestTrue(TEXT("1 m walk changes horizontal range to 2 m"), FMath::IsNearlyEqual(View.GroundRangeMeters, 2.f));
        WallhackSpatialMath::ProjectContact(Fixed, FVector(1,0,1.7) * Scale, 90, Scale, View);
        // ProjectContact uses float SinCos; allow sub-millimetre roundoff.
        TestTrue(TEXT("Turning right puts fixed person on left"), FMath::IsNearlyEqual(View.RightMeters, -2.f, .001f));
    }
    FVector Hit;
    TestTrue(TEXT("Downward controller ray intersects floor"), WallhackSensorPeopleMath::FloorAim(FVector(0,0,170), FVector(1,0,-1).GetSafeNormal(), 0, Hit));
    TestTrue(TEXT("Floor hit is below the aimed ground location"), Hit.Equals(FVector(170,0,0), .001));
    TestFalse(TEXT("Horizontal controller ray cannot place reference"), WallhackSensorPeopleMath::FloorAim(FVector(0,0,170), FVector::ForwardVector, 0, Hit));
    return true;
}
#endif

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR && !PLATFORM_ANDROID
#include "WallhackRuntimeTestWorld.h"
#include "WallhackPeopleRenderer.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "AssetCompilingManager.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWallhackSensorPeopleRendererTest,
    "Wallhack.SensorPeople.RendererSourceTransitionAndRemoval", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWallhackSensorPeopleRendererTest::RunTest(const FString&)
{
    FScopedWallhackRuntimeWorld Fixture;
    if (!TestNotNull(TEXT("Test world"), Fixture.World)) return false;
    auto* Renderer = Fixture.World->SpawnActor<AWallhackPeopleRenderer>();
    if (!TestNotNull(TEXT("Existing human renderer"), Renderer)) return false;
    FAssetCompilingManager::Get().FinishAllCompilation();
    FWallhackPersonPose Pose;
    Pose.Id = 1; Pose.Feet = FVector(3,1,0); Pose.Height = 1.65f; Pose.Facing = 180;
    const FLinearColor Green(.25f,.9f,.35f,1), Amber(1,.62f,.08f,1);
    for (float Units : {50.f, 100.f, 200.f})
    {
        Pose.Tint = Green;
        Pose.SourceLabel=TEXT("RADAR");
        Renderer->Present({Pose}, INDEX_NONE, nullptr, true, Units,{1,1,1.7},FQuat::Identity,0,1);
        if (!TestTrue(TEXT("Native body allocated"), Renderer->GetBodyComponents().Num() >= 1)) return false;
        auto* Body = Renderer->GetBodyComponents()[0].Get();
        if (!TestNotNull(TEXT("Human mesh loaded"), Body->GetStaticMesh().Get())) return false;
        TestFalse(TEXT("Fresh silhouette visible"), Body->bHiddenInGame || Renderer->IsHidden());
        TestTrue(TEXT("Metric feet mapped with WorldToMeters"), Body->GetComponentLocation().Equals(Pose.Feet * Units, .001));
        TestTrue(TEXT("Generic standing height is 1.65 m"), FMath::IsNearlyEqual(
            Body->CalcBounds(Body->GetComponentTransform()).GetBox().GetSize().Z, 1.65 * Units, .05));
        auto* Material = Body->GetMaterial(0);
        if (!TestNotNull(TEXT("Human material"), Material)) return false;
        FLinearColor Tint;
        Material->GetVectorParameterValue(FMaterialParameterInfo(TEXT("Tint")), Tint);
        TestTrue(TEXT("Radar is green"), Tint.Equals(Green));
        TestTrue(TEXT("Sensor uses the shared corner label with horizontal wearer range"),Renderer->GetTelemetryText()[0].Contains(TEXT("C1 / RADAR\n2.0 M")));
        TestTrue(TEXT("Body dimensions are explicitly assumptions"),Renderer->GetTelemetryText()[0].Contains(TEXT("ASSUMED H")));
        Pose.Tint = Amber;
        Pose.SourceLabel=TEXT("ESTIMATED");
        Renderer->Present({Pose}, INDEX_NONE, nullptr, true, Units,{1,1,1.7},FQuat::Identity,0,1.3);
        Material->GetVectorParameterValue(FMaterialParameterInfo(TEXT("Tint")), Tint);
        TestTrue(TEXT("Same body's estimate changes to amber immediately"), Tint.Equals(Amber));
        TestTrue(TEXT("Fallback updates the label without claiming a manual contact"),Renderer->GetTelemetryText()[0].Contains(TEXT("ESTIMATED"))&&!Renderer->GetTelemetryText()[0].Contains(TEXT("MANUAL")));
        TestTrue(TEXT("Source transition reuses the same body"), Renderer->GetBodyComponents()[0].Get() == Body);
        FWallhackPersonPose RadarPose = Pose;
        RadarPose.bRadarOnly = true; RadarPose.SourceLabel = TEXT("RADAR ONLY");
        RadarPose.Tint = FLinearColor(.35f,.7f,1,1);
        RadarPose.Feet.Y += 1;
        Renderer->Present({Pose,RadarPose}, INDEX_NONE, nullptr, true, Units,{1,1,1.7},FQuat::Identity,0,1.31);
        TestTrue(TEXT("Camera C1 label stays distinct from radar R1"),Renderer->GetTelemetryText()[0].Contains(TEXT("C1 / ESTIMATED")));
        TestTrue(TEXT("Same numeric radar ID gets an independent body and source label"),Renderer->GetTelemetryText()[1].Contains(TEXT("R1 / RADAR ONLY")));
        TestFalse(TEXT("Independent radar body is visible"),Renderer->GetBodyComponents()[1]->bHiddenInGame);
        Renderer->Present({RadarPose}, INDEX_NONE, nullptr, true, Units,{1,1,1.7},FQuat::Identity,0,1.32);
        TestTrue(TEXT("A pooled camera slot immediately becomes radar-only"),Renderer->GetTelemetryText()[0].Contains(TEXT("R1 / RADAR ONLY")));
        TestTrue(TEXT("Old second body is hidden after the source transition"),Renderer->GetBodyComponents()[1]->bHiddenInGame);
        Renderer->Present({}, INDEX_NONE, nullptr, true, Units);
        TestTrue(TEXT("Fresh empty frame hides previous body"), Body->bHiddenInGame);
        Renderer->Present({Pose}, INDEX_NONE, nullptr, false, Units);
        TestTrue(TEXT("Tracking loss hides world rendering"), Renderer->IsHidden());
    }
    return true;
}
#endif
