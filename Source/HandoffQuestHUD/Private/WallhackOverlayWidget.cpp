#include "WallhackOverlayWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "HAL/PlatformMisc.h"
#include "Kismet/GameplayStatics.h"
#include "WallhackTelemetrySubsystem.h"
#include "WallhackHUDPalette.h"

namespace
{
    const FLinearColor Cyan = WallhackHUDPalette::Accent;
    const FLinearColor DimCyan = WallhackHUDPalette::Secondary;
    const FLinearColor PanelFill = WallhackHUDPalette::Panel;
    const FLinearColor Good = WallhackHUDPalette::Accent;
    constexpr FLinearColor Warn(1.0f, 0.68f, 0.08f, 1.0f);
    constexpr FLinearColor Bad(1.0f, 0.20f, 0.14f, 1.0f);

    UTextBlock* MakeLabel(UWidgetTree* Tree, const FName Name, const FString& Text, int32 FontSize, const FLinearColor& Color, ETextJustify::Type Justification = ETextJustify::Left)
    {
        UTextBlock* Label = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), Name);
        Label->SetText(FText::FromString(Text));
        Label->SetColorAndOpacity(FSlateColor(Color));
        Label->SetShadowColorAndOpacity(FLinearColor::Black);
        Label->SetShadowOffset(FVector2D(2.f, 2.f));
        Label->SetJustification(Justification);
        Label->SetFont(FSlateFontInfo(Label->GetFont().FontObject, FontSize));
        return Label;
    }

    UBorder* MakePanel(UWidgetTree* Tree, const FName Name)
    {
        UBorder* Panel = Tree->ConstructWidget<UBorder>(UBorder::StaticClass(), Name);
        Panel->SetBrushColor(PanelFill);
        Panel->SetPadding(FMargin(18.f, 14.f));
        return Panel;
    }

    FString LinkLabel(EWallhackLinkState State)
    {
        switch (State)
        {
            case EWallhackLinkState::Live: return TEXT("GROUND LINK  /  LIVE");
            case EWallhackLinkState::Connecting: return TEXT("GROUND LINK  /  ACQUIRING");
            case EWallhackLinkState::Stale: return TEXT("GROUND LINK  /  SENSOR OFFLINE");
            default: return TEXT("GROUND LINK  /  DISCONNECTED");
        }
    }

    FLinearColor LinkColor(EWallhackLinkState State)
    {
        return State == EWallhackLinkState::Live ? Good : State == EWallhackLinkState::Connecting ? Warn : Bad;
    }

    FLinearColor ContactColor(EWallhackContactState State)
    {
        return State == EWallhackContactState::Nominal ? Good : State == EWallhackContactState::Degraded ? Warn : Bad;
    }

    FString NetworkLabel(ENetworkConnectionType Type)
    {
        switch (Type)
        {
            case ENetworkConnectionType::WiFi: return TEXT("HEADSET NETWORK  /  WIFI");
            case ENetworkConnectionType::Ethernet: return TEXT("HEADSET NETWORK  /  ETHERNET");
            case ENetworkConnectionType::Cell: return TEXT("HEADSET NETWORK  /  CELLULAR");
            case ENetworkConnectionType::None: return TEXT("HEADSET NETWORK  /  OFFLINE");
            default: return TEXT("HEADSET NETWORK  /  UNKNOWN");
        }
    }
}

