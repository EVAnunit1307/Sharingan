#include "WallhackWorldContact.h"
#include "WallhackSpatialMath.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "IXRTrackingSystem.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Parse.h"
#include "UObject/ConstructorHelpers.h"

#if PLATFORM_ANDROID
#include "OculusXRAnchorBPFunctionLibrary.h"
#include "OculusXRAnchorComponent.h"
#include "OculusXRAnchors.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogWallhackContact, Log, All);

namespace
{
    constexpr float PlacementDistanceMeters = 3.0f;
    constexpr float DotDiameterMeters = 0.06f;
    constexpr float StableTrackingDelaySeconds = 1.0f;
    constexpr double CreateTimeoutSeconds = 20.0;

    const TCHAR* StateName(EWallhackContactTrackingState State)
    {
        switch (State)
        {
        case EWallhackContactTrackingState::Unplaced: return TEXT("Unplaced");
        case EWallhackContactTrackingState::Creating: return TEXT("Creating");
        case EWallhackContactTrackingState::Tracked: return TEXT("Tracked");
        case EWallhackContactTrackingState::Unlocalized: return TEXT("Unlocalized");
        case EWallhackContactTrackingState::Failed: return TEXT("Failed");
        default: return TEXT("Unknown");
        }
    }

#if PLATFORM_ANDROID
    void DestroyContactAnchor(UOculusXRAnchorComponent* Anchor)
    {
        if (!IsValid(Anchor))
        {
            return;
        }

        Anchor->SetComponentTickEnabled(false);
        // A late callback can add its component after the actor's EndPlay,
        // when component EndPlay is no longer guaranteed to run. Release the
        // runtime space explicitly and clear its handle to prevent a second
        // destroy from the plugin's ordinary component EndPlay path.
        if (Anchor->HasValidHandle())
        {
            EOculusXRAnchorResult::Type Result = EOculusXRAnchorResult::Failure;
            if (OculusXRAnchors::FOculusXRAnchors::DestroyAnchor(Anchor->GetHandle().GetValue(), Result))
            {
                Anchor->SetHandle(FOculusXRUInt64(0));
            }
            else
            {
                UE_LOG(LogWallhackContact, Warning, TEXT("ContactAnchorReleaseFailed handle=%llu result=%d"),
                    Anchor->GetHandle().GetValue(), static_cast<int32>(Result));
                // Leave the handle intact so component EndPlay can retry.
            }
        }
        Anchor->DestroyComponent();
    }
#endif

#if !UE_BUILD_SHIPPING
    FAutoConsoleCommandWithWorld PlaceContactCommand(
        TEXT("wallhack.PlaceContact"),
        TEXT("Re-place synthetic contact 1 three metres ahead using valid live tracking."),
        FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
        {
            if (!World || !World->IsGameWorld()) return;
            for (TActorIterator<AWallhackWorldContact> It(World); It; ++It)
            {
                It->PlaceInFrontOfViewer();
                return;
            }
        }));

    FAutoConsoleCommandWithWorld ContactStatusCommand(
        TEXT("wallhack.ContactStatus"),
        TEXT("Log synthetic contact state, world position and current measured range."),
        FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
        {
            if (!World || !World->IsGameWorld()) return;
            for (TActorIterator<AWallhackWorldContact> It(World); It; ++It)
            {
                FVector Viewer;
                FQuat Orientation;
                const bool bViewer = It->GetViewerWorldPose(Viewer, Orientation);
                const float Scale = World->GetWorldSettings()->WorldToMeters;
                TArray<FWallhackSpatialContact> Contacts;
                It->GetSpatialContacts(Contacts);
                for (const FWallhackSpatialContact& Contact : Contacts)
                {
                    UE_LOG(LogWallhackContact, Display,
                        TEXT("ContactStatus id=%d state=%s status=\"%s\" visible=%d stale=%d age_s=%.3f world=%s viewer=%s range_m=%.3f"),
                        Contact.Id, StateName(It->GetTrackingState()), *It->GetTrackingStatus(), Contact.bPositionValid,
                        Contact.bStale, Contact.AgeSeconds < TNumericLimits<float>::Max() ? Contact.AgeSeconds : -1.0f,
                        Contact.bPositionValid ? *Contact.WorldPosition.ToString() : TEXT("UNAVAILABLE"),
                        bViewer ? *Viewer.ToString() : TEXT("UNAVAILABLE"),
                        Contact.bPositionValid && bViewer && Scale > 0.0f
                            ? FVector::Distance(Contact.WorldPosition, Viewer) / Scale : -1.0f);
                }
                return;
            }
            UE_LOG(LogWallhackContact, Display, TEXT("ContactStatus actor=missing"));
        }));
