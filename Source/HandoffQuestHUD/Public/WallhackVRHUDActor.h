#pragma once

#include "CoreMinimal.h"
#include "Components/StereoLayerComponent.h"
#include "GameFramework/Actor.h"
#include "WallhackTelemetrySubsystem.h"
#include "WallhackVRHUDActor.generated.h"

class USceneComponent;
class UTextureRenderTarget2D;
class UStereoLayerShapeCylinder;
class UInputAction;
class UInputMappingContext;
class UFont;
class UMotionControllerComponent;
class UTexture2D;
class AWallhackWorldContact;
class AWallhackSensorPeopleActor;
class UCanvas;
class UInstancedStaticMeshComponent;
struct FInputActionValue;

/** Exposes the engine's protected layer-shape selection for this HUD only. */
UCLASS()
class HANDOFFQUESTHUD_API UWallhackCurvedStereoLayerComponent : public UStereoLayerComponent
{
    GENERATED_BODY()

public:
    void ConfigureVisorCurve(float Radius, float ArcLength, int32 Height);
};

/** How much of the operator HUD is currently drawn. Cycled live by the operator. */
UENUM()
enum class EWallhackHUDDensity : uint8
{
    Full,
    Minimal,
    Hidden
};

/** A single line in the on-screen comms/event ticker, with its own fade-out clock. */
USTRUCT()
struct FWallhackHUDEvent
{
    GENERATED_BODY()
    FString Text;
    FLinearColor Color = FLinearColor::White;
    float SpawnSeconds = 0.f;
};

/** Headset-wide transparent operator display drawn into a Quest compositor layer. */
UCLASS()
class HANDOFFQUESTHUD_API AWallhackVRHUDActor : public AActor
{
    GENERATED_BODY()

public:
    AWallhackVRHUDActor();
    virtual ~AWallhackVRHUDActor();
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void Tick(float DeltaSeconds) override;
    void SetWorldContact(AWallhackWorldContact* Contact);
    void SetSensorPeople(AWallhackSensorPeopleActor* People);
    void ExportSpatialHUD();
    /** The actual compositor texture, also presented by the opt-in desktop preview. */
    UTextureRenderTarget2D* GetHUDRenderTarget() const { return HUDRenderTarget; }
    void SelectNextContact();
    int32 GetSelectedContactId() const { return SelectedSpatialContactId; }
    void CycleMapRange();
    float GetMapRangeMeters() const { return SpatialMapRangeMeters; }
    EWallhackHUDDensity GetHUDDensity() const { return HUDDensity; }

private:
    void DrawOperatorHUD(float DeltaSeconds = 0.f);
    void DrawSpatialHUD(UCanvas* Canvas, UFont* Font);
    void DrawNavigationHUD(UCanvas* Canvas, UFont* Font);
    void DrawNavigationMap(UCanvas* Canvas, UFont* Font);
    void DrawNavigationDirection(UCanvas* Canvas, UFont* Font);
    void DrawNavigationCompass(UCanvas* Canvas, UFont* Font);
    void DrawSensorPeopleHUD(UCanvas* Canvas);
    void ConfirmSensorPlacement();
    void ResetSensorPlacement();
    UPROPERTY(Transient) TObjectPtr<AWallhackSensorPeopleActor> SensorPeople;
    UPROPERTY(Transient) TObjectPtr<UInputAction> SensorConfirmAction;
    UPROPERTY(Transient) TObjectPtr<UInputAction> SensorResetAction;
    void BeginNavigationAim();
    void EndNavigationAim();
    void ConfirmNavigation();
    void CancelNavigation();
    void TogglePeopleEditing();
    void SelectNextPerson();
    void MoveSelectedPerson();
    void AdjustPersonHeight(const FInputActionValue& Value);
    void AdjustPersonFacing(const FInputActionValue& Value);
    UPROPERTY(Transient) TObjectPtr<UInputAction> PeopleModeAction;
    UPROPERTY(Transient) TObjectPtr<UInputAction> PeopleSelectAction;
    UPROPERTY(Transient) TObjectPtr<UInputAction> PeopleHeightAction;
    UPROPERTY(Transient) TObjectPtr<UInputAction> PeopleFacingAction;
    UPROPERTY(Transient) TObjectPtr<UInputAction> PeopleMoveAction;
    UPROPERTY(Transient) TObjectPtr<UInputAction> NavigationAimAction;
    UPROPERTY(Transient) TObjectPtr<UInputAction> NavigationConfirmAction;
    UPROPERTY(Transient) TObjectPtr<UInputAction> NavigationCancelAction;
    void ToggleSimulation();
    void ToggleContactUpdates();
    void UpdateSpatialLabels();
    void CycleHUDDensity();
    void CalibrateNorth();
    void PlaceWorldContact();
    void StartTransmit();
    void StopTransmit();
    void PushEvent(const FString& Text, const FLinearColor& Color);

