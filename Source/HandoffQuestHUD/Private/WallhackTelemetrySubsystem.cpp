#include "WallhackTelemetrySubsystem.h"

#include "Dom/JsonObject.h"
#include "Misc/CommandLine.h"
#include "Json.h"
#include "Misc/DateTime.h"
#include "Misc/Parse.h"
#include "WallhackHUDSettings.h"
#include "WebSocketsModule.h"

void UWallhackTelemetrySubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    ActiveBridgeUrl = GetDefault<UWallhackHUDSettings>()->BridgeUrl;
    bDesktopPreviewEnabled = PLATFORM_WINDOWS && FParse::Param(FCommandLine::Get(), TEXT("WallhackPreview"));
    if (bDesktopPreviewEnabled)
    {
        UE_LOG(LogTemp, Warning, TEXT("Wallhack HUD desktop preview is using simulated data. It must not be used operationally."));
    }
    NextReconnectSeconds = FPlatformTime::Seconds();
}

void UWallhackTelemetrySubsystem::Deinitialize()
{
    CloseSocket();
    Super::Deinitialize();
}

void UWallhackTelemetrySubsystem::Tick(float DeltaTime)
{
    if (!FParse::Param(FCommandLine::Get(), TEXT("WallhackBridge"))) return;
    const double Now = FPlatformTime::Seconds();
    if (!bConnected && !Socket.IsValid() && Now >= NextReconnectSeconds)
    {
        OpenSocket();
    }
}

FWallhackDisplayFrame UWallhackTelemetrySubsystem::GetDisplayFrame() const
{
    const double Now = FPlatformTime::Seconds();
    if (bDesktopPreviewEnabled)
    {
        return BuildDesktopPreviewFrame(Now);
    }

    FWallhackDisplayFrame Frame;
    Frame.LinkState = CurrentLinkState(Now);
    Frame.Rig = LastRig;
    Frame.PacketAgeSeconds = LastPacketSeconds < 0.0
        ? TNumericLimits<float>::Max()
        : static_cast<float>(Now - LastPacketSeconds);

    // This is the safety gate. No caller is given stale contacts to accidentally render.
    if (Frame.LinkState == EWallhackLinkState::Live)
    {
        Frame.Contacts = LastContacts;
    }
    return Frame;
}

FWallhackDisplayFrame UWallhackTelemetrySubsystem::BuildDesktopPreviewFrame(double Now) const
{
    FWallhackDisplayFrame Frame;
    Frame.bIsSimulated = true;
    Frame.SourceLabel = TEXT("DESKTOP SIMULATION / NOT OPERATIONAL");
    Frame.LinkState = EWallhackLinkState::Live;
    Frame.PacketAgeSeconds = 0.042f;
    Frame.Rig.X = 8.f + FMath::Cos(static_cast<float>(Now) * .11f) * 2.f;
    Frame.Rig.Y = 5.f + FMath::Sin(static_cast<float>(Now) * .11f) * 2.f;
    Frame.Rig.HeadingDegrees = FMath::Fmod(static_cast<float>(Now) * 12.f, 360.f);

    auto AddContact = [&Frame](int32 Id, float OffsetX, float OffsetY, float Confidence,
        EWallhackContactPosture Posture = EWallhackContactPosture::Unknown, bool bVitalsLocked = false, float BreathingRateBpm = 0.f)
    {
        FWallhackContact Contact;
        Contact.Id = Id;
        Contact.X = Frame.Rig.X + OffsetX;
        Contact.Y = Frame.Rig.Y + OffsetY;
        Contact.Confidence = Confidence;
        Contact.RangeMeters = FVector2D(OffsetX, OffsetY).Size();
        // Same heading correction as the real bridge path below (line ~226) --
        // this was previously bearing-from-world-north with no
        // Frame.Rig.HeadingDegrees subtracted, so on the desktop preview (whose
        // simulated rig spins continuously) every contact dot visibly drifted
        // out of sync with the HUD's forward-locked sensor cone, which always
        // points "up" on the map regardless of heading.
        Contact.RelativeBearingDegrees = NormalizeDegrees(FMath::RadiansToDegrees(FMath::Atan2(OffsetX, OffsetY)) - Frame.Rig.HeadingDegrees);
        Contact.State = StateForConfidence(Confidence);
        Contact.Posture = Posture;
        Contact.bVitalsLocked = bVitalsLocked;
        Contact.BreathingRateBpm = BreathingRateBpm;
        Frame.Contacts.Add(Contact);
    };

    // Posture/vitals only populated here (desktop simulator) so the HUD's
    // NLOS stick-figure rendering has something to show on a monitor with no
    // real through-wall sensor attached -- never sent by the real bridge
    // path, exactly like the Hostile classification cycle below.
    AddContact(12, 2.5f + FMath::Sin(static_cast<float>(Now)) * .4f, 4.2f, .91f,
        EWallhackContactPosture::Standing, true, 15.f + FMath::Sin(static_cast<float>(Now) * .3f) * 2.f);
    AddContact(27, -3.1f, 2.2f + FMath::Cos(static_cast<float>(Now) * .7f) * .5f, .67f,
        EWallhackContactPosture::Crouched, false);
    AddContact(31, 4.7f, -1.6f, .48f,
        EWallhackContactPosture::Prone, true, 22.f + FMath::Sin(static_cast<float>(Now) * .4f) * 3.f);

    // Desktop-preview-only: cycles contact 12 through Hostile every ~12s so the
    // top IFF banner can be seen and timed on a monitor without needing a real
    // classified detection from the bridge. Never happens on Quest -- this
    // whole function only runs behind -WallhackPreview (see Initialize()).
    if (Frame.Contacts.IsValidIndex(0) && FMath::Fmod(Now, 12.0) < 3.0)
    {
        Frame.Contacts[0].Classification = EWallhackContactClassification::Hostile;
    }
    return Frame;
}