#endif
}

AWallhackWorldContact::AWallhackWorldContact()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickGroup = TG_PostUpdateWork;

    ContactRoot = CreateDefaultSubobject<USceneComponent>(TEXT("ContactRoot"));
    SetRootComponent(ContactRoot);

    ContactMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ContactDot"));
    MovingContactMesh02 = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ContactDot02"));
    MovingContactMesh03 = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ContactDot03"));

    // Hard references ensure mesh and unlit green material enter the cook.
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(
        TEXT("/Engine/BasicShapes/Sphere.Sphere"));
    static ConstructorHelpers::FObjectFinder<UMaterialInterface> Material(
        TEXT("/Game/Materials/M_WallhackContact.M_WallhackContact"));
    for (UStaticMeshComponent* Mesh : { ContactMesh.Get(), MovingContactMesh02.Get(), MovingContactMesh03.Get() })
    {
        Mesh->SetupAttachment(ContactRoot);
        Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Mesh->SetGenerateOverlapEvents(false);
        Mesh->SetCastShadow(false);
        Mesh->SetSimulatePhysics(false);
        Mesh->SetHiddenInGame(true);
        if (Sphere.Succeeded()) Mesh->SetStaticMesh(Sphere.Object);
        if (Material.Succeeded()) Mesh->SetMaterial(0, Material.Object);
    }
    bVisualAssetsReady = Sphere.Succeeded() && Material.Succeeded();
}

void AWallhackWorldContact::BeginPlay()
{
    Super::BeginPlay();

    BackgroundDelegateHandle = FCoreDelegates::ApplicationWillEnterBackgroundDelegate.AddUObject(
        this, &AWallhackWorldContact::OnApplicationBackground);
    ForegroundDelegateHandle = FCoreDelegates::ApplicationHasEnteredForegroundDelegate.AddUObject(
        this, &AWallhackWorldContact::OnApplicationForeground);

#if !PLATFORM_ANDROID && !UE_BUILD_SHIPPING
    bDesktopPreview = FParse::Param(FCommandLine::Get(), TEXT("WallhackTrackingPreview"));
#endif

    const float Scale = GetWorld()->GetWorldSettings()->WorldToMeters;
    if (!bVisualAssetsReady || !FMath::IsFinite(Scale) || Scale <= 0.0f)
    {
        bAutomaticPlacementAttempted = true;
        SetTrackingState(EWallhackContactTrackingState::Failed, TEXT("CONTACT VISUAL UNAVAILABLE"));
        return;
    }
    // Engine's basic sphere has a 100 Unreal unit diameter. All contacts use
    // the same verified opaque, unlit green material and stationary parent.
    for (UStaticMeshComponent* Mesh : { ContactMesh.Get(), MovingContactMesh02.Get(), MovingContactMesh03.Get() })
    {
        Mesh->SetRelativeScale3D(FVector(DotDiameterMeters * Scale / 100.0f));
    }

    UE_LOG(LogWallhackContact, Display,
        TEXT("ContactInitialized id=1 simulated=1 backend=%s distance_m=3.000 diameter_m=0.060 world_to_meters=%.3f contacts=3 stale_after_s=0.750"),
        bDesktopPreview ? TEXT("DESKTOP_PREVIEW_NO_SPATIAL_ANCHOR") : TEXT("META_SPATIAL_ANCHOR"), Scale);
}

void AWallhackWorldContact::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    bEndingPlay = true;
    FCoreDelegates::ApplicationWillEnterBackgroundDelegate.Remove(BackgroundDelegateHandle);
    FCoreDelegates::ApplicationHasEnteredForegroundDelegate.Remove(ForegroundDelegateHandle);
    ++PlacementGeneration;
    UpdateContactVisuals();
    ReleaseAnchor();
    // The SDK holds a weak actor reference for a pending create. It destroys
    // an orphaned space before invoking our callback if this actor is gone.
    Super::EndPlay(EndPlayReason);
}

