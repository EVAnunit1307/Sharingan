#include "WallhackHUD.h"

#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "HAL/PlatformMisc.h"
#include "WallhackHUDSettings.h"
#include "WallhackHUDPalette.h"
#include "WallhackVRHUDActor.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace WallhackHUD
{
    constexpr float CompassWindowDegrees = 120.f;
    constexpr float CompassTickDegrees = 15.f;
}

void AWallhackHUD::DrawHUD()
{
    Super::DrawHUD();
    if (!Canvas) return;

#if !PLATFORM_ANDROID && !UE_BUILD_SHIPPING
    if (FParse::Param(FCommandLine::Get(), TEXT("WallhackTrackingPreview")) || FParse::Param(FCommandLine::Get(), TEXT("WallhackNavigationPreview")))
    {
        // Render the same texture used on Quest. A stereo layer has no desktop
        // compositor, and the older telemetry preview can cover the 3D dot.
        for (TActorIterator<AWallhackVRHUDActor> It(GetWorld()); It; ++It)
        {
            if (UTextureRenderTarget2D* Texture = It->GetHUDRenderTarget())
            {
                DrawTexture(Texture, 0.f, 0.f, Canvas->SizeX, Canvas->SizeY,
                    0.f, 0.f, 1.f, 1.f, FLinearColor::White, BLEND_Translucent);
            }
            break;
        }
        return;
    }
#endif
    if (!GetGameInstance()) return;

    const UWallhackTelemetrySubsystem* Telemetry = GetGameInstance()->GetSubsystem<UWallhackTelemetrySubsystem>();
    if (!Telemetry) return;

    const float Width = Canvas->SizeX;
    const float Height = Canvas->SizeY;
    const FWallhackDisplayFrame Frame = Telemetry->GetDisplayFrame();
    if (Frame.bIsSimulated)
    {
        DrawDesktopOperatorPreview(Frame, Width, Height);
        return;
    }

    UFont* Font = GEngine ? GEngine->GetSmallFont() : nullptr;
    const FLinearColor LinkColor = Frame.LinkState == EWallhackLinkState::Live
        ? FLinearColor(0.25f, 1.0f, 0.50f)
        : FLinearColor(1.0f, 0.18f, 0.12f);

    DrawCompass(Frame, Width, Height);
    DrawText(FString::Printf(TEXT("CONTACTS  %02d"), Frame.Contacts.Num()), FLinearColor::White, 36.f, Height - 102.f, Font, 1.25f);
    DrawText(LinkText(Frame.LinkState), LinkColor, 36.f, Height - 72.f, Font, 1.1f);
    DrawText(FString::Printf(TEXT("RIG  X %.2f  Y %.2f  HDG %03.0f"), Frame.Rig.X, Frame.Rig.Y, Frame.Rig.HeadingDegrees),
        FLinearColor(0.76f, 0.86f, 0.95f), 36.f, Height - 42.f, Font, 0.95f);

    if (Frame.LinkState != EWallhackLinkState::Live)
    {
        const FString Alert = Frame.LinkState == EWallhackLinkState::Stale ? TEXT("SENSOR OFFLINE") : TEXT("LINK LOST");
        const float AlertWidth = 330.f;
        DrawRect(FLinearColor(0.f, 0.f, 0.f, 0.70f), Width * .5f - AlertWidth * .5f, Height * .5f - 46.f, AlertWidth, 92.f);
        DrawText(Alert, FLinearColor(1.f, .18f, .12f), Width * .5f - 132.f, Height * .5f - 18.f, Font, 1.65f);
        DrawText(TEXT("CONTACTS HIDDEN UNTIL FRESH DATA"), FLinearColor::White, Width * .5f - 142.f, Height * .5f + 18.f, Font, .75f);
        return;
    }

    for (const FWallhackContact& Contact : Frame.Contacts)
    {
        const float NormalizedBearing = FMath::Clamp(Contact.RelativeBearingDegrees / (WallhackHUD::CompassWindowDegrees * .5f), -1.f, 1.f);
        const float TagX = Width * .5f + NormalizedBearing * Width * .34f;
        // There is no vertical/elevation data in the bridge contract. Range is intentionally used
        // only to spread screen-space tags, not to imply a spatially registered world position.
        const float TagY = Height * .48f + FMath::Clamp(Contact.RangeMeters, 0.f, 10.f) * 13.f;
        DrawContactTag(Contact, TagX, TagY, 94.f);
    }
}