void UWallhackTelemetrySubsystem::Reconnect()
{
    CloseSocket();
    NextReconnectSeconds = FPlatformTime::Seconds();
}

void UWallhackTelemetrySubsystem::SetBridgeUrl(const FString& NewUrl)
{
    ActiveBridgeUrl = NewUrl.TrimStartAndEnd();
    Reconnect();
}

void UWallhackTelemetrySubsystem::OpenSocket()
{
    if (ActiveBridgeUrl.IsEmpty())
    {
        UE_LOG(LogTemp, Error, TEXT("Wallhack HUD BridgeUrl is empty."));
        NextReconnectSeconds = FPlatformTime::Seconds() + 2.0;
        return;
    }

    Socket = FWebSocketsModule::Get().CreateWebSocket(ActiveBridgeUrl);
    Socket->OnConnected().AddUObject(this, &UWallhackTelemetrySubsystem::HandleSocketConnected);
    Socket->OnConnectionError().AddUObject(this, &UWallhackTelemetrySubsystem::HandleSocketError);
    Socket->OnClosed().AddUObject(this, &UWallhackTelemetrySubsystem::HandleSocketClosed);
    Socket->OnMessage().AddUObject(this, &UWallhackTelemetrySubsystem::HandleMessage);
    Socket->Connect();
}

void UWallhackTelemetrySubsystem::CloseSocket()
{
    bConnected = false;
    if (Socket.IsValid())
    {
        Socket->Close();
        Socket.Reset();
    }
}

void UWallhackTelemetrySubsystem::HandleSocketConnected()
{
    bConnected = true;
    UE_LOG(LogTemp, Display, TEXT("Wallhack HUD connected to ground bridge."));
}

void UWallhackTelemetrySubsystem::HandleSocketError(const FString& Error)
{
    UE_LOG(LogTemp, Warning, TEXT("Wallhack HUD socket error: %s"), *Error);
    bConnected = false;
    Socket.Reset();
    NextReconnectSeconds = FPlatformTime::Seconds() + 2.0;
}

void UWallhackTelemetrySubsystem::HandleSocketClosed(int32 StatusCode, const FString& Reason, bool bWasClean)
{
    UE_LOG(LogTemp, Warning, TEXT("Wallhack HUD socket closed (%d): %s"), StatusCode, *Reason);
    bConnected = false;
    Socket.Reset();
    NextReconnectSeconds = FPlatformTime::Seconds() + 2.0;
}

void UWallhackTelemetrySubsystem::HandleMessage(const FString& JsonText)
{
    FWallhackRigPose CandidateRig;
    TArray<FWallhackContact> CandidateContacts;
    if (!ParsePacket(JsonText, CandidateRig, CandidateContacts))
    {
        UE_LOG(LogTemp, Warning, TEXT("Wallhack HUD discarded malformed bridge packet."));
        return;
    }

    LastRig = CandidateRig;
    LastContacts = MoveTemp(CandidateContacts);
    LastPacketSeconds = FPlatformTime::Seconds();
}