bool AWallhackWorldContact::GetViewerWorldPose(FVector& OutPosition, FQuat& OutOrientation) const
{
    if (bEndingPlay || bApplicationSuspended || !GetWorld()) return false;

    if (!bDesktopPreview)
    {
        if (!GEngine || !GEngine->XRSystem.IsValid()) return false;
        const auto XR = GEngine->XRSystem;
        FQuat TrackedOrientation;
        FVector TrackedPosition;
        if (!XR->IsHeadTrackingAllowedForWorld(*GetWorld())
            || !XR->IsTracking(IXRTrackingSystem::HMDDeviceId)
            || !XR->HasValidTrackingPosition()
            || !XR->GetCurrentPose(IXRTrackingSystem::HMDDeviceId, TrackedOrientation, TrackedPosition)
            || TrackedOrientation.ContainsNaN() || TrackedPosition.ContainsNaN())
        {
            return false;
        }
    }

    const APlayerController* Controller = GetWorld()->GetFirstPlayerController();
    const APlayerCameraManager* Camera = Controller ? Controller->PlayerCameraManager : nullptr;
    if (!IsValid(Camera)) return false;
    const FVector Position = Camera->GetCameraLocation();
    const FQuat Orientation = Camera->GetCameraRotation().Quaternion();
    if (Position.ContainsNaN() || Orientation.ContainsNaN()) return false;
    OutPosition = Position;
    OutOrientation = Orientation;
    return true;
}

bool AWallhackWorldContact::IsAnchorPoseLocalized() const
{
    if (bDesktopPreview) return TrackingState == EWallhackContactTrackingState::Tracked;

#if PLATFORM_ANDROID
    UOculusXRAnchorComponent* Anchor = Cast<UOculusXRAnchorComponent>(AnchorComponent.Get());
    if (!IsValid(Anchor) || !Anchor->HasValidHandle()) return false;
    FTransform Pose;
    FOculusXRAnchorLocationFlags Flags;
    // A stationary anchor can remain valid while the runtime infers its pose
    // between observations. Meta's own anchor component consumes IsValid()
    // poses; the tracked bits describe quality, not pose usability. Keep the
    // successful-locate/finite-pose checks and the separate live viewer gate.
    return UOculusXRAnchorBPFunctionLibrary::TryGetAnchorTransformByHandle(
        Anchor->GetHandle(), Pose, Flags, EOculusXRAnchorSpace::World)
        && Flags.IsValid()
        && !Pose.ContainsNaN();
#else
    return false;
#endif
}

bool AWallhackWorldContact::HasUsableTrackingPose() const
{
    FVector Viewer;
    FQuat Orientation;
    return TrackingState == EWallhackContactTrackingState::Tracked
        && GetViewerWorldPose(Viewer, Orientation) && IsAnchorPoseLocalized();
}

bool AWallhackWorldContact::IsContactSampleStale() const
{
    return !bHasSample || bNeedsFreshSample || SampleAgeSeconds > StaleAfterSeconds;
}

bool AWallhackWorldContact::GetContactWorldPosition(FVector& OutPosition) const
{
    OutPosition = FVector::ZeroVector;
    if (IsContactSampleStale() || !HasUsableTrackingPose()) return false;
    // The Meta component is the sole writer after placement; consumers read
    // exactly the same position as the real stereo mesh, with no head offset.
    const FVector Position = ContactMesh->GetComponentLocation();
    if (Position.ContainsNaN()) return false;
    OutPosition = Position;
    return true;
}

void AWallhackWorldContact::GetSpatialContacts(TArray<FWallhackSpatialContact>& Out) const
{
    const int32 Count = bMultipleContactsEnabled ? 3 : 1;
    Out.Reset(Count);
    const bool bStale = IsContactSampleStale();
    const bool bValid = !bStale && HasUsableTrackingPose();
    const UStaticMeshComponent* Meshes[] = { ContactMesh.Get(), MovingContactMesh02.Get(), MovingContactMesh03.Get() };
    for (int32 Index = 0; Index < Count; ++Index)
    {
        FWallhackSpatialContact& Contact = Out.AddDefaulted_GetRef();
        Contact.Id = Index + 1;
        Contact.AgeSeconds = SampleAgeSeconds;
        Contact.bStale = bStale;
        if (bValid)
        {
            const FVector Position = Meshes[Index]->GetComponentLocation();
            Contact.bPositionValid = !Position.ContainsNaN();
            if (Contact.bPositionValid) Contact.WorldPosition = Position;
        }
    }
}