void AWallhackHUD::DrawDesktopOperatorPreview(const FWallhackDisplayFrame& Frame, float Width, float Height)
{
    UFont* Font = GEngine ? GEngine->GetSmallFont() : nullptr;
    const FLinearColor Cyan = WallhackHUDPalette::Accent;
    const FLinearColor Dim = WallhackHUDPalette::Secondary;
    const FLinearColor Green = WallhackHUDPalette::Accent;
    const FLinearColor Amber(1.f, .68f, .08f, 1.f);
    const FLinearColor Red(1.f, .18f, .12f, 1.f);
    const FLinearColor Panel = WallhackHUDPalette::Panel;

    // Keep this layout deliberately close to the Quest compositor design. It is a simulator only.
    DrawRect(FLinearColor(.005f, .012f, .028f, 1.f), 0.f, 0.f, Width, Height);
    DrawCompass(Frame, Width, Height);
    DrawText(TEXT("DESKTOP OPERATOR PREVIEW  /  SIMULATED DATA  /  NOT OPERATIONAL"), Amber, Width * .5f - 230.f, 12.f, Font, .92f);

    const float MapLeft = 30.f, MapTop = Height - 305.f, MapWidth = Width * .30f, MapHeight = 270.f;
    DrawRect(Panel, MapLeft, MapTop, MapWidth, MapHeight);
    DrawLine(MapLeft, MapTop, MapLeft + MapWidth, MapTop, Cyan, 2.f); DrawLine(MapLeft, MapTop, MapLeft, MapTop + MapHeight, Cyan, 2.f);
    DrawText(TEXT("TACTICAL MAP / RELATIVE TO RIG"), Cyan, MapLeft + 12.f, MapTop + 12.f, Font, .85f);
    const float MapCX = MapLeft + MapWidth * .5f, MapCY = MapTop + MapHeight * .56f;
    for (int32 Index = -2; Index <= 2; ++Index)
    {
        DrawLine(MapCX + Index * MapWidth * .14f, MapTop + 48.f, MapCX + Index * MapWidth * .14f, MapTop + MapHeight - 42.f, FLinearColor(Dim.R, Dim.G, Dim.B, .45f), 1.f);
        DrawLine(MapLeft + 34.f, MapCY + Index * MapHeight * .10f, MapLeft + MapWidth - 34.f, MapCY + Index * MapHeight * .10f, FLinearColor(Dim.R, Dim.G, Dim.B, .45f), 1.f);
    }
    DrawLine(MapCX - 8.f, MapCY, MapCX + 8.f, MapCY, FLinearColor::White, 2.f); DrawLine(MapCX, MapCY - 8.f, MapCX, MapCY + 8.f, FLinearColor::White, 2.f);
    float Radius = 3.f;
    for (const FWallhackContact& Contact : Frame.Contacts) Radius = FMath::Max(Radius, FMath::Max(FMath::Abs(Contact.X - Frame.Rig.X), FMath::Abs(Contact.Y - Frame.Rig.Y)) + 1.f);
    for (const FWallhackContact& Contact : Frame.Contacts)
    {
        const float X = MapCX + FMath::Clamp((Contact.X - Frame.Rig.X) / Radius, -1.f, 1.f) * MapWidth * .37f;
        const float Y = MapCY - FMath::Clamp((Contact.Y - Frame.Rig.Y) / Radius, -1.f, 1.f) * MapHeight * .25f;
        const FLinearColor Color = ColorFor(Contact.State);
        DrawRect(Color, X - 4.f, Y - 4.f, 8.f, 8.f);
        DrawText(FString::Printf(TEXT("%02d  %.0f%%"), Contact.Id, Contact.Confidence * 100.f), Color, X + 8.f, Y - 9.f, Font, .68f);
    }
    DrawText(FString::Printf(TEXT("SIM CONTACTS %02d / ±%.1fm"), Frame.Contacts.Num(), Radius), Amber, MapLeft + 12.f, MapTop + MapHeight - 27.f, Font, .74f);

    const float CardWidth = Width * .29f, CardLeft = Width - CardWidth - 30.f, CardTop = 130.f;
    DrawRect(Panel, CardLeft, CardTop, CardWidth, 230.f);
    DrawLine(CardLeft, CardTop, CardLeft + CardWidth, CardTop, Cyan, 2.f); DrawLine(CardLeft + CardWidth, CardTop, CardLeft + CardWidth, CardTop + 230.f, Cyan, 2.f);
    DrawText(TEXT("SYSTEM TELEMETRY"), Cyan, CardLeft + 14.f, CardTop + 15.f, Font, .92f);
    DrawText(TEXT("GROUND LINK / SIMULATED LIVE"), Amber, CardLeft + 14.f, CardTop + 55.f, Font, .88f);
    DrawText(TEXT("PACKET AGE / 42 ms"), Dim, CardLeft + 14.f, CardTop + 87.f, Font, .76f);
    DrawText(FString::Printf(TEXT("RIG / X %.2f Y %.2f HDG %03.0f°"), Frame.Rig.X, Frame.Rig.Y, Frame.Rig.HeadingDegrees), Dim, CardLeft + 14.f, CardTop + 118.f, Font, .70f);
    const int32 Battery = FPlatformMisc::GetBatteryLevel();
    DrawText(Battery >= 0 ? FString::Printf(TEXT("LAPTOP BATTERY / %d%%"), Battery) : TEXT("LAPTOP BATTERY / UNAVAILABLE"), Battery >= 0 && Battery < 20 ? Red : Dim, CardLeft + 14.f, CardTop + 152.f, Font, .76f);
    DrawText(FPlatformMisc::GetNetworkConnectionType() == ENetworkConnectionType::WiFi ? TEXT("NETWORK / WIFI") : TEXT("NETWORK / UNKNOWN"), Dim, CardLeft + 14.f, CardTop + 183.f, Font, .76f);
    DrawText(FString::Printf(TEXT("LOCAL %s"), *FDateTime::Now().ToString(TEXT("%H:%M:%S"))), FLinearColor::White, CardLeft + 14.f, CardTop + 210.f, Font, .76f);

    const float VideoWidth = Width * .30f, VideoLeft = Width - VideoWidth - 30.f, VideoTop = Height - 285.f;
    DrawRect(Panel, VideoLeft, VideoTop, VideoWidth, 250.f);
    DrawLine(VideoLeft, VideoTop, VideoLeft + VideoWidth, VideoTop, Cyan, 2.f); DrawLine(VideoLeft + VideoWidth, VideoTop, VideoLeft + VideoWidth, VideoTop + 250.f, Cyan, 2.f);
    DrawText(TEXT("DRONE VIDEO / RECEIVER"), Cyan, VideoLeft + 14.f, VideoTop + 12.f, Font, .85f);
    DrawLine(VideoLeft + 24.f, VideoTop + 58.f, VideoLeft + VideoWidth - 24.f, VideoTop + 58.f, Dim, 1.f); DrawLine(VideoLeft + 24.f, VideoTop + 58.f, VideoLeft + 24.f, VideoTop + 182.f, Dim, 1.f);
    DrawLine(VideoLeft + VideoWidth - 24.f, VideoTop + 58.f, VideoLeft + VideoWidth - 24.f, VideoTop + 182.f, Dim, 1.f); DrawLine(VideoLeft + 24.f, VideoTop + 182.f, VideoLeft + VideoWidth - 24.f, VideoTop + 182.f, Dim, 1.f);
    DrawText(TEXT("VIDEO RECEIVER STANDBY"), Dim, VideoLeft + 55.f, VideoTop + 112.f, Font, .90f);
    DrawText(TEXT("NO VIDEO ENDPOINT CONFIGURED"), Amber, VideoLeft + 36.f, VideoTop + 208.f, Font, .70f);

    DrawText(TEXT("PRODUCTION SAFETY: SIMULATED CONTACTS ARE NEVER DEPLOYED TO QUEST"), Green, Width * .5f - 215.f, Height - 26.f, Font, .72f);
}