void UWallhackOverlayWidget::NativeConstruct()
{
    Super::NativeConstruct();

    UCanvasPanel* Canvas = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("WallhackOverlayCanvas"));
    WidgetTree->RootWidget = Canvas;

    UBorder* Header = MakePanel(WidgetTree, TEXT("WallhackHeader"));
    UCanvasPanelSlot* HeaderSlot = Canvas->AddChildToCanvas(Header);
    HeaderSlot->SetAnchors(FAnchors(0.5f, 0.f)); HeaderSlot->SetAlignment(FVector2D(0.5f, 0.f));
    HeaderSlot->SetPosition(FVector2D(0.f, 24.f)); HeaderSlot->SetSize(FVector2D(1500.f, 155.f));
    UVerticalBox* HeaderContent = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("WallhackHeaderContent"));
    Header->SetContent(HeaderContent);
    HeadingText = MakeLabel(WidgetTree, TEXT("WallhackHeading"), TEXT("OPS NAV  /  HEADING 000°"), 47, Cyan, ETextJustify::Center);
    HeaderContent->AddChildToVerticalBox(HeadingText)->SetHorizontalAlignment(HAlign_Fill);
    CompassText = MakeLabel(WidgetTree, TEXT("WallhackCompass"), TEXT("W   ──  NW  ──  N  ──  NE  ──  E"), 25, DimCyan, ETextJustify::Center);
    UVerticalBoxSlot* CompassSlot = HeaderContent->AddChildToVerticalBox(CompassText);
    CompassSlot->SetPadding(FMargin(0.f, 9.f, 0.f, 0.f));
    CompassSlot->SetHorizontalAlignment(HAlign_Fill);

    UBorder* MapPanel = MakePanel(WidgetTree, TEXT("WallhackMinimap"));
    UCanvasPanelSlot* MapSlot = Canvas->AddChildToCanvas(MapPanel);
    MapSlot->SetAnchors(FAnchors(0.f, 1.f)); MapSlot->SetAlignment(FVector2D(0.f, 1.f));
    MapSlot->SetPosition(FVector2D(32.f, -32.f)); MapSlot->SetSize(FVector2D(610.f, 455.f));
    MinimapCanvas = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("WallhackMinimapCanvas"));
    MapPanel->SetContent(MinimapCanvas);
    UTextBlock* MapTitle = MakeLabel(WidgetTree, TEXT("WallhackMapTitle"), TEXT("TACTICAL MAP  /  RELATIVE TO RIG"), 27, Cyan);
    UCanvasPanelSlot* MapTitleSlot = MinimapCanvas->AddChildToCanvas(MapTitle); MapTitleSlot->SetPosition(FVector2D(10.f, 4.f)); MapTitleSlot->SetSize(FVector2D(570.f, 38.f));
    UTextBlock* MapGrid = MakeLabel(WidgetTree, TEXT("WallhackMapGrid"), TEXT("┌──────────────────────┐\n│          │           │\n│          │           │\n├──────────◆───────────┤\n│          │           │\n│          │           │\n└──────────────────────┘"), 25, FLinearColor(0.17f, 0.52f, 0.63f, 0.78f), ETextJustify::Center);
    UCanvasPanelSlot* MapGridSlot = MinimapCanvas->AddChildToCanvas(MapGrid); MapGridSlot->SetPosition(FVector2D(60.f, 62.f)); MapGridSlot->SetSize(FVector2D(500.f, 260.f));
    RigMarker = MakeLabel(WidgetTree, TEXT("WallhackRigMarker"), TEXT("▲ RIG"), 20, FLinearColor::White, ETextJustify::Center);
    UCanvasPanelSlot* RigSlot = MinimapCanvas->AddChildToCanvas(RigMarker); RigSlot->SetPosition(FVector2D(254.f, 202.f)); RigSlot->SetSize(FVector2D(100.f, 30.f));
    ContactText = MakeLabel(WidgetTree, TEXT("WallhackContacts"), TEXT("CONTACTS 00  /  AWAITING FRESH DATA"), 21, DimCyan);
    UCanvasPanelSlot* ContactSlot = MinimapCanvas->AddChildToCanvas(ContactText); ContactSlot->SetPosition(FVector2D(10.f, 355.f)); ContactSlot->SetSize(FVector2D(570.f, 30.f));
    MapScaleText = MakeLabel(WidgetTree, TEXT("WallhackMapScale"), TEXT("MAP SCALE  /  -- m"), 18, DimCyan);
    UCanvasPanelSlot* ScaleSlot = MinimapCanvas->AddChildToCanvas(MapScaleText); ScaleSlot->SetPosition(FVector2D(10.f, 390.f)); ScaleSlot->SetSize(FVector2D(570.f, 25.f));

    UBorder* VideoPanel = MakePanel(WidgetTree, TEXT("WallhackVideo"));
    UCanvasPanelSlot* VideoSlot = Canvas->AddChildToCanvas(VideoPanel);
    VideoSlot->SetAnchors(FAnchors(1.f, 1.f)); VideoSlot->SetAlignment(FVector2D(1.f, 1.f));
    VideoSlot->SetPosition(FVector2D(-32.f, -32.f)); VideoSlot->SetSize(FVector2D(640.f, 390.f));
    UVerticalBox* VideoContent = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("WallhackVideoContent"));
    VideoPanel->SetContent(VideoContent);
    VideoContent->AddChildToVerticalBox(MakeLabel(WidgetTree, TEXT("WallhackVideoTitle"), TEXT("DRONE VIDEO  /  INPUT"), 29, Cyan))->SetHorizontalAlignment(HAlign_Fill);
    UVerticalBoxSlot* VideoViewportSlot = VideoContent->AddChildToVerticalBox(MakeLabel(WidgetTree, TEXT("WallhackVideoViewport"), TEXT("╔══════════════════════════╗\n║                          ║\n║       VIDEO WINDOW       ║\n║                          ║\n╚══════════════════════════╝"), 27, FLinearColor(0.18f, 0.55f, 0.68f, 0.9f), ETextJustify::Center));
    VideoViewportSlot->SetPadding(FMargin(0.f, 22.f, 0.f, 12.f));
    VideoViewportSlot->SetHorizontalAlignment(HAlign_Fill);
    VideoStateText = MakeLabel(WidgetTree, TEXT("WallhackVideoState"), TEXT("NO VIDEO ENDPOINT CONFIGURED"), 20, Warn, ETextJustify::Center);
    VideoContent->AddChildToVerticalBox(VideoStateText)->SetHorizontalAlignment(HAlign_Fill);

    UBorder* TelemetryPanel = MakePanel(WidgetTree, TEXT("WallhackTelemetry"));
    UCanvasPanelSlot* TelemetrySlot = Canvas->AddChildToCanvas(TelemetryPanel);
    TelemetrySlot->SetAnchors(FAnchors(1.f, 0.f)); TelemetrySlot->SetAlignment(FVector2D(1.f, 0.f));
    TelemetrySlot->SetPosition(FVector2D(-32.f, 210.f)); TelemetrySlot->SetSize(FVector2D(600.f, 310.f));
    UVerticalBox* TelemetryContent = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("WallhackTelemetryContent"));
    TelemetryPanel->SetContent(TelemetryContent);
    TelemetryContent->AddChildToVerticalBox(MakeLabel(WidgetTree, TEXT("WallhackTelemetryTitle"), TEXT("SYSTEM TELEMETRY"), 28, Cyan));
    LinkText = MakeLabel(WidgetTree, TEXT("WallhackLink"), TEXT("GROUND LINK  /  ACQUIRING"), 26, Warn);
    TelemetryContent->AddChildToVerticalBox(LinkText)->SetPadding(FMargin(0.f, 12.f, 0.f, 3.f));
    PacketAgeText = MakeLabel(WidgetTree, TEXT("WallhackPacketAge"), TEXT("PACKET AGE  /  -- ms"), 21, DimCyan); TelemetryContent->AddChildToVerticalBox(PacketAgeText);
    RigText = MakeLabel(WidgetTree, TEXT("WallhackRig"), TEXT("RIG  X --  Y --  HDG ---°"), 21, DimCyan); TelemetryContent->AddChildToVerticalBox(RigText);
    BatteryText = MakeLabel(WidgetTree, TEXT("WallhackBattery"), TEXT("HEADSET BATTERY  /  --%"), 21, DimCyan); TelemetryContent->AddChildToVerticalBox(BatteryText)->SetPadding(FMargin(0.f, 10.f, 0.f, 0.f));
    NetworkText = MakeLabel(WidgetTree, TEXT("WallhackNetwork"), TEXT("HEADSET NETWORK  /  UNKNOWN"), 21, DimCyan); TelemetryContent->AddChildToVerticalBox(NetworkText);

    TimeText = MakeLabel(WidgetTree, TEXT("WallhackTime"), TEXT("LOCAL --:--:--"), 26, FLinearColor::White, ETextJustify::Right);
    UCanvasPanelSlot* TimeSlot = Canvas->AddChildToCanvas(TimeText);
    TimeSlot->SetAnchors(FAnchors(1.f, 0.f)); TimeSlot->SetAlignment(FVector2D(1.f, 0.f)); TimeSlot->SetPosition(FVector2D(-54.f, 52.f)); TimeSlot->SetSize(FVector2D(360.f, 54.f));
}

void UWallhackOverlayWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
    Super::NativeTick(MyGeometry, InDeltaTime);
    const UWallhackTelemetrySubsystem* Telemetry = GetGameInstance() ? GetGameInstance()->GetSubsystem<UWallhackTelemetrySubsystem>() : nullptr;
    const FWallhackDisplayFrame Frame = Telemetry ? Telemetry->GetDisplayFrame() : FWallhackDisplayFrame{};
    APlayerCameraManager* Camera = UGameplayStatics::GetPlayerCameraManager(this, 0);
    const int32 ViewHeading = Camera ? FMath::RoundToInt(FMath::Fmod(Camera->GetCameraRotation().Yaw + 360.f, 360.f)) : 0;
    HeadingText->SetText(FText::FromString(FString::Printf(TEXT("OPS NAV  /  HEAD %03d°  /  RIG %03.0f°"), ViewHeading, Frame.Rig.HeadingDegrees)));
    CompassText->SetText(FText::FromString(BuildCompassMarkers(Frame)));
    TimeText->SetText(FText::FromString(FString::Printf(TEXT("LOCAL %s"), *FDateTime::Now().ToString(TEXT("%H:%M:%S")))));
    LinkText->SetText(FText::FromString(LinkLabel(Frame.LinkState))); LinkText->SetColorAndOpacity(FSlateColor(LinkColor(Frame.LinkState)));
    PacketAgeText->SetText(FText::FromString(Frame.PacketAgeSeconds == TNumericLimits<float>::Max() ? TEXT("PACKET AGE  /  NO PACKETS") : FString::Printf(TEXT("PACKET AGE  /  %.0f ms"), Frame.PacketAgeSeconds * 1000.f)));
    RigText->SetText(FText::FromString(FString::Printf(TEXT("RIG  X %.2f  Y %.2f  HDG %03.0f°"), Frame.Rig.X, Frame.Rig.Y, Frame.Rig.HeadingDegrees)));
    const int32 Battery = FPlatformMisc::GetBatteryLevel();
    BatteryText->SetText(FText::FromString(Battery >= 0 ? FString::Printf(TEXT("HEADSET BATTERY  /  %d%%"), Battery) : TEXT("HEADSET BATTERY  /  UNAVAILABLE")));
    BatteryText->SetColorAndOpacity(FSlateColor(Battery >= 0 && Battery < 20 ? Bad : DimCyan));
    NetworkText->SetText(FText::FromString(NetworkLabel(FPlatformMisc::GetNetworkConnectionType())));
    UpdateMinimap(Frame);
}