void AWallhackWorldContact::SetMultipleContactsEnabled(bool bEnabled)
{
    if (bMultipleContactsEnabled == bEnabled) return;
    bMultipleContactsEnabled = bEnabled;
    UpdateContactVisuals();
    UE_LOG(LogWallhackContact, Display, TEXT("ContactMode multiple=%d"), bMultipleContactsEnabled);
}

void AWallhackWorldContact::SetSimulationPaused(bool bPaused)
{
    if (bSimulationPaused == bPaused) return;
    bSimulationPaused = bPaused;
    bSkipNextMotionDelta = true;
    UE_LOG(LogWallhackContact, Display, TEXT("ContactSimulation paused=%d"), bSimulationPaused);
}

void AWallhackWorldContact::SetContactUpdatesEnabled(bool bEnabled)
{
    if (bContactUpdatesEnabled == bEnabled) return;
    bContactUpdatesEnabled = bEnabled;
    bSkipNextMotionDelta = true;
    // Re-enabling the producer does not publish a position until its next Tick.
    UE_LOG(LogWallhackContact, Display, TEXT("ContactUpdates enabled=%d"), bContactUpdatesEnabled);
}

void AWallhackWorldContact::SetApplicationSuspended(bool bSuspended)
{
    if (bEndingPlay || bApplicationSuspended == bSuspended) return;
    const double Now = FPlatformTime::Seconds();
    if (bSuspended)
    {
        ApplicationSuspendedAtSeconds = Now;
    }
    else if (bCreatePending)
    {
        // An inactive application cannot service the asynchronous SDK request.
        // Do not consume the active request timeout while in the background.
        RequestStartSeconds += FMath::Max(0.0, Now - ApplicationSuspendedAtSeconds);
    }
    bApplicationSuspended = bSuspended;
    bNeedsFreshSample = true;
    bSkipNextMotionDelta = true;
    StableTrackingSeconds = 0.0f;
    UpdateContactVisuals();
    UE_LOG(LogWallhackContact, Display,
        TEXT("ContactLifecycle suspended=%d fresh_sample_required=1 anchor=%s"),
        bApplicationSuspended, AnchorId.IsEmpty() ? TEXT("NONE") : *AnchorId);
}

void AWallhackWorldContact::OnApplicationBackground()
{
    SetApplicationSuspended(true);
}

void AWallhackWorldContact::OnApplicationForeground()
{
    SetApplicationSuspended(false);
}

void AWallhackWorldContact::PublishContactSample(float MotionDeltaSeconds)
{
    const float Scale = GetWorld()->GetWorldSettings()->WorldToMeters;
    if (!FMath::IsFinite(Scale) || Scale <= 0.0f)
    {
        bNeedsFreshSample = true;
        bSkipNextMotionDelta = true;
        return;
    }
    // Advance only the synthetic source clock. Resume never catches up missed
    // background/tracking/update time, and a long active frame advances <= 0.1 s.
    if (!bSimulationPaused && !bSkipNextMotionDelta)
    {
        SimulationTimeSeconds += FMath::Clamp(MotionDeltaSeconds, 0.0f, 0.1f);
    }
    bSkipNextMotionDelta = false;
    const double Phase02 = SimulationTimeSeconds * 0.65;
    const double Phase03 = SimulationTimeSeconds * 0.45;
    // Distinct bounded paths in metres, relative to the SAME stationary anchor.
    // Contact 01 stays at the root; only these children are ever animated.
    MovingContactMesh02->SetRelativeLocation(Scale * FVector(
        0.50 * FMath::Sin(Phase02), -1.0 + 0.35 * FMath::Cos(Phase02), 0.15 * FMath::Sin(Phase02)));
    MovingContactMesh03->SetRelativeLocation(Scale * FVector(
        0.55 * FMath::Cos(Phase03), 1.0 + 0.25 * FMath::Sin(2.0 * Phase03), 0.20 * FMath::Cos(Phase03)));
    SampleAgeSeconds = 0.0f;
    bHasSample = true;
    bNeedsFreshSample = false;
}

