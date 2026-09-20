#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "IWebSocket.h"
#include "WallhackSensorPeopleTypes.h"
#include "WallhackTelemetrySubsystem.generated.h"

UENUM(BlueprintType)
enum class EWallhackLinkState : uint8
{
    Connecting,
    Live,
    Stale,
    Disconnected
};

UENUM(BlueprintType)
enum class EWallhackContactState : uint8
{
    Nominal,
    Degraded,
    Critical
};

/**
 * IFF/threat classification. Deliberately separate from EWallhackContactState,
 * which is a data-quality read (how confident the fix is), not a threat read.
 * Conflating the two would mean a merely-noisy contact could paint as
 * "hostile" -- collapsing them was considered and rejected. Defaults to
 * Unknown and only becomes meaningful once the ground bridge actually sends
 * a classification; see ParsePacket().
 */
UENUM(BlueprintType)
enum class EWallhackContactClassification : uint8
{
    Unknown,
    Friendly,
    Neutral,
    Hostile
};

/**
 * Body posture for a contact, e.g. from a through-wall/NLOS sensor's pose
 * estimate. Same philosophy as EWallhackContactClassification just above:
 * defaults to Unknown and only becomes meaningful once a sensor actually
 * sends one. A HUD that draws a confident stick figure for a contact with
 * no real posture data would be lying by omission -- see ParsePacket() and
 * the "NO POSTURE DATA" fallback in AWallhackVRHUDActor::DrawOperatorHUD().
 */
UENUM(BlueprintType)
enum class EWallhackContactPosture : uint8
{
    Unknown,
    Standing,
    Crouched,
    Prone
};

USTRUCT(BlueprintType)
struct FWallhackRigPose
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) float X = 0.f;
    UPROPERTY(BlueprintReadOnly) float Y = 0.f;
    UPROPERTY(BlueprintReadOnly) float HeadingDegrees = 0.f;
};

USTRUCT(BlueprintType)
struct FWallhackContact
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) int32 Id = -1;
    UPROPERTY(BlueprintReadOnly) float X = 0.f;
    UPROPERTY(BlueprintReadOnly) float Y = 0.f;
    UPROPERTY(BlueprintReadOnly) float Confidence = 0.f;
    UPROPERTY(BlueprintReadOnly) float RangeMeters = 0.f;
    UPROPERTY(BlueprintReadOnly) float RelativeBearingDegrees = 0.f;
    UPROPERTY(BlueprintReadOnly) EWallhackContactState State = EWallhackContactState::Critical;
    UPROPERTY(BlueprintReadOnly) EWallhackContactClassification Classification = EWallhackContactClassification::Unknown;

    // --- NLOS / through-wall pose read, all optional (see EWallhackContactPosture) ---
    UPROPERTY(BlueprintReadOnly) EWallhackContactPosture Posture = EWallhackContactPosture::Unknown;
    // True only once a vitals/breathing lock is actually held on this contact.
    // BreathingRateBpm is meaningless while this is false -- HUD code must
    // check bVitalsLocked, never infer a lock from BreathingRateBpm != 0.
    UPROPERTY(BlueprintReadOnly) bool bVitalsLocked = false;
    UPROPERTY(BlueprintReadOnly) float BreathingRateBpm = 0.f;
};

/** The only data model consumed by rendering. If not Live its Contacts array is empty by design. */
USTRUCT(BlueprintType)
struct FWallhackDisplayFrame
{
    GENERATED_BODY()
    /** True only for the explicit Windows -WallhackPreview simulator. Never emitted on Quest. */
    UPROPERTY(BlueprintReadOnly) bool bIsSimulated = false;
    UPROPERTY(BlueprintReadOnly) EWallhackLinkState LinkState = EWallhackLinkState::Connecting;
    UPROPERTY(BlueprintReadOnly) FWallhackRigPose Rig;
    UPROPERTY(BlueprintReadOnly) TArray<FWallhackContact> Contacts;
    UPROPERTY(BlueprintReadOnly) float PacketAgeSeconds = TNumericLimits<float>::Max();
    UPROPERTY(BlueprintReadOnly) FString SourceLabel = TEXT("GROUND BRIDGE");
};

/**
 * Owns socket lifecycle, JSON validation, bearing calculation, confidence states and the hard
 * stale-data gate. HUD code must only use GetDisplayFrame(), never the last received payload.
 */
UCLASS()
class HANDOFFQUESTHUD_API UWallhackTelemetrySubsystem final : public UGameInstanceSubsystem, public FTickableGameObject
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(UWallhackTelemetrySubsystem, STATGROUP_Tickables); }
    virtual bool IsTickable() const override { return !IsTemplate(); }

    UFUNCTION(BlueprintPure, Category="Wallhack")
    FWallhackDisplayFrame GetDisplayFrame() const;

    UFUNCTION(BlueprintCallable, Category="Wallhack")
    void Reconnect();

    /** Runtime override for a future settings widget; it reconnects immediately and is not persisted. */
    UFUNCTION(BlueprintCallable, Category="Wallhack")
    void SetBridgeUrl(const FString& NewUrl);

    UFUNCTION(BlueprintPure, Category="Wallhack")
    FString GetBridgeUrl() const { return ActiveBridgeUrl; }

    FWallhackSensorPeopleFrame GetSensorPeopleFrame() const;
    void ReportControllerRig(bool bAligned,bool bTracked,const FString& Status);

private:
    void OpenSocket();
    void CloseSocket();
    void HandleSocketConnected();
    void HandleSocketError(const FString& Error);
    void HandleSocketClosed(int32 StatusCode, const FString& Reason, bool bWasClean);
    void HandleMessage(const FString& JsonText);
    bool ParsePacket(const FString& JsonText, FWallhackRigPose& OutRig, TArray<FWallhackContact>& OutContacts) const;
    static EWallhackContactState StateForConfidence(float Confidence);
    static float NormalizeDegrees(float Degrees);
    EWallhackLinkState CurrentLinkState(double Now) const;
    FWallhackDisplayFrame BuildDesktopPreviewFrame(double Now) const;

    TSharedPtr<IWebSocket> Socket;
    FWallhackRigPose LastRig;
    TArray<FWallhackContact> LastContacts;
    double LastPacketSeconds = -1.0;
    double NextReconnectSeconds = 0.0;
    bool bConnected = false;
    bool bDesktopPreviewEnabled = false;
    FString ActiveBridgeUrl;
    FWallhackSensorPeopleStream SensorPeople;
    double LastControllerReport=-1;
};
