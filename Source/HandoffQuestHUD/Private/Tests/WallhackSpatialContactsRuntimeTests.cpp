#include "WallhackRuntimeTestWorld.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR && !PLATFORM_ANDROID

#include "Components/StaticMeshComponent.h"
#include "EnhancedInputComponent.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "Misc/AutomationTest.h"
#include "WallhackSpatialMath.h"

namespace
{
    const FWallhackSpatialContact* FindContact(const TArray<FWallhackSpatialContact>& Contacts, int32 Id)
    {
        return Contacts.FindByPredicate([Id](const FWallhackSpatialContact& Contact) { return Contact.Id == Id; });
    }

    bool CheckFreshThree(FAutomationTestBase& Test, const TCHAR* Stage, const TArray<FWallhackSpatialContact>& Contacts)
    {
        bool bPassed = Test.TestEqual(FString(Stage) + TEXT(" retains three identities"), Contacts.Num(), 3);
        for (int32 Id = 1; Id <= 3; ++Id)
        {
            const FWallhackSpatialContact* Contact = FindContact(Contacts, Id);
            const FString Label = FString::Printf(TEXT("%s contact %02d"), Stage, Id);
            bPassed &= Test.TestNotNull(Label + TEXT(" exists"), Contact);
            if (!Contact) continue;
            bPassed &= Test.TestTrue(Label + TEXT(" has a usable current pose"), Contact->bPositionValid && !Contact->bStale);
            bPassed &= Test.TestTrue(Label + TEXT(" has a finite position"), !Contact->WorldPosition.ContainsNaN());
            bPassed &= Test.TestTrue(Label + TEXT(" has a fresh nonnegative age"),
                FMath::IsFinite(Contact->AgeSeconds) && Contact->AgeSeconds >= 0.f && Contact->AgeSeconds <= 0.75f);
        }
        return bPassed;
    }

    void CheckUnusable(FAutomationTestBase& Test, const TCHAR* Stage, const TArray<FWallhackSpatialContact>& Contacts)
    {
        Test.TestEqual(FString(Stage) + TEXT(" preserves contact identities"), Contacts.Num(), 3);
        for (const FWallhackSpatialContact& Contact : Contacts)
        {
            const FString Label = FString::Printf(TEXT("%s contact %02d"), Stage, Contact.Id);
            Test.TestFalse(Label + TEXT(" publishes no valid position"), Contact.bPositionValid);
            Test.TestTrue(Label + TEXT(" clears unusable coordinates"), Contact.WorldPosition.IsNearlyZero());
        }
    }

    bool AllContactMeshesHidden(AWallhackWorldContact* Actor)
    {
        TInlineComponentArray<UStaticMeshComponent*> Meshes(Actor);
        if (Meshes.Num() < 3) return false;
        for (const UStaticMeshComponent* Mesh : Meshes)
        {
            if (!Mesh->bHiddenInGame) return false;
        }
        return true;
    }

    bool InvokeHUDKey(AWallhackVRHUDActor* HUD, const FKey& Key)
    {
        if (!HUD) return false;
        const UInputMappingContext* Mapping = FindObject<UInputMappingContext>(HUD, TEXT("WallhackHUDMappingContext"));
        const UEnhancedInputComponent* Input = HUD->FindComponentByClass<UEnhancedInputComponent>();
        if (!Mapping || !Input) return false;
        for (const FEnhancedActionKeyMapping& KeyMapping : Mapping->GetMappings())
        {
            if (KeyMapping.Key != Key) continue;
            for (const TUniquePtr<FEnhancedInputActionEventBinding>& Binding : Input->GetActionEventBindings())
            {
                if (Binding->GetAction() == KeyMapping.Action && Binding->GetTriggerEvent() == ETriggerEvent::Started)
                {
                    Binding->Execute(FInputActionInstance(Binding->GetAction()));
                    return true;
                }
            }
        }
        return false;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWallhackRuntimeMultipleMotionTest,
    "Wallhack.Spatial.Runtime.MultipleContactsWorldMotion", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWallhackRuntimeMultipleMotionTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("Product actor defaults to multiple contacts"), GetDefault<AWallhackWorldContact>()->AreMultipleContactsEnabled());
    FScopedWallhackRuntimeWorld Fixture;
    if (!TestTrue(TEXT("Fixture viewer initialized"), Fixture.HasViewer()) || !TestNotNull(TEXT("Contact actor"), Fixture.Contact)) return false;
    Fixture.Contact->SetMultipleContactsEnabled(true);
    Fixture.AdvanceContact(1.2f);
    TArray<FWallhackSpatialContact> Initial;
    Fixture.Contact->GetSpatialContacts(Initial);
    if (!CheckFreshThree(*this, TEXT("Initially"), Initial)) return false;