void AWallhackWorldContact::UpdateContactVisuals()
{
    const bool bVisible = !IsContactSampleStale() && HasUsableTrackingPose();
    ContactMesh->SetHiddenInGame(!bVisible);
    MovingContactMesh02->SetHiddenInGame(!bVisible || !bMultipleContactsEnabled);
    MovingContactMesh03->SetHiddenInGame(!bVisible || !bMultipleContactsEnabled);
}

FString AWallhackWorldContact::GetTrackingStatus() const
{
    if (bApplicationSuspended) return TEXT("APP PAUSED");
    if (TrackingState == EWallhackContactTrackingState::Tracked)
    {
        if (!HasUsableTrackingPose()) return TEXT("TRACKING LOST");
        if (IsContactSampleStale()) return TEXT("CONTACT DATA STALE");
    }
    return StatusDetail;
}

void AWallhackWorldContact::SetTrackingState(EWallhackContactTrackingState NewState, const FString& Detail)
{
    const bool bChanged = TrackingState != NewState || StatusDetail != Detail;
    TrackingState = NewState;
    StatusDetail = Detail;
    UpdateContactVisuals();
    if (bChanged)
    {
        UE_LOG(LogWallhackContact, Display, TEXT("ContactState id=1 state=%s detail=\"%s\" anchor=%s"),
            StateName(NewState), *Detail, AnchorId.IsEmpty() ? TEXT("NONE") : *AnchorId);
    }
}

void AWallhackWorldContact::ReleaseAnchor()
{
    bHasSample = false;
    bNeedsFreshSample = true;
    bSkipNextMotionDelta = true;
    SampleAgeSeconds = TNumericLimits<float>::Max();
    UpdateContactVisuals();
    if (UActorComponent* Component = AnchorComponent.Get())
    {
        RemoveTickPrerequisiteComponent(Component);
#if PLATFORM_ANDROID
        DestroyContactAnchor(Cast<UOculusXRAnchorComponent>(Component));
#endif
    }
    AnchorComponent.Reset();
    AnchorId.Reset();
}