void AWallhackHUD::DrawCompass(const FWallhackDisplayFrame& Frame, float Width, float Height)
{
    UFont* Font = GEngine ? GEngine->GetSmallFont() : nullptr;
    const float StripWidth = Width * .72f;
    const float Left = (Width - StripWidth) * .5f;
    const float CenterX = Width * .5f;
    const float Top = 42.f;
    const float Bottom = Top + 56.f;
    const FLinearColor LineColor = WallhackHUDPalette::Accent;
    DrawRect(FLinearColor(0.f, 0.f, 0.f, .38f), Left, Top - 10.f, StripWidth, 82.f);
    DrawLine(Left, Bottom, Left + StripWidth, Bottom, LineColor, 2.f);
    DrawLine(CenterX, Top - 5.f, CenterX, Bottom + 12.f, FLinearColor::White, 3.f);

    const float StartHeading = Frame.Rig.HeadingDegrees - WallhackHUD::CompassWindowDegrees * .5f;
    for (float TickHeading = FMath::CeilToFloat(StartHeading / WallhackHUD::CompassTickDegrees) * WallhackHUD::CompassTickDegrees;
        TickHeading <= Frame.Rig.HeadingDegrees + WallhackHUD::CompassWindowDegrees * .5f;
        TickHeading += WallhackHUD::CompassTickDegrees)
    {
        const float Relative = FMath::UnwindDegrees(TickHeading - Frame.Rig.HeadingDegrees);
        const float X = CenterX + (Relative / (WallhackHUD::CompassWindowDegrees * .5f)) * StripWidth * .5f;
        const bool bCardinal = FMath::IsNearlyZero(FMath::Fmod(FMath::Abs(TickHeading), 90.f), .1f);
        DrawLine(X, Bottom, X, Bottom - (bCardinal ? 20.f : 12.f), LineColor, 1.5f);
        if (bCardinal)
        {
            const int32 Cardinal = static_cast<int32>(FMath::RoundToInt(FMath::Fmod(TickHeading + 3600.f, 360.f)));
            const TCHAR* Name = Cardinal == 0 ? TEXT("N") : Cardinal == 90 ? TEXT("E") : Cardinal == 180 ? TEXT("S") : TEXT("W");
            DrawText(Name, FLinearColor::White, X - 4.f, Top + 3.f, Font, 1.0f);
        }
    }

    for (const FWallhackContact& Contact : Frame.Contacts)
    {
        if (FMath::Abs(Contact.RelativeBearingDegrees) > WallhackHUD::CompassWindowDegrees * .5f) continue;
        const float X = CenterX + (Contact.RelativeBearingDegrees / (WallhackHUD::CompassWindowDegrees * .5f)) * StripWidth * .5f;
        const FLinearColor Color = ColorFor(Contact.State);
        DrawLine(X, Bottom + 4.f, X, Bottom + 18.f, Color, 4.f);
        DrawText(FString::FromInt(Contact.Id), Color, X - 4.f, Bottom + 20.f, Font, .8f);
    }
}