    Fixture.AdvanceContact(1.f);
    TArray<FWallhackSpatialContact> Moving;
    Fixture.Contact->GetSpatialContacts(Moving);
    if (!CheckFreshThree(*this, TEXT("After motion"), Moving)) return false;
    TestTrue(TEXT("Person 01 remains the fixed anchor reference"), FindContact(Moving, 1)->WorldPosition.Equals(FindContact(Initial, 1)->WorldPosition, 0.001));
    const FVector Motion2 = FindContact(Moving, 2)->WorldPosition - FindContact(Initial, 2)->WorldPosition;
    const FVector Motion3 = FindContact(Moving, 3)->WorldPosition - FindContact(Initial, 3)->WorldPosition;
    TestTrue(TEXT("Person 02 moves independently of the viewer"), Motion2.Size() > 1.);
    TestTrue(TEXT("Person 03 moves independently of the viewer"), Motion3.Size() > 1.);
    TestFalse(TEXT("Moving people do not share one rigid translation"), Motion2.Equals(Motion3, 0.01));

    Fixture.Contact->SetSimulationPaused(true);
    Fixture.SetViewerPose(FVector(100., 100., 150.), FRotator(15., 90., 0.));
    Fixture.AdvanceContact(1.f);
    TArray<FWallhackSpatialContact> AfterViewerMove;
    Fixture.Contact->GetSpatialContacts(AfterViewerMove);
    if (!CheckFreshThree(*this, TEXT("After viewer motion while paused"), AfterViewerMove)) return false;
    for (int32 Id = 1; Id <= 3; ++Id)
    {
        TestTrue(FString::Printf(TEXT("Viewer translation and turn do not move person %02d"), Id),
            FindContact(AfterViewerMove, Id)->WorldPosition.Equals(FindContact(Moving, Id)->WorldPosition, 0.001));
    }

    FVector Viewer;
    FQuat Orientation;
    TestTrue(TEXT("Real camera pose remains available"), Fixture.Contact->GetViewerWorldPose(Viewer, Orientation));
    WallhackSpatialMath::FContactView View;
    TestTrue(TEXT("Fixed person projects from the same published multi-contact pose"),
        WallhackSpatialMath::ProjectContact(FindContact(AfterViewerMove, 1)->WorldPosition, Viewer,
            Orientation.Rotator().Yaw, 100.f, View));
    TestTrue(TEXT("Viewer movement produces correct map coordinate for person 01"),
        WallhackSpatialMath::MapOffset(View, 149.f, 5.f).Equals(FVector2D(-59.6, 29.8), 0.001));