void AWallhackWorldContact::PlaceInFrontOfViewer()
{
    if (bEndingPlay || !bVisualAssetsReady) return;
    if (bCreatePending)
    {
        UE_LOG(LogWallhackContact, Display, TEXT("ContactPlacementRejected id=1 reason=create_pending"));
        return;
    }

    FVector Viewer;
    FQuat Orientation;
    if (!GetViewerWorldPose(Viewer, Orientation))
    {
        UE_LOG(LogWallhackContact, Display, TEXT("ContactPlacementRejected id=1 reason=head_tracking_invalid"));
        return;
    }

    const float Scale = GetWorld()->GetWorldSettings()->WorldToMeters;
    if (!FMath::IsFinite(Scale) || Scale <= 0.0f) return;
    const FVector Position = WallhackSpatialMath::PlaceAhead(Viewer, Orientation, PlacementDistanceMeters, Scale);

    bAutomaticPlacementAttempted = true;
    ReleaseAnchor();
    SimulationTimeSeconds = 0.0;
    ++PlacementGeneration;
    SetActorLocationAndRotation(Position, FQuat::Identity, false, nullptr, ETeleportType::TeleportPhysics);
    UE_LOG(LogWallhackContact, Display,
        TEXT("ContactPlacement id=1 generation=%u viewer=%s world=%s requested_range_m=3.000 simulated=1"),
        PlacementGeneration, *Viewer.ToString(), *Position.ToString());

    if (bDesktopPreview)
    {
        SetTrackingState(EWallhackContactTrackingState::Tracked, TEXT("SIM 01 / DESKTOP PREVIEW"));
        // Desktop placement has no asynchronous localization step. Preserve the
        // immediate placement contract, including input-driven local tests.
        if (bContactUpdatesEnabled) PublishContactSample(0.0f);
        UpdateContactVisuals();
        return;
    }

#if PLATFORM_ANDROID
    bCreatePending = true;
    bCreateTimedOut = false;
    RequestStartSeconds = FPlatformTime::Seconds();
    SetTrackingState(EWallhackContactTrackingState::Creating, TEXT("CREATING SPATIAL ANCHOR"));
    const uint32 RequestGeneration = PlacementGeneration;
    const TWeakObjectPtr<AWallhackWorldContact> WeakThis(this);
    const FOculusXRSpatialAnchorCreateDelegate Callback = FOculusXRSpatialAnchorCreateDelegate::CreateLambda(
        [WeakThis, RequestGeneration](EOculusXRAnchorResult::Type Result, UOculusXRAnchorComponent* CreatedAnchor)
        {
            AWallhackWorldContact* Contact = WeakThis.Get();
            if (!Contact || Contact->bEndingPlay || Contact->PlacementGeneration != RequestGeneration)
            {
                DestroyContactAnchor(CreatedAnchor);
                return;
            }

            Contact->bCreatePending = false;
            if (Contact->bCreateTimedOut
                || !UOculusXRAnchorBPFunctionLibrary::IsAnchorResultSuccess(Result)
                || !IsValid(CreatedAnchor) || !CreatedAnchor->HasValidHandle())
            {
                DestroyContactAnchor(CreatedAnchor);
                UE_LOG(LogWallhackContact, Warning,
                    TEXT("ContactCreateFailed id=1 result=%d timeout=%d"), static_cast<int32>(Result), Contact->bCreateTimedOut);
                Contact->SetTrackingState(EWallhackContactTrackingState::Failed, TEXT("ANCHOR FAILED / A RETRY"));
                return;
            }

            Contact->AnchorComponent = CreatedAnchor;
            Contact->AnchorId = CreatedAnchor->GetUUID().ToString();
            Contact->AddTickPrerequisiteComponent(CreatedAnchor);
            // Creation success is not proof of localization. Only Tick can
            // expose the marker after its first complete component update.
            Contact->SetTrackingState(EWallhackContactTrackingState::Unlocalized, TEXT("LOCALIZING SPATIAL ANCHOR"));
        });

    EOculusXRAnchorResult::Type ImmediateResult = EOculusXRAnchorResult::Failure;
    const bool bAccepted = OculusXRAnchors::FOculusXRAnchors::CreateSpatialAnchor(
        GetActorTransform(), this, Callback, ImmediateResult);
    if (!bAccepted)
    {
        // The installed SDK invokes the callback synchronously on rejection.
        bCreatePending = false;
        UE_LOG(LogWallhackContact, Warning, TEXT("ContactCreateRejected id=1 result=%d"), static_cast<int32>(ImmediateResult));
        SetTrackingState(EWallhackContactTrackingState::Failed, TEXT("ANCHOR FAILED / A RETRY"));
    }
#else
    SetTrackingState(EWallhackContactTrackingState::Failed, TEXT("SPATIAL ANCHORS REQUIRE QUEST"));
#endif
}

void AWallhackWorldContact::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (bEndingPlay) return;

    const float SafeDeltaSeconds = FMath::IsFinite(DeltaSeconds) ? FMath::Max(DeltaSeconds, 0.0f) : 0.0f;
    const bool bWasStale = IsContactSampleStale();
    if (bHasSample)
    {
        SampleAgeSeconds = FMath::Min(SampleAgeSeconds + SafeDeltaSeconds, TNumericLimits<float>::Max());
    }

    FVector Viewer;
    FQuat Orientation;
    const bool bViewerTracked = GetViewerWorldPose(Viewer, Orientation);
    if (!bAutomaticPlacementAttempted)
    {
        StableTrackingSeconds = bViewerTracked
            ? StableTrackingSeconds + FMath::Clamp(SafeDeltaSeconds, 0.0f, 0.1f) : 0.0f;
        if (StableTrackingSeconds >= StableTrackingDelaySeconds) PlaceInFrontOfViewer();
    }

    if (!bApplicationSuspended && bCreatePending && !bCreateTimedOut
        && FPlatformTime::Seconds() - RequestStartSeconds > CreateTimeoutSeconds)
    {
        // The API has no cancellation handle. Keep the in-flight slot occupied
        // until the callback arrives; any late success is released above.
        bCreateTimedOut = true;
        SetTrackingState(EWallhackContactTrackingState::Failed, TEXT("ANCHOR REQUEST TIMED OUT"));
    }

    if (AnchorComponent.IsValid())
    {
        const bool bLocalized = bViewerTracked && IsAnchorPoseLocalized();
        SetTrackingState(bLocalized ? EWallhackContactTrackingState::Tracked : EWallhackContactTrackingState::Unlocalized,
            bLocalized ? TEXT("SIM 01 / ANCHORED") : TEXT("TRACKING LOST / RELOCALIZING"));
    }

    if (!HasUsableTrackingPose())
    {
        // Previously published points cannot become valid again merely because
        // tracking recovered; the source must first publish a fresh sample.
        bNeedsFreshSample = true;
        bSkipNextMotionDelta = true;
    }
    else if (bContactUpdatesEnabled)
    {
        PublishContactSample(SafeDeltaSeconds);
    }
    UpdateContactVisuals();
    const bool bNowStale = IsContactSampleStale();
    if (bWasStale != bNowStale)
    {
        UE_LOG(LogWallhackContact, Display,
            TEXT("ContactFreshness stale=%d age_s=%.3f updates_enabled=%d simulation_paused=%d"),
            bNowStale, bHasSample ? SampleAgeSeconds : -1.0f, bContactUpdatesEnabled, bSimulationPaused);
    }
    const double Now = FPlatformTime::Seconds();
    if (Now - LastLogSeconds >= 2.0)
    {
        LastLogSeconds = Now;
        LogContact();
    }
}