bool UWallhackTelemetrySubsystem::ParsePacket(const FString& JsonText, FWallhackRigPose& OutRig, TArray<FWallhackContact>& OutContacts) const
{
    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid()) return false;

    const TSharedPtr<FJsonObject>* RigObject = nullptr;
    if (!Root->TryGetObjectField(TEXT("rig"), RigObject) || !RigObject || !RigObject->IsValid()) return false;

    double Value = 0.0;
    if (!(*RigObject)->TryGetNumberField(TEXT("x"), Value)) return false;
    OutRig.X = static_cast<float>(Value);
    if (!(*RigObject)->TryGetNumberField(TEXT("y"), Value)) return false;
    OutRig.Y = static_cast<float>(Value);
    if (!(*RigObject)->TryGetNumberField(TEXT("heading_deg"), Value)) return false;
    OutRig.HeadingDegrees = static_cast<float>(Value);

    const TArray<TSharedPtr<FJsonValue>>* DetectionValues = nullptr;
    if (!Root->TryGetArrayField(TEXT("detections"), DetectionValues) || !DetectionValues) return false;

    const UWallhackHUDSettings* Settings = GetDefault<UWallhackHUDSettings>();
    for (const TSharedPtr<FJsonValue>& DetectionValue : *DetectionValues)
    {
        const TSharedPtr<FJsonObject> Detection = DetectionValue.IsValid() ? DetectionValue->AsObject() : nullptr;
        if (!Detection.IsValid()) continue;

        FWallhackContact Contact;
        double Id = 0.0;
        if (!Detection->TryGetNumberField(TEXT("id"), Id)) continue;
        Contact.Id = FMath::RoundToInt(Id);
        double ContactX = 0.0;
        double ContactY = 0.0;
        double Confidence = 0.0;
        if (!Detection->TryGetNumberField(TEXT("x"), ContactX)) continue;
        if (!Detection->TryGetNumberField(TEXT("y"), ContactY)) continue;
        if (!Detection->TryGetNumberField(TEXT("conf"), Confidence)) continue;
        Contact.X = static_cast<float>(ContactX);
        Contact.Y = static_cast<float>(ContactY);
        Contact.Confidence = static_cast<float>(Confidence);

        const float Dx = Contact.X - OutRig.X;
        const float Dy = Contact.Y - OutRig.Y;
        Contact.RangeMeters = FMath::Sqrt(Dx * Dx + Dy * Dy);

        // Production convention: atan2(dx, dy), clockwise from forward. The two settings are
        // intentionally prominent because real fusion orientation has not been calibrated yet.
        const float BearingX = Settings->bSwapBearingAxes ? Dy : Dx;
        const float BearingY = Settings->bSwapBearingAxes ? Dx : Dy;
        float AbsoluteBearing = FMath::RadiansToDegrees(FMath::Atan2(BearingX, BearingY));
        if (Settings->bInvertBearing) AbsoluteBearing = -AbsoluteBearing;
        Contact.RelativeBearingDegrees = NormalizeDegrees(AbsoluteBearing - OutRig.HeadingDegrees + Settings->BearingOffsetDegrees);
        Contact.State = StateForConfidence(Contact.Confidence);

        // Optional. Most bridges won't send this yet -- Contact.Classification
        // simply stays Unknown, which the HUD renders as "no IFF data", not as
        // "confirmed friendly". Never infer a classification from confidence.
        FString ClassificationText;
        if (Detection->TryGetStringField(TEXT("classification"), ClassificationText))
        {
            if (ClassificationText.Equals(TEXT("hostile"), ESearchCase::IgnoreCase)) Contact.Classification = EWallhackContactClassification::Hostile;
            else if (ClassificationText.Equals(TEXT("friendly"), ESearchCase::IgnoreCase)) Contact.Classification = EWallhackContactClassification::Friendly;
            else if (ClassificationText.Equals(TEXT("neutral"), ESearchCase::IgnoreCase)) Contact.Classification = EWallhackContactClassification::Neutral;
        }

        // Optional, same pattern as classification just above: most bridges
        // (and definitely no through-wall sensor yet) won't send any of
        // this, so it simply stays Unknown/false/0, which the HUD renders
        // as "no posture data", never as a guessed stick figure.
        FString PostureText;
        if (Detection->TryGetStringField(TEXT("posture"), PostureText))
        {
            if (PostureText.Equals(TEXT("standing"), ESearchCase::IgnoreCase)) Contact.Posture = EWallhackContactPosture::Standing;
            else if (PostureText.Equals(TEXT("crouched"), ESearchCase::IgnoreCase)) Contact.Posture = EWallhackContactPosture::Crouched;
            else if (PostureText.Equals(TEXT("prone"), ESearchCase::IgnoreCase)) Contact.Posture = EWallhackContactPosture::Prone;
        }
        bool bVitalsLocked = false;
        if (Detection->TryGetBoolField(TEXT("vitals_locked"), bVitalsLocked))
        {
            Contact.bVitalsLocked = bVitalsLocked;
        }
        double BreathingRateBpm = 0.0;
        if (Contact.bVitalsLocked && Detection->TryGetNumberField(TEXT("breathing_bpm"), BreathingRateBpm))
        {
            Contact.BreathingRateBpm = static_cast<float>(BreathingRateBpm);
        }
        OutContacts.Add(Contact);
    }
    return true;
}

EWallhackContactState UWallhackTelemetrySubsystem::StateForConfidence(float Confidence)
{
    if (Confidence >= 0.72f) return EWallhackContactState::Nominal;
    if (Confidence >= 0.50f) return EWallhackContactState::Degraded;
    return EWallhackContactState::Critical;
}

float UWallhackTelemetrySubsystem::NormalizeDegrees(float Degrees)
{
    return FMath::UnwindDegrees(Degrees);
}

EWallhackLinkState UWallhackTelemetrySubsystem::CurrentLinkState(double Now) const
{
    if (!bConnected) return EWallhackLinkState::Disconnected;
    if (LastPacketSeconds < 0.0) return EWallhackLinkState::Connecting;
    return (Now - LastPacketSeconds) <= GetDefault<UWallhackHUDSettings>()->StaleAfterSeconds
        ? EWallhackLinkState::Live
        : EWallhackLinkState::Stale;
}
