#include "WallhackVRHUDActor.h"

#include "WallhackWorldContact.h"
#include "WallhackSpatialMath.h"
#include "WallhackHUDProjection.h"
#include "WallhackHUDPalette.h"
#include "WallhackCanvasLabels.h"
#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/Canvas.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

void AWallhackVRHUDActor::SelectNextContact()
{
    if (!IsValid(WorldContact)) return;
    TArray<FWallhackSpatialContact> Contacts;
    WorldContact->GetSpatialContacts(Contacts);
    if (Contacts.IsEmpty()) return;
    const int32 Index = Contacts.IndexOfByPredicate([this](const auto& Item) { return Item.Id == SelectedSpatialContactId; });
    SelectedSpatialContactId = Contacts[(Index + 1) % Contacts.Num()].Id;
    UpdateSpatialLabels();
    DrawOperatorHUD();
}

void AWallhackVRHUDActor::CycleMapRange()
{
    SpatialMapRangeMeters = SpatialMapRangeMeters < 10.f ? 10.f : SpatialMapRangeMeters < 20.f ? 20.f : 5.f;
    DrawOperatorHUD();
}

void AWallhackVRHUDActor::ToggleSimulation()
{
    if (IsValid(WorldContact)) WorldContact->SetSimulationPaused(!WorldContact->IsSimulationPaused());
    DrawOperatorHUD();
}

void AWallhackVRHUDActor::ToggleContactUpdates()
{
    if (IsValid(WorldContact)) WorldContact->SetContactUpdatesEnabled(!WorldContact->AreContactUpdatesEnabled());
    DrawOperatorHUD();
}

void AWallhackVRHUDActor::UpdateSpatialLabels()
{
    if (!ContactLabels) return;
    FVector Viewer;
    FQuat Orientation;
    const bool bShow = IsValid(WorldContact) && HUDDensity != EWallhackHUDDensity::Hidden
        && WorldContact->AreMultipleContactsEnabled() && !WorldContact->IsHidden()
        && WorldContact->GetViewerWorldPose(Viewer, Orientation);
    ContactLabels->SetHiddenInGame(!bShow);
    if (!bShow) return;

    TArray<FWallhackSpatialContact> Contacts;
    WorldContact->GetSpatialContacts(Contacts);
    constexpr int32 InstanceCount = 3 * 2 * 7;
    if (ContactLabels->GetInstanceCount() != InstanceCount)
    {
        ContactLabels->ClearInstances();
        for (int32 Index = 0; Index < InstanceCount; ++Index)
            ContactLabels->AddInstance(FTransform(FQuat::Identity, FVector::ZeroVector, FVector::ZeroVector));
    }
    const float Scale = GetWorld()->GetWorldSettings()->WorldToMeters;
    if (!FMath::IsFinite(Scale) || Scale <= 0.f)
    {
        ContactLabels->SetHiddenInGame(true);
        return;
    }
    // Segments: top, upper-right, lower-right, bottom, lower-left,
    // upper-left, middle. Each ellipsoid is a rounded green stroke.
    static const uint8 Digits[] = {0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f};
    static const FVector2D Centers[] = {{0,.5}, {.5,.25}, {.5,-.25}, {0,-.5}, {-.5,-.25}, {-.5,.25}, {0,0}};
    for (int32 Slot = 0; Slot < 3; ++Slot)
    {
        const FWallhackSpatialContact* Contact = Contacts.IsValidIndex(Slot) ? &Contacts[Slot] : nullptr;
        const bool bSelected = Contact && Contact->Id == SelectedSpatialContactId;
        const bool bValid = Contact && Contact->bPositionValid && !Contact->bStale
            && (HUDDensity == EWallhackHUDDensity::Full || bSelected);
        const float Height = bSelected ? .08f : .065f;
        const float Width = Height * .52f;
        for (int32 Digit = 0; Digit < 2; ++Digit)
        {
            const int32 Number = Contact ? (Digit == 0 ? Contact->Id / 10 : Contact->Id % 10) : 0;
            for (int32 Segment = 0; Segment < 7; ++Segment)
            {
                const int32 Index = Slot * 14 + Digit * 7 + Segment;
                const bool bOn = bValid && (Digits[FMath::Clamp(Number, 0, 9)] & (1 << Segment));
                const bool bHorizontal = Segment == 0 || Segment == 3 || Segment == 6;
                const FVector Dimensions(.006f, bHorizontal ? Width : .007f, bHorizontal ? .007f : Height * .5f);
                const FVector Offset(0.f, (Digit == 0 ? -.62f : .62f) * Width + Centers[Segment].X * Width,
                    .11f + Centers[Segment].Y * Height);
                const FVector Position = bOn ? Contact->WorldPosition + Orientation.RotateVector(Offset * Scale) : FVector::ZeroVector;
                ContactLabels->UpdateInstanceTransform(Index,
                    FTransform(Orientation, Position, bOn ? Dimensions * Scale / 100.f : FVector::ZeroVector),
                    true, Index == InstanceCount - 1, true);
            }
        }
    }
}