void AWallhackWorldContact::LogContact() const
{
    FVector Viewer;
    FQuat Orientation;
    const bool bViewer = GetViewerWorldPose(Viewer, Orientation);
    const float Scale = GetWorld()->GetWorldSettings()->WorldToMeters;
    TArray<FWallhackSpatialContact> Contacts;
    GetSpatialContacts(Contacts);
    for (const FWallhackSpatialContact& Contact : Contacts)
    {
        UE_LOG(LogWallhackContact, Display,
            TEXT("ContactSample id=%d simulated=1 state=%s visible=%d head_tracked=%d world=%s viewer=%s range_m=%.3f anchor=%s stale=%d age_s=%.3f simulation_paused=%d updates_enabled=%d suspended=%d"),
            Contact.Id, StateName(TrackingState), Contact.bPositionValid, bViewer,
            Contact.bPositionValid ? *Contact.WorldPosition.ToString() : TEXT("UNAVAILABLE"),
            bViewer ? *Viewer.ToString() : TEXT("UNAVAILABLE"),
            Contact.bPositionValid && bViewer && Scale > 0.0f
                ? FVector::Distance(Contact.WorldPosition, Viewer) / Scale : -1.0f,
            AnchorId.IsEmpty() ? TEXT("NONE") : *AnchorId, Contact.bStale,
            bHasSample ? Contact.AgeSeconds : -1.0f, bSimulationPaused, bContactUpdatesEnabled, bApplicationSuspended);
    }

#if PLATFORM_ANDROID
    // LogContact is called at most once every two seconds. Report individual
    // validity/quality bits so suspend recovery can distinguish an unusable
    // locate result from a valid pose with temporarily inferred tracking.
    const UOculusXRAnchorComponent* Anchor = Cast<UOculusXRAnchorComponent>(AnchorComponent.Get());
    const bool bHasHandle = IsValid(Anchor) && Anchor->HasValidHandle();
    FTransform AnchorPose = FTransform::Identity;
    FOculusXRAnchorLocationFlags AnchorFlags;
    const bool bLocated = bHasHandle && UOculusXRAnchorBPFunctionLibrary::TryGetAnchorTransformByHandle(
        Anchor->GetHandle(), AnchorPose, AnchorFlags, EOculusXRAnchorSpace::World);
    UE_LOG(LogWallhackContact, Display,
        TEXT("ContactAnchorSample id=1 handle_valid=%d locate_ok=%d pose_finite=%d flags_valid=%d position_valid=%d orientation_valid=%d position_tracked=%d orientation_tracked=%d"),
        bHasHandle, bLocated, bLocated && !AnchorPose.ContainsNaN(), AnchorFlags.IsValid(),
        AnchorFlags.PositionValid(), AnchorFlags.OrientationValid(),
        AnchorFlags.PositionTracked(), AnchorFlags.OrientationTracked());
#endif
}