void UWallhackOverlayWidget::UpdateMinimap(const FWallhackDisplayFrame& Frame)
{
    for (UTextBlock* Blip : ContactBlips) if (Blip) Blip->RemoveFromParent();
    ContactBlips.Reset();
    if (Frame.LinkState != EWallhackLinkState::Live)
    {
        ContactText->SetText(FText::FromString(TEXT("CONTACTS HIDDEN  /  FRESH DATA REQUIRED"))); ContactText->SetColorAndOpacity(FSlateColor(Bad));
        MapScaleText->SetText(FText::FromString(TEXT("MAP SCALE  /  -- m"))); return;
    }
    float Radius = 3.f;
    for (const FWallhackContact& Contact : Frame.Contacts) Radius = FMath::Max(Radius, FMath::Max(FMath::Abs(Contact.X - Frame.Rig.X), FMath::Abs(Contact.Y - Frame.Rig.Y)) + 1.f);
    ContactText->SetText(FText::FromString(FString::Printf(TEXT("CONTACTS %02d  /  LIVE FUSED POSITIONS"), Frame.Contacts.Num()))); ContactText->SetColorAndOpacity(FSlateColor(Good));
    MapScaleText->SetText(FText::FromString(FString::Printf(TEXT("MAP SCALE  /  ±%.1f m  /  ▲ RIG"), Radius)));
    for (const FWallhackContact& Contact : Frame.Contacts)
    {
        const float RelativeX = FMath::Clamp((Contact.X - Frame.Rig.X) / Radius, -1.f, 1.f);
        const float RelativeY = FMath::Clamp((Contact.Y - Frame.Rig.Y) / Radius, -1.f, 1.f);
        UTextBlock* Blip = MakeLabel(WidgetTree, *FString::Printf(TEXT("WallhackContact_%d"), Contact.Id), FString::Printf(TEXT("◆%02d"), Contact.Id), 22, ContactColor(Contact.State), ETextJustify::Center);
        UCanvasPanelSlot* BlipSlot = MinimapCanvas->AddChildToCanvas(Blip); BlipSlot->SetPosition(FVector2D(284.f + RelativeX * 210.f, 210.f - RelativeY * 130.f)); BlipSlot->SetSize(FVector2D(70.f, 28.f));
        ContactBlips.Add(Blip);
    }
}

FString UWallhackOverlayWidget::BuildCompassMarkers(const FWallhackDisplayFrame& Frame) const
{
    FString Markers = TEXT("W  ──  NW  ──  N  ──  NE  ──  E");
    if (Frame.LinkState != EWallhackLinkState::Live) return Markers + TEXT("     /     CONTACT MARKERS OFFLINE");
    FString Contacts;
    for (const FWallhackContact& Contact : Frame.Contacts) if (FMath::Abs(Contact.RelativeBearingDegrees) <= 60.f) Contacts += FString::Printf(TEXT("  [%02d %+.0f°]"), Contact.Id, Contact.RelativeBearingDegrees);
    return Contacts.IsEmpty() ? Markers + TEXT("     /     NO CONTACTS IN 120° WINDOW") : Markers + Contacts;
}
