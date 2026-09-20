#include "WallhackRuntimeTestWorld.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR && !PLATFORM_ANDROID

#include "Components/StaticMeshComponent.h"
#include "EnhancedInputComponent.h"
#include "GameFramework/InputSettings.h"
#include "GameFramework/PlayerInput.h"
#include "InputAction.h"
#include "InputKeyEventArgs.h"
#include "InputMappingContext.h"
#include "Misc/AutomationTest.h"
#include "WallhackSpatialMath.h"

namespace
{
    UStaticMeshComponent* ContactMesh(const FScopedWallhackRuntimeWorld& Fixture)
    {
        return Fixture.Contact ? FindObject<UStaticMeshComponent>(Fixture.Contact, TEXT("ContactDot")) : nullptr;
    }

    /** Exercise the same Started delegate registered for the actual input key. */
    bool PressHUDKey(AWallhackVRHUDActor* HUD, const FKey& Key)
    {
        if (!HUD) return false;
        const UEnhancedInputComponent* Input = HUD->FindComponentByClass<UEnhancedInputComponent>();
        if (!Input) return false;
        for (const auto* Mapping : {HUD->GetCommonInputContext(), HUD->GetModeInputContext()})
        {
            if (!Mapping) continue;
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
        }
        return false;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWallhackRuntimePlacementTest,
    "Wallhack.Spatial.Runtime.PlacementAndViewerMotion", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWallhackRuntimePlacementTest::RunTest(const FString& Parameters)
{
    FScopedWallhackRuntimeWorld Fixture;
    if (!TestTrue(TEXT("Isolated world has real pawn and player camera manager"), Fixture.HasViewer())
        || !TestNotNull(TEXT("Actual contact actor spawned"), Fixture.Contact)
        || !TestNotNull(TEXT("Actual contact mesh exists"), ContactMesh(Fixture))) return false;

    FVector WorldPosition;
    TestFalse(TEXT("Unplaced actor does not publish a position"), Fixture.Contact->GetContactWorldPosition(WorldPosition));
    TestTrue(TEXT("Unplaced mesh is hidden"), ContactMesh(Fixture)->bHiddenInGame);
    Fixture.AdvanceContact(0.5f);
    TestFalse(TEXT("Automatic placement waits for stable viewer input"), Fixture.Contact->GetContactWorldPosition(WorldPosition));
    Fixture.AdvanceContact(0.7f);
    if (!TestTrue(TEXT("Automatic placement publishes actual actor position"), Fixture.Contact->GetContactWorldPosition(WorldPosition))) return false;
    TestTrue(TEXT("Default contact is 3 m ahead at 1.7 m eye height"), WorldPosition.Equals(FVector(300., 0., 170.), 0.01));
    TestFalse(TEXT("Placed mesh is unhidden"), ContactMesh(Fixture)->bHiddenInGame);
    TestTrue(TEXT("Desktop source is labelled explicitly"), Fixture.Contact->GetTrackingStatus().Contains(TEXT("DESKTOP PREVIEW")));
    TestNull(TEXT("Contact has no actor attachment to the viewer"), Fixture.Contact->GetAttachParentActor());
    const FVector FixedPosition = WorldPosition;

    Fixture.SetViewerPose(FVector(100., 100., 150.), FRotator(15., 90., 0.));
    Fixture.AdvanceContact(2.f);
    TestTrue(TEXT("Moved viewer still sees the contact"), Fixture.Contact->GetContactWorldPosition(WorldPosition));
    TestTrue(TEXT("Automatic placement happens once: point survives movement and turning"), WorldPosition.Equals(FixedPosition, 0.001));

    FVector Viewer;
    FQuat Orientation;
    TestTrue(TEXT("Viewer position comes through actual player camera manager"), Fixture.Contact->GetViewerWorldPose(Viewer, Orientation));
    TestTrue(TEXT("Camera follows the moved pawn"), Viewer.Equals(FVector(100., 100., 150.), 0.01));
    WallhackSpatialMath::FContactView View;
    TestTrue(TEXT("Published world pose projects through the production map conversion"),
        WallhackSpatialMath::ProjectContact(WorldPosition, Viewer, Orientation.Rotator().Yaw, 100.f, View));
    TestTrue(TEXT("Contact is 1 m behind after the 90 degree turn"), FMath::IsNearlyEqual(View.ForwardMeters, -1.f, 0.001f));
    TestTrue(TEXT("Contact is 2 m left after the 90 degree turn"), FMath::IsNearlyEqual(View.RightMeters, -2.f, 0.001f));
    TestTrue(TEXT("Crouching puts contact 0.2 m above eyes"), FMath::IsNearlyEqual(View.HeightMeters, 0.2f, 0.001f));
    TestTrue(TEXT("Actual relative point gives the correct slant range"), FMath::IsNearlyEqual(View.RangeMeters, FMath::Sqrt(5.04f), 0.001f));
    TestTrue(TEXT("Actual map marker appears left and below centre"),
        WallhackSpatialMath::MapOffset(View, 149.f, 5.f).Equals(FVector2D(-59.6, 29.8), 0.001));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWallhackRuntimeViewerValidityTest,
    "Wallhack.Spatial.Runtime.ViewerLossAndRecovery", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWallhackRuntimeViewerValidityTest::RunTest(const FString& Parameters)
{
    FScopedWallhackRuntimeWorld Fixture;
    if (!TestTrue(TEXT("Fixture has viewer"), Fixture.HasViewer()) || !TestNotNull(TEXT("Fixture contact"), Fixture.Contact)) return false;
    Fixture.AdvanceContact(1.2f);
    FVector InitialPosition;
    if (!TestTrue(TEXT("Initial contact is available"), Fixture.Contact->GetContactWorldPosition(InitialPosition))) return false;

    APlayerCameraManager* SavedCameraManager = Fixture.Controller->PlayerCameraManager;
    Fixture.Controller->PlayerCameraManager = nullptr;
    Fixture.AdvanceContact(0.2f);
    FVector Position;
    TestFalse(TEXT("Missing viewer provider suppresses published position"), Fixture.Contact->GetContactWorldPosition(Position));
    TestTrue(TEXT("Missing viewer provider hides actual mesh"), ContactMesh(Fixture)->bHiddenInGame);
    TestTrue(TEXT("Unavailable viewer is reported as tracking lost"), Fixture.Contact->GetTrackingStatus().Contains(TEXT("TRACKING LOST")));
    Fixture.Contact->PlaceInFrontOfViewer();
    TestTrue(TEXT("Placement without viewer does not move stored actor"), Fixture.Contact->GetActorLocation().Equals(InitialPosition, 0.001));

    Fixture.Controller->PlayerCameraManager = SavedCameraManager;
    Fixture.SetViewerPose(FVector(100., 0., 170.), FRotator::ZeroRotator);
    Fixture.AdvanceContact(1.2f);
    TestTrue(TEXT("Restored camera makes same contact available"), Fixture.Contact->GetContactWorldPosition(Position));
    TestTrue(TEXT("Recovery preserves anchor instead of replacing it ahead"), Position.Equals(InitialPosition, 0.001));
    TestFalse(TEXT("Recovery unhides actual mesh"), ContactMesh(Fixture)->bHiddenInGame);
    TestTrue(TEXT("Recovered range reflects moved viewer"), FMath::IsNearlyEqual(FVector::Distance(Position, SavedCameraManager->GetCameraLocation()) / 100., 2., 0.001));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWallhackRuntimeFailedInitializationTest,
    "Wallhack.Spatial.Runtime.InvalidScaleStaysHidden", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWallhackRuntimeFailedInitializationTest::RunTest(const FString& Parameters)
{
    FScopedWallhackRuntimeWorld Fixture(0.f);
    if (!TestNotNull(TEXT("Invalid-scale contact still constructs"), Fixture.Contact)
        || !TestNotNull(TEXT("Invalid-scale contact has visual component"), ContactMesh(Fixture))) return false;
    Fixture.AdvanceContact(2.f);
    FVector Position;
    TestTrue(TEXT("Invalid initialization enters Failed state"), Fixture.Contact->GetTrackingState() == EWallhackContactTrackingState::Failed);
    TestFalse(TEXT("Invalid initialization never exposes a point"), Fixture.Contact->GetContactWorldPosition(Position));
    TestTrue(TEXT("Invalid initialization keeps mesh hidden"), ContactMesh(Fixture)->bHiddenInGame);
    Fixture.Contact->PlaceInFrontOfViewer();
    TestFalse(TEXT("Manual placement cannot bypass invalid scale"), Fixture.Contact->GetContactWorldPosition(Position));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWallhackRuntimeHUDControlsTest,
    "Wallhack.Spatial.Runtime.HUDPlacementAndVisibilityControls", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWallhackRuntimeHUDControlsTest::RunTest(const FString& Parameters)
{
    FScopedWallhackRuntimeWorld Fixture;
    if (!TestTrue(TEXT("Fixture has viewer"), Fixture.HasViewer()) || !TestNotNull(TEXT("Fixture contact"), Fixture.Contact)) return false;
    Fixture.AdvanceContact(1.2f);
    if (!TestNotNull(TEXT("Actual compositor HUD begins play"), Fixture.SpawnHUD())) return false;

    Fixture.SetViewerPose(FVector(50., -25., 145.), FRotator(45., 90., 0.));
    TestTrue(TEXT("Actual A mapping invokes bound placement action"), PressHUDKey(Fixture.HUD, EKeys::OculusTouch_Right_A_Click));
    Fixture.AdvanceContact(0.1f);
    FVector Position;
    TestTrue(TEXT("A replacement publishes contact"), Fixture.Contact->GetContactWorldPosition(Position));
    TestTrue(TEXT("A places 3 horizontal metres ahead at new eye height"), Position.Equals(FVector(50., 275., 145.), 0.01));
    TestEqual(TEXT("Replacement keeps stable person ID"), Fixture.Contact->ContactId, 1);
    const FVector ReplacedPosition = Position;
    Fixture.SetViewerPose(FVector(75., -25., 145.), FRotator::ZeroRotator);
    Fixture.AdvanceContact(1.2f);
    TestTrue(TEXT("Placement action does not keep dragging contact after press"), Fixture.Contact->GetActorLocation().Equals(ReplacedPosition, 0.001));

    TestTrue(TEXT("HUD starts in Minimal density"), Fixture.HUD->GetHUDDensity() == EWallhackHUDDensity::Minimal);
    TestFalse(TEXT("Minimal mode retains world marker"), Fixture.Contact->IsHidden());
    TestTrue(TEXT("B binding switches Minimal to Hidden"), PressHUDKey(Fixture.HUD, EKeys::OculusTouch_Right_B_Click));
    Fixture.AdvanceContact(0.2f);
    TestTrue(TEXT("Hidden actor remains hidden through contact ticks"), Fixture.Contact->IsHidden());
    TestTrue(TEXT("Hiding does not destroy or move the anchor"), Fixture.Contact->GetActorLocation().Equals(ReplacedPosition, 0.001));
    TestTrue(TEXT("B binding restores Full"), PressHUDKey(Fixture.HUD, EKeys::OculusTouch_Right_B_Click));
    TestFalse(TEXT("Full mode restores world actor visibility"), Fixture.Contact->IsHidden());
    TestFalse(TEXT("Full mode has visible contact mesh"), ContactMesh(Fixture)->bHiddenInGame);

    TestTrue(TEXT("Desktop Space mapping invokes same placement action"), PressHUDKey(Fixture.HUD, EKeys::SpaceBar));
    TestTrue(TEXT("Space replacement uses current viewer pose"), Fixture.Contact->GetActorLocation().Equals(FVector(375., -25., 145.), 0.01));

    const FVector BeforeCalibration = Fixture.Contact->GetActorLocation();
    Fixture.SetViewerPose(FVector(75., -25., 145.), FRotator(0., 117., 0.));
    TestTrue(TEXT("Desktop X mapping invokes compass calibration"), PressHUDKey(Fixture.HUD, EKeys::X));
    Fixture.AdvanceContact(0.2f);
    TestTrue(TEXT("Compass calibration never relocates actual world contact"), Fixture.Contact->GetActorLocation().Equals(BeforeCalibration, 0.001));
    FVector Viewer;
    FQuat Orientation;
    TestTrue(TEXT("Viewer remains available after calibration"), Fixture.Contact->GetViewerWorldPose(Viewer, Orientation));
    TestTrue(TEXT("Calibration leaves physical camera yaw unchanged"), FMath::IsNearlyEqual(Orientation.Rotator().Yaw, 117., 0.001));
    WallhackSpatialMath::FContactView View;
    TestTrue(TEXT("Same contact geometry projects after actual calibration action"),
        WallhackSpatialMath::ProjectContact(BeforeCalibration, Viewer, Orientation.Rotator().Yaw, 100.f, View));
    TestTrue(TEXT("Actual contact retains its -117 degree relative bearing"), FMath::IsNearlyEqual(View.BearingDegrees, -117.f, 0.001f));

    TestTrue(TEXT("Keyboard B switches Full to Minimal"), PressHUDKey(Fixture.HUD, EKeys::B));
    TestFalse(TEXT("Keyboard Minimal retains actor"), Fixture.Contact->IsHidden());
    TestTrue(TEXT("Keyboard B switches Minimal to Hidden"), PressHUDKey(Fixture.HUD, EKeys::B));
    TestTrue(TEXT("Keyboard Hidden hides actor"), Fixture.Contact->IsHidden());
    TestTrue(TEXT("Keyboard B restores Full"), PressHUDKey(Fixture.HUD, EKeys::B));
    TestFalse(TEXT("Keyboard Full restores actor"), Fixture.Contact->IsHidden());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWallhackRuntimeDesktopMovementTest,
    "Wallhack.Spatial.Runtime.DesktopKeyboardMovement", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWallhackRuntimeDesktopMovementTest::RunTest(const FString& Parameters)
{
    FScopedWallhackRuntimeWorld Fixture;
    if (!TestTrue(TEXT("Fixture has real controlled pawn"), Fixture.HasViewer())
        || !TestNotNull(TEXT("Fixture has contact"), Fixture.Contact)) return false;
    Fixture.AdvanceContact(1.2f);
    const FVector InitialContact = Fixture.Contact->GetActorLocation();

    // Supply real engine key events to the project's configured PlayerInput.
    // The production pawn reads these through APlayerController::IsInputKeyDown.
    Fixture.Controller->PlayerInput = NewObject<UPlayerInput>(Fixture.Controller, UInputSettings::GetDefaultPlayerInputClass());
    UPlayerInput* Input = Fixture.Controller->PlayerInput;
    if (!TestNotNull(TEXT("Configured player input created"), Input)) return false;
    const TArray<UInputComponent*> EmptyInputStack;
    const auto SetKey = [Input, &EmptyInputStack](const FKey& Key, bool bPressed)
    {
        Input->InputKey(FInputKeyEventArgs(nullptr, INPUTDEVICEID_NONE, Key,
            bPressed ? IE_Pressed : IE_Released, bPressed ? 1.f : 0.f, false, 0u));
        Input->ProcessInputStack(EmptyInputStack, 0.1f, false);
    };
    const auto AdvancePawn = [&Fixture](int32 Steps)
    {
        for (int32 Step = 0; Step < Steps; ++Step)
        {
            Fixture.Pawn->Tick(0.1f);
            Fixture.AdvanceContact(0.1f);
        }
    };
    const auto HoldKey = [&SetKey, &AdvancePawn](const FKey& Key, int32 Steps)
    {
        SetKey(Key, true);
        AdvancePawn(Steps);
        SetKey(Key, false);
    };

    HoldKey(EKeys::W, 10);
    TestTrue(TEXT("W moves actual pawn one metre forward per second"), Fixture.Pawn->GetActorLocation().Equals(FVector(100., 0., 170.), 0.01));
    AdvancePawn(2);
    TestTrue(TEXT("Releasing W stops movement"), Fixture.Pawn->GetActorLocation().Equals(FVector(100., 0., 170.), 0.01));
    HoldKey(EKeys::D, 10);
    TestTrue(TEXT("D moves actual pawn one metre right"), Fixture.Pawn->GetActorLocation().Equals(FVector(100., 100., 170.), 0.01));
    HoldKey(EKeys::A, 10);
    HoldKey(EKeys::S, 10);
    TestTrue(TEXT("A and S reverse the lateral and forward steps"), Fixture.Pawn->GetActorLocation().Equals(FVector(0., 0., 170.), 0.01));

    HoldKey(EKeys::Right, 15);
    TestTrue(TEXT("Right arrow turns 90 degrees in 1.5 seconds"), FMath::IsNearlyEqual(Fixture.Pawn->GetActorRotation().Yaw, 90., 0.01));
    HoldKey(EKeys::Up, 10);
    TestTrue(TEXT("Up arrow changes actual camera pitch"), FMath::IsNearlyEqual(Fixture.Pawn->GetActorRotation().Pitch, 60., 0.01));
    HoldKey(EKeys::W, 10);
    TestTrue(TEXT("Forward movement stays horizontal while looking up"), Fixture.Pawn->GetActorLocation().Equals(FVector(0., 100., 170.), 0.01));
    HoldKey(EKeys::E, 10);
    TestTrue(TEXT("E raises viewer one metre"), Fixture.Pawn->GetActorLocation().Equals(FVector(0., 100., 270.), 0.01));
    HoldKey(EKeys::Q, 10);
    TestTrue(TEXT("Q lowers viewer one metre"), Fixture.Pawn->GetActorLocation().Equals(FVector(0., 100., 170.), 0.01));
    HoldKey(EKeys::Down, 10);
    HoldKey(EKeys::Left, 15);
    TestTrue(TEXT("Opposite arrow inputs restore original view direction"), Fixture.Pawn->GetActorRotation().Equals(FRotator::ZeroRotator, 0.01));

    const FVector BeforeDiagonal = Fixture.Pawn->GetActorLocation();
    SetKey(EKeys::W, true);
    SetKey(EKeys::D, true);
    AdvancePawn(10);
    SetKey(EKeys::W, false);
    SetKey(EKeys::D, false);
    TestTrue(TEXT("Diagonal movement keeps the same one metre per second speed"),
        FMath::IsNearlyEqual(FVector::Distance(BeforeDiagonal, Fixture.Pawn->GetActorLocation()), 100., 0.01));
    TestTrue(TEXT("All actual keyboard motion preserves stationary world contact"), Fixture.Contact->GetActorLocation().Equals(InitialContact, 0.001));
    return true;
}

#endif