    UPROPERTY(VisibleAnywhere)
    TObjectPtr<USceneComponent> Root;

    UPROPERTY(VisibleAnywhere)
    TObjectPtr<UInstancedStaticMeshComponent> ContactLabels;

    UPROPERTY(VisibleAnywhere)
    TObjectPtr<UWallhackCurvedStereoLayerComponent> StereoLayer;

    UPROPERTY(Transient)
    TObjectPtr<UTextureRenderTarget2D> HUDRenderTarget;

    UPROPERTY(Transient)
    TObjectPtr<UTexture2D> SpatialLabelAtlas;

    UPROPERTY(Transient)
    TObjectPtr<AWallhackWorldContact> WorldContact;

    UPROPERTY(Transient)
    TObjectPtr<UInputAction> PlaceContactAction;

    UPROPERTY(Transient)
    TObjectPtr<UInputAction> SelectContactAction;
    UPROPERTY(Transient)
    TObjectPtr<UInputAction> MapRangeAction;
    UPROPERTY(Transient)
    TObjectPtr<UInputAction> PauseSimulationAction;
    UPROPERTY(Transient)
    TObjectPtr<UInputAction> ContactUpdatesAction;

    int32 SelectedSpatialContactId = 1;
    float SpatialMapRangeMeters = 5.f;

    // Right-controller B press cycles Full -> Minimal -> Hidden -> Full. This
    // isn't just a game-style flourish: it's the operator's one-press way to
    // clear the overlay when the real world in front of them needs full,
    // unobstructed attention.
    UPROPERTY(Transient)
    TObjectPtr<UInputAction> CycleHUDAction;

    UPROPERTY(Transient)
    TObjectPtr<UInputMappingContext> HUDMappingContext;

    // Left-controller X sets the current facing direction as compass "north".
    // The first valid viewer yaw supplies the initial reference; neither is a
    // magnetic/true-north measurement. Local spatial geometry is unaffected.
    UPROPERTY(Transient)
    TObjectPtr<UInputAction> CalibrateNorthAction;

    // Wrist-raise comms panel (5 Sept, per Evan): left controller's trigger
    // is push-to-talk while the panel is showing. LeftHandController tracks
    // the physical left controller so DrawOperatorHUD can tell how close it
    // is to the headset -- "raised to your face" is the trigger for showing
    // the panel at all, same gesture every smartwatch UI uses. NOTE: this
    // wires up the UI and the button state only. There is no audio channel
    // to the drone yet -- the ground bridge only carries JSON telemetry over
    // its WebSocket today -- so StartTransmit()/StopTransmit() just flip
    // bIsTransmitting for the HUD to react to. Actually capturing mic audio
    // and getting it to the drone's end is separate, real backend work.
    UPROPERTY(Transient)
    TObjectPtr<UInputAction> TransmitAction;

    UPROPERTY(VisibleAnywhere)
    TObjectPtr<UMotionControllerComponent> LeftHandController;

    bool bIsTransmitting = false;
    float WristPanelAlpha = 0.f;

    EWallhackHUDDensity HUDDensity = EWallhackHUDDensity::Minimal;
    float SpatialHelpUntilSeconds = 8.f;

    float ElapsedSeconds = 0.f;
    float DisplayedHeading = 0.f;
    float SmoothedFPS = 0.f;
    float PerformanceWindowSeconds = 0.f;
    double HUDSubmitSeconds = 0.0;
    int32 PerformanceFrames = 0;

    // Display offset added to viewer yaw. In spatial mode it changes compass
    // labels only: local-map projection and the world contact use the tracked
    // world pose directly and are independent of this calibration.
    float NorthOffsetDegrees = 0.f;
    bool bHasAutoCalibratedNorth = false;

    EWallhackLinkState LastLinkState = EWallhackLinkState::Connecting;
    TSet<int32> KnownContactIds;
    TArray<FWallhackHUDEvent> EventLog;

    // Target lock: the single contact currently highlighted with a locking
    // reticle (nearest hostile, else nearest contact overall -- see
    // DrawOperatorHUD). -1 means no lock. LockAcquiredAtSeconds is the
    // ElapsedSeconds timestamp of the most recent lock change, used to
    // animate the wide-to-tight bracket snap; the far-past default means no
    // animation plays for the (nonexistent) lock at boot.
    int32 LockedContactId = -1;
    float LockAcquiredAtSeconds = -1000.f;

    // Optional font override. The imported Offline font is disabled after a
    // reproduced Android startup assertion (see constructor); runtime engine
    // fonts work with Text()'s destination-alpha blend. Composite tests traced
    // the invisible-label bug to missing alpha, not missing runtime glyphs.
    UPROPERTY()
    TObjectPtr<UFont> HardRefFont;
};