    Fixture.Contact->SetMultipleContactsEnabled(false);
    TArray<FWallhackSpatialContact> Single;
    Fixture.Contact->GetSpatialContacts(Single);
    TestEqual(TEXT("Single-contact toggle exposes one identity"), Single.Num(), 1);
    if (Single.Num() == 1) TestEqual(TEXT("Single-contact identity remains 01"), Single[0].Id, 1);
    Fixture.Contact->SetMultipleContactsEnabled(true);
    Fixture.AdvanceContact(0.1f);
    Fixture.Contact->GetSpatialContacts(AfterViewerMove);
    CheckFreshThree(*this, TEXT("Reenabled multiple contacts"), AfterViewerMove);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWallhackRuntimeContactFreshnessTest,
    "Wallhack.Spatial.Runtime.PausedMotionVersusStaleUpdates", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWallhackRuntimeContactFreshnessTest::RunTest(const FString& Parameters)
{
    FScopedWallhackRuntimeWorld Fixture;
    if (!TestTrue(TEXT("Fixture viewer initialized"), Fixture.HasViewer()) || !TestNotNull(TEXT("Contact actor"), Fixture.Contact)) return false;
    Fixture.Contact->SetMultipleContactsEnabled(true);
    Fixture.AdvanceContact(1.2f);
    Fixture.Contact->SetSimulationPaused(true);
    TArray<FWallhackSpatialContact> Frozen;
    Fixture.Contact->GetSpatialContacts(Frozen);
    if (!CheckFreshThree(*this, TEXT("Before freeze"), Frozen)) return false;

    Fixture.AdvanceContact(2.f);
    TArray<FWallhackSpatialContact> Contacts;
    Fixture.Contact->GetSpatialContacts(Contacts);
    if (!CheckFreshThree(*this, TEXT("Paused motion still samples fresh data"), Contacts)) return false;
    for (int32 Id = 1; Id <= 3; ++Id)
    {
        TestTrue(FString::Printf(TEXT("Paused person %02d stays at the same point"), Id),
            FindContact(Contacts, Id)->WorldPosition.Equals(FindContact(Frozen, Id)->WorldPosition, 0.001));
    }

    Fixture.Contact->SetContactUpdatesEnabled(false);
    TestFalse(TEXT("Update toggle reflects disabled source"), Fixture.Contact->AreContactUpdatesEnabled());
    Fixture.AdvanceContact(0.5f);
    Fixture.Contact->GetSpatialContacts(Contacts);
    if (!CheckFreshThree(*this, TEXT("Short update gap"), Contacts)) return false;
    for (const FWallhackSpatialContact& Contact : Contacts)
    {
        TestTrue(TEXT("Disabled updates expose increasing sample age"), Contact.AgeSeconds >= 0.49f);
    }
    Fixture.AdvanceContact(0.3f);
    Fixture.Contact->GetSpatialContacts(Contacts);
    CheckUnusable(*this, TEXT("Expired source"), Contacts);
    for (const FWallhackSpatialContact& Contact : Contacts)
    {
        TestTrue(TEXT("Expired source explicitly marks stale"), Contact.bStale && Contact.AgeSeconds > 0.75f);
    }
    TestTrue(TEXT("Stale source hides every actual contact mesh"), AllContactMeshesHidden(Fixture.Contact));
    FVector LegacyPosition;
    TestFalse(TEXT("Legacy point accessor cannot leak stale coordinates"), Fixture.Contact->GetContactWorldPosition(LegacyPosition));

    Fixture.Contact->SetContactUpdatesEnabled(true);
    Fixture.AdvanceContact(0.1f);
    Fixture.Contact->GetSpatialContacts(Contacts);
    if (!CheckFreshThree(*this, TEXT("Updates restored"), Contacts)) return false;
    for (int32 Id = 1; Id <= 3; ++Id)
    {
        TestTrue(FString::Printf(TEXT("Fresh paused person %02d recovers original point"), Id),
            FindContact(Contacts, Id)->WorldPosition.Equals(FindContact(Frozen, Id)->WorldPosition, 0.001));
    }
    Fixture.Contact->SetSimulationPaused(false);
    TestFalse(TEXT("Motion toggle resumes source"), Fixture.Contact->IsSimulationPaused());
    Fixture.AdvanceContact(1.f);
    Fixture.Contact->GetSpatialContacts(Contacts);
    if (CheckFreshThree(*this, TEXT("Motion resumed"), Contacts))
    {
        TestFalse(TEXT("Person 02 moves again after resuming"), FindContact(Contacts, 2)->WorldPosition.Equals(FindContact(Frozen, 2)->WorldPosition, 0.01));
        TestFalse(TEXT("Person 03 moves again after resuming"), FindContact(Contacts, 3)->WorldPosition.Equals(FindContact(Frozen, 3)->WorldPosition, 0.01));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWallhackRuntimeContactLifecycleTest,
    "Wallhack.Spatial.Runtime.MultipleContactsLifecycle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWallhackRuntimeContactLifecycleTest::RunTest(const FString& Parameters)
{
    FScopedWallhackRuntimeWorld Fixture;
    if (!TestTrue(TEXT("Fixture viewer initialized"), Fixture.HasViewer()) || !TestNotNull(TEXT("Contact actor"), Fixture.Contact)) return false;
    Fixture.Contact->SetMultipleContactsEnabled(true);
    Fixture.AdvanceContact(1.2f);
    Fixture.Contact->SetSimulationPaused(true);
    TArray<FWallhackSpatialContact> Initial;
    Fixture.Contact->GetSpatialContacts(Initial);
    if (!CheckFreshThree(*this, TEXT("Before suspension"), Initial)) return false;
    const FVector AnchorPosition = Fixture.Contact->GetActorLocation();

    Fixture.Contact->SetApplicationSuspended(true);
    TArray<FWallhackSpatialContact> Contacts;
    Fixture.Contact->GetSpatialContacts(Contacts);
    CheckUnusable(*this, TEXT("Suspend invalidates immediately"), Contacts);
    TestTrue(TEXT("Suspend hides all world meshes immediately"), AllContactMeshesHidden(Fixture.Contact));
    Fixture.SetViewerPose(FVector(200., 50., 140.), FRotator(0., 135., 0.));
    Fixture.Contact->PlaceInFrontOfViewer();
    Fixture.AdvanceContact(2.f);
    TestTrue(TEXT("Placement during suspend cannot move anchor"), Fixture.Contact->GetActorLocation().Equals(AnchorPosition, 0.001));

    Fixture.Contact->SetApplicationSuspended(false);
    Fixture.Contact->GetSpatialContacts(Contacts);
    CheckUnusable(*this, TEXT("Resume waits for a fresh sample"), Contacts);
    Fixture.AdvanceContact(0.1f);
    Fixture.Contact->GetSpatialContacts(Contacts);
    if (!CheckFreshThree(*this, TEXT("After valid resume tick"), Contacts)) return false;
    for (int32 Id = 1; Id <= 3; ++Id)
    {
        TestTrue(FString::Printf(TEXT("Resume retains paused person %02d world position"), Id),
            FindContact(Contacts, Id)->WorldPosition.Equals(FindContact(Initial, Id)->WorldPosition, 0.001));
    }

    APlayerCameraManager* SavedCamera = Fixture.Controller->PlayerCameraManager;
    Fixture.Controller->PlayerCameraManager = nullptr;
    Fixture.Contact->GetSpatialContacts(Contacts);
    CheckUnusable(*this, TEXT("Missing viewer invalidates every snapshot"), Contacts);
    Fixture.AdvanceContact(0.1f);
    TestTrue(TEXT("Missing viewer hides all meshes"), AllContactMeshesHidden(Fixture.Contact));
    Fixture.Controller->PlayerCameraManager = SavedCamera;
    Fixture.AdvanceContact(0.1f);
    Fixture.Contact->GetSpatialContacts(Contacts);
    CheckFreshThree(*this, TEXT("Viewer recovered"), Contacts);

    Fixture.Contact->RouteEndPlay(EEndPlayReason::EndPlayInEditor);
    Fixture.Contact->GetSpatialContacts(Contacts);
    for (const FWallhackSpatialContact& Contact : Contacts)
    {
        TestFalse(TEXT("Ended actor exposes no usable positions"), Contact.bPositionValid);
        TestTrue(TEXT("Ended actor exposes no stale world coordinates"), Contact.WorldPosition.IsNearlyZero());
    }
    TestTrue(TEXT("Ended actor hides all meshes"), AllContactMeshesHidden(Fixture.Contact));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWallhackRuntimeMultiHUDInteractionTest,
    "Wallhack.Spatial.Runtime.MultiContactHUDControls", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWallhackRuntimeMultiHUDInteractionTest::RunTest(const FString& Parameters)
{
    FScopedWallhackRuntimeWorld Fixture;
    if (!TestTrue(TEXT("Fixture viewer initialized"), Fixture.HasViewer()) || !TestNotNull(TEXT("Contact actor"), Fixture.Contact)) return false;
    Fixture.Contact->SetMultipleContactsEnabled(true);
    Fixture.AdvanceContact(1.2f);
    if (!TestNotNull(TEXT("Actual multi-contact HUD"), Fixture.SpawnHUD())) return false;
    TestEqual(TEXT("Initial selection is person 01"), Fixture.HUD->GetSelectedContactId(), 1);
    TestTrue(TEXT("Initial map radius is 5 metres"), FMath::IsNearlyEqual(Fixture.HUD->GetMapRangeMeters(), 5.f));

    TestTrue(TEXT("Tab selects next contact through actual mapping"), InvokeHUDKey(Fixture.HUD, EKeys::Tab));
    TestEqual(TEXT("Tab selects person 02"), Fixture.HUD->GetSelectedContactId(), 2);
    TestTrue(TEXT("Right trigger selects next person"), InvokeHUDKey(Fixture.HUD, EKeys::OculusTouch_Right_Trigger_Click));
    TestEqual(TEXT("Trigger selects person 03"), Fixture.HUD->GetSelectedContactId(), 3);
    Fixture.HUD->SelectNextContact();
    TestEqual(TEXT("Selection wraps to person 01"), Fixture.HUD->GetSelectedContactId(), 1);
    Fixture.HUD->SelectNextContact();
    TestEqual(TEXT("Selection can return to person 02"), Fixture.HUD->GetSelectedContactId(), 2);

    TestTrue(TEXT("M cycles map range"), InvokeHUDKey(Fixture.HUD, EKeys::M));
    TestTrue(TEXT("First range step is 10 metres"), FMath::IsNearlyEqual(Fixture.HUD->GetMapRangeMeters(), 10.f));
    TestTrue(TEXT("Right thumbstick click cycles map range"), InvokeHUDKey(Fixture.HUD, EKeys::OculusTouch_Right_Thumbstick_Click));
    TestTrue(TEXT("Second range step is 20 metres"), FMath::IsNearlyEqual(Fixture.HUD->GetMapRangeMeters(), 20.f));
    Fixture.HUD->CycleMapRange();
    TestTrue(TEXT("Range wraps to 5 metres"), FMath::IsNearlyEqual(Fixture.HUD->GetMapRangeMeters(), 5.f));

    TestTrue(TEXT("P pauses simulation motion"), InvokeHUDKey(Fixture.HUD, EKeys::P));
    TestTrue(TEXT("Motion is paused"), Fixture.Contact->IsSimulationPaused());
    TestTrue(TEXT("Left Y resumes simulation motion"), InvokeHUDKey(Fixture.HUD, EKeys::OculusTouch_Left_Y_Click));
    TestFalse(TEXT("Motion resumes"), Fixture.Contact->IsSimulationPaused());
    TestTrue(TEXT("F disables source updates"), InvokeHUDKey(Fixture.HUD, EKeys::F));
    TestFalse(TEXT("Source updates disabled"), Fixture.Contact->AreContactUpdatesEnabled());
    Fixture.AdvanceContact(1.f);
    Fixture.HUD->Tick(1.f);
    TestEqual(TEXT("Stale interval preserves selected person identity"), Fixture.HUD->GetSelectedContactId(), 2);
    TestTrue(TEXT("Left thumbstick restores source updates"), InvokeHUDKey(Fixture.HUD, EKeys::OculusTouch_Left_Thumbstick_Click));
    Fixture.AdvanceContact(0.1f);
    Fixture.HUD->Tick(0.1f);
    TestTrue(TEXT("Source updates restored"), Fixture.Contact->AreContactUpdatesEnabled());
    TestEqual(TEXT("Fresh recovery retains same selection"), Fixture.HUD->GetSelectedContactId(), 2);

    Fixture.Contact->SetMultipleContactsEnabled(false);
    Fixture.HUD->Tick(0.1f);
    TestEqual(TEXT("Removing selected person falls back to available person 01"), Fixture.HUD->GetSelectedContactId(), 1);
    return true;
}

#endif