void AWallhackVRHUDActor::DrawSpatialHUD(UCanvas* Canvas, UFont* Font)
{
    const FLinearColor Green(.12f, .85f, .04f, .9f);
    const FLinearColor Ink(.82f, .88f, .86f, .92f);
    const FLinearColor Soft(.56f, .65f, .62f, .82f);
    const FLinearColor Amber(1.f, .62f, .12f, 1.f);
    auto Fade = [](FLinearColor Color, float Alpha) { Color.A *= Alpha; return Color; };
    auto Label = [this, Canvas, Font](const FString& Value, float X, float Y, float Height, FLinearColor Color, bool bCenter = false)
    {
        if (!Canvas->Canvas) return;
        if (!Font)
        {
            WallhackCanvasLabels::Draw(Canvas, SpatialLabelAtlas, Value, X, Y, Height, Color, bCenter);
            return;
        }
        float Width = 0.f, FontHeight = 0.f;
        Canvas->StrLen(Font, Value, Width, FontHeight);
        const float Scale = Height / FMath::Max(FontHeight, 1.f);
        // Runtime glyphs need destination alpha to survive the Quest compositor.
        const bool bPreviousAlpha = Canvas->Canvas->IsWriteDestinationAlphaSet();
        Canvas->Canvas->SetWriteDestinationAlpha(true);
        Canvas->K2_DrawText(Font, Value, FVector2D(X, Y), FVector2D(Scale), Color, 0.f,
            FLinearColor(0.f, 0.f, 0.f, Color.A), FVector2D(1.f), bCenter, false, true,
            FLinearColor(0.f, 0.f, 0.f, Color.A));
        Canvas->Canvas->SetWriteDestinationAlpha(bPreviousAlpha);
    };
    auto Line = [Canvas](FVector2D A, FVector2D B, FLinearColor Color, float Thickness = 2.f)
    {
        Canvas->K2_DrawLine(A, B, Thickness + 1.5f, FLinearColor(0.f, 0.f, 0.f, Color.A));
        Canvas->K2_DrawLine(A, B, Thickness, Color);
    };
    auto Circle = [Canvas](FVector2D Center, float Radius, FLinearColor Color, float Thickness = 2.f)
    {
        constexpr int32 Segments = 48;
        // Complete the outline first; per-chord outlines erase neighbouring strokes.
        for (int32 Pass = 0; Pass < 2; ++Pass)
        {
            FVector2D Previous = Center + FVector2D(Radius, 0.f);
            for (int32 Index = 1; Index <= Segments; ++Index)
            {
                const float Angle = Index * UE_TWO_PI / Segments;
                const FVector2D Next = Center + FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)) * Radius;
                Canvas->K2_DrawLine(Previous, Next, Thickness + (Pass == 0 ? 1.5f : 0.f),
                    Pass == 0 ? FLinearColor(0.f, 0.f, 0.f, Color.A) : Color);
                Previous = Next;
            }
        }
    };
    if (HUDDensity == EWallhackHUDDensity::Hidden)
    {
        return;
    }

    TArray<FWallhackSpatialContact> Contacts;
    WorldContact->GetSpatialContacts(Contacts);
    if (!Contacts.ContainsByPredicate([this](const auto& Item) { return Item.Id == SelectedSpatialContactId; }))
        SelectedSpatialContactId = Contacts.IsEmpty() ? -1 : Contacts[0].Id;
    FVector Viewer = FVector::ZeroVector;
    FQuat Orientation = FQuat::Identity;
    const bool bViewer = WorldContact->GetViewerWorldPose(Viewer, Orientation);
    const float WorldScale = GetWorld()->GetWorldSettings()->WorldToMeters;
    if (bViewer)
    {
        const float Yaw = Orientation.Rotator().Yaw;
        if (!bHasAutoCalibratedNorth)
        {
            NorthOffsetDegrees = -Yaw;
            bHasAutoCalibratedNorth = true;
        }
        DisplayedHeading = FMath::UnwindDegrees(Yaw + NorthOffsetDegrees);
    }
    const int32 Heading = FMath::RoundToInt(FMath::Fmod(DisplayedHeading + 360.f, 360.f)) % 360;
    int32 ValidCount = 0;
    bool bStale = false;
    bool bHasSample = false;
    for (const auto& Contact : Contacts)
    {
        ValidCount += Contact.bPositionValid && !Contact.bStale ? 1 : 0;
        bStale |= Contact.bStale;
        bHasSample |= Contact.AgeSeconds < TNumericLimits<float>::Max();
    }
    const bool bUpdates = WorldContact->AreContactUpdatesEnabled();
    const FString Status = !bViewer ? TEXT("TRACKING UNAVAILABLE")
        : WorldContact->GetTrackingState() == EWallhackContactTrackingState::Failed ? TEXT("RESTART REQUIRED")
        : !bHasSample ? TEXT("LOCATING CONTACTS") : bStale ? TEXT("CONTACT DATA STALE")
        : ValidCount == 0 ? TEXT("LOCATING CONTACTS") : !bUpdates ? TEXT("UPDATES INTERRUPTED")
        : WorldContact->IsSimulationPaused() ? TEXT("MOTION PAUSED") : TEXT("TRACKING");
    const bool bNeedsAttention = !bViewer || bStale || ValidCount == 0 || !bUpdates
        || WorldContact->GetTrackingState() == EWallhackContactTrackingState::Failed;
    const FLinearColor StatusColor = bNeedsAttention ? Amber : Soft;
    const bool bFull = HUDDensity == EWallhackHUDDensity::Full;

    // Relative heading stays at the upper edge. No north claim or center reticle.
    Label(bViewer ? FString::Printf(TEXT("%03d / REL"), Heading) : TEXT("--- / REL"),
        1024, 145, 24.f, bViewer ? Ink : Amber, true);
    if (bFull) for (int32 Degrees = 0; Degrees < 360; Degrees += 10)
    {
        const float Delta = FMath::FindDeltaAngleDegrees(DisplayedHeading, static_cast<float>(Degrees));
        if (!bViewer || FMath::Abs(Delta) > 40.f) continue;
        const float X = 1024.f + Delta * 4.f;
        const bool bMajor = Degrees % 30 == 0;
        Line({X, 187}, {X, bMajor ? 177.f : 182.f}, Fade(Soft, .5f), 1.f);
    }

    // A small source label remains honest about simulated data. Healthy status
    // stays peripheral; failures get a readable amber message in both modes.
    Label(TEXT("LOCAL SIM"), 160, 980, 18.f, Soft);
    if (bNeedsAttention || WorldContact->IsSimulationPaused())
        Label(Status, 160, 1010, 20.f, StatusColor);
    else if (bFull)
        Label(FString::Printf(TEXT("%02d CONTACTS"), ValidCount), 160, 1010, 18.f, Soft);

    const FVector2D MapCenter(320.f, 790.f);
    constexpr float Radius = 149.f;
    if (bFull)
    {
        Label(FString::Printf(TEXT("LOCAL MAP / %.0f M"), SpatialMapRangeMeters), 160, 610, 20.f, Ink);
        Circle(MapCenter, Radius, Fade(Soft, .4f), 1.f);
        Line(MapCenter + FVector2D(0,-7), MapCenter + FVector2D(-5,5), Soft, 1.5f);
        Line(MapCenter + FVector2D(0,-7), MapCenter + FVector2D(5,5), Soft, 1.5f);
    }

    WallhackSpatialMath::FContactView SelectedView;
    bool bSelectedValid = false;
    const FWallhackSpatialContact* Selected = nullptr;
    TArray<FVector2D, TInlineAllocator<3>> UsedArrowPositions;
    TArray<FVector2D, TInlineAllocator<3>> UsedMapLabels;
    for (const auto& Contact : Contacts)
    {
        const bool bSelected = Contact.Id == SelectedSpatialContactId;
        if (bSelected) Selected = &Contact;
        WallhackSpatialMath::FContactView View;
        if (!Contact.bPositionValid || Contact.bStale || !bViewer
            || !WallhackSpatialMath::ProjectContact(Contact.WorldPosition, Viewer, Orientation.Rotator().Yaw, WorldScale, View)) continue;
        if (bSelected) { SelectedView = View; bSelectedValid = true; }
        const FLinearColor Color = bSelected ? Green : Fade(Green, .8f);
        if (bFull)
        {
            const FVector2D P = MapCenter + WallhackSpatialMath::MapOffset(View, Radius, SpatialMapRangeMeters);
            Circle(P, 7.f, Color, 2.5f);
            if (View.GroundRangeMeters <= SpatialMapRangeMeters) Line(P - FVector2D(3,0), P + FVector2D(3,0), Color, 5.f);
            if (bSelected) Circle(P, 12.f, Fade(Ink, .65f), 1.f);
            FVector2D MapLabel(P.X < MapCenter.X ? P.X - 37.f : P.X + 15.f, P.Y - 6.f);
            for (const auto& Used : UsedMapLabels)
                if (FMath::Abs(MapLabel.X - Used.X) < 26.f && FMath::Abs(MapLabel.Y - Used.Y) < 16.f)
                    MapLabel.Y = Used.Y + 19.f;
            UsedMapLabels.Add(MapLabel);
            if (FMath::Abs(MapLabel.Y - P.Y) > 12.f)
                Line(P + FVector2D(0, 9), MapLabel + FVector2D(10, -3), Fade(Color, .5f), 1.f);
            Label(FString::Printf(TEXT("%02d"), Contact.Id), MapLabel.X, MapLabel.Y, 18.f, bSelected ? Ink : Soft);
        }
        auto Edge = WallhackHUDProjection::ProjectOffscreen(Contact.WorldPosition, Viewer, Orientation);
        if (!Edge.bOutside || (!bFull && !bSelected)) continue;
        // Keep nearby arrow labels in separate rows while preserving the
        // directional arrow itself. Labels remain inside the visor margins.
        FVector2D LabelPosition = Edge.Position - Edge.Direction * 38.f;
        for (const auto& Used : UsedArrowPositions)
            if (FMath::Abs(LabelPosition.X - Used.X) < 150.f && FMath::Abs(LabelPosition.Y - Used.Y) < 34.f)
                LabelPosition.Y = Used.Y + 36.f;
        UsedArrowPositions.Add(LabelPosition);
        const FVector2D Side(-Edge.Direction.Y, Edge.Direction.X);
        Line(Edge.Position - Edge.Direction * 15.f + Side * 9.f, Edge.Position, Color, bSelected ? 3.f : 2.f);
        Line(Edge.Position - Edge.Direction * 15.f - Side * 9.f, Edge.Position, Color, bSelected ? 3.f : 2.f);
        Label(FString::Printf(TEXT("%02d / %.1f M"), Contact.Id, View.RangeMeters),
            FMath::Clamp(LabelPosition.X, 270., 1778.), LabelPosition.Y, 20.f, Ink, true);
    }

    if (bFull && bSelectedValid && SelectedView.GroundRangeMeters > SpatialMapRangeMeters)
    {
        Label(TEXT("SELECTED CONTACT OUTSIDE MAP"), 160, 949, 18.f, Soft);
    }

    // Unboxed peripheral readout. Expanded mode adds one row of geometry,
    // rather than a large opaque information card.
    const float CardY = bFull ? 866.f : 952.f;
    Line({1540, CardY}, {1540, CardY + 54}, bSelectedValid ? Green : Amber, 2.f);
    Label(SelectedSpatialContactId >= 0 ? FString::Printf(TEXT("CONTACT %02d"), SelectedSpatialContactId) : TEXT("NO CONTACT"),
        1558, CardY, 20.f, Soft);
    if (bSelectedValid)
    {
        Label(FString::Printf(TEXT("%.1f m"), SelectedView.RangeMeters), 1558, CardY + 26, 32.f, Ink);
        if (bFull)
        {
            Label(FString::Printf(TEXT("%+.0f DEG / HEIGHT %+.1f M"), SelectedView.BearingDegrees, SelectedView.HeightMeters),
                1558, CardY + 72, 18.f, Soft);
        }
    }
    else
    {
        Label(TEXT("-- m"), 1558, CardY + 26, 32.f, Amber);
        if (bFull) Label(Selected && Selected->bStale ? TEXT("STALE / POSITION HIDDEN") : TEXT("WAITING FOR TRACKING"),
            1558, CardY + 72, 18.f, Amber);
    }

    if (ElapsedSeconds < SpatialHelpUntilSeconds)
    {
        bool bDesktop = false;
#if !PLATFORM_ANDROID && !UE_BUILD_SHIPPING
        bDesktop = FParse::Param(FCommandLine::Get(), TEXT("WallhackTrackingPreview"));
#endif
        const float HelpAlpha = FMath::Clamp(SpatialHelpUntilSeconds - ElapsedSeconds, 0.f, 1.f);
        Label(bDesktop ? TEXT("TAB  SELECT     B  CHANGE VIEW") : TEXT("TRIGGER  SELECT     B  CHANGE VIEW"),
            1024, 1054, 18.f, Fade(Soft, HelpAlpha), true);
    }
}