void AWallhackHUD::DrawContactTag(const FWallhackContact& Contact, float X, float Y, float Size)
{
    UFont* Font = GEngine ? GEngine->GetSmallFont() : nullptr;
    const FLinearColor Color = ColorFor(Contact.State);
    const float Left = X - Size * .5f;
    const float Top = Y - Size * .5f;
    DrawCornerBrackets(Left, Top, Size, Color);
    DrawStickFigure(X, Y, Size * .34f, Color);
    DrawText(FString::Printf(TEXT("ID %02d  %03.0f%%  %.1fm"), Contact.Id, Contact.Confidence * 100.f, Contact.RangeMeters),
        Color, Left, Top + Size + 8.f, Font, .86f);
}

void AWallhackHUD::DrawCornerBrackets(float Left, float Top, float Size, const FLinearColor& Color)
{
    const float Right = Left + Size;
    const float Bottom = Top + Size;
    const float Corner = Size * .27f;
    const float Thickness = 2.5f;
    DrawLine(Left, Top, Left + Corner, Top, Color, Thickness); DrawLine(Left, Top, Left, Top + Corner, Color, Thickness);
    DrawLine(Right, Top, Right - Corner, Top, Color, Thickness); DrawLine(Right, Top, Right, Top + Corner, Color, Thickness);
    DrawLine(Left, Bottom, Left + Corner, Bottom, Color, Thickness); DrawLine(Left, Bottom, Left, Bottom - Corner, Color, Thickness);
    DrawLine(Right, Bottom, Right - Corner, Bottom, Color, Thickness); DrawLine(Right, Bottom, Right, Bottom - Corner, Color, Thickness);
}

void AWallhackHUD::DrawStickFigure(float X, float Y, float Scale, const FLinearColor& Color)
{
    const float HeadRadius = Scale * .18f;
    const float HeadY = Y - Scale * .35f;
    constexpr int32 HeadSegments = 12;
    for (int32 Segment = 0; Segment < HeadSegments; ++Segment)
    {
        const float A = 6.283185f * static_cast<float>(Segment) / HeadSegments;
        const float B = 6.283185f * static_cast<float>(Segment + 1) / HeadSegments;
        DrawLine(X + FMath::Cos(A) * HeadRadius, HeadY + FMath::Sin(A) * HeadRadius,
            X + FMath::Cos(B) * HeadRadius, HeadY + FMath::Sin(B) * HeadRadius, Color, 2.f);
    }
    DrawLine(X, HeadY + HeadRadius, X, Y + Scale * .28f, Color, 2.f);
    DrawLine(X - Scale * .30f, Y - Scale * .04f, X + Scale * .30f, Y - Scale * .04f, Color, 2.f);
    DrawLine(X, Y + Scale * .28f, X - Scale * .26f, Y + Scale * .58f, Color, 2.f);
    DrawLine(X, Y + Scale * .28f, X + Scale * .26f, Y + Scale * .58f, Color, 2.f);
}

FLinearColor AWallhackHUD::ColorFor(EWallhackContactState State)
{
    switch (State)
    {
        case EWallhackContactState::Nominal: return WallhackHUDPalette::Accent;
        case EWallhackContactState::Degraded: return FLinearColor(1.f, .68f, .08f);
        default: return FLinearColor(1.f, .18f, .12f);
    }
}

FString AWallhackHUD::LinkText(EWallhackLinkState State)
{
    switch (State)
    {
        case EWallhackLinkState::Live: return TEXT("LINK LIVE");
        case EWallhackLinkState::Stale: return TEXT("SENSOR OFFLINE (STALE)");
        case EWallhackLinkState::Connecting: return TEXT("CONNECTING");
        default: return TEXT("LINK LOST");
    }
}
