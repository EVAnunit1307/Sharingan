#include "WallhackVRHUDActor.h"
#include "WallhackSensorPeopleActor.h"
#include "WallhackCanvasLabels.h"
#include "Engine/Canvas.h"

void AWallhackVRHUDActor::SetSensorPeople(AWallhackSensorPeopleActor* People)
{
    SensorPeople = People;
    if (People) AddTickPrerequisiteActor(People);
}
void AWallhackVRHUDActor::ConfirmSensorPlacement() { if (SensorPeople) SensorPeople->ConfirmPlacement(); }
void AWallhackVRHUDActor::ResetSensorPlacement() { if (SensorPeople) SensorPeople->ResetPlacement(); }

void AWallhackVRHUDActor::DrawSensorPeopleHUD(UCanvas* Canvas)
{
    if (HUDDensity == EWallhackHUDDensity::Hidden || !SensorPeople) return;
    const FLinearColor White(.85f,.92f,.9f,1), Green(.25f,.9f,.35f,1), Amber(1,.62f,.08f,1), Blue(.35f,.7f,1,1);
    auto Text = [&](const FString& Value, float X, float Y, float Size, FLinearColor Color, bool Center=false)
    { WallhackCanvasLabels::Draw(Canvas, SpatialLabelAtlas, Value, X, Y, Size, Color, Center); };
    Text(SensorPeople->IsReplay() ? TEXT("RECORDED REPLAY / SENSOR PEOPLE") : TEXT("SENSOR PEOPLE"), 1024, 245, 26, SensorPeople->IsReplay() ? Amber : White, true);
    Text(SensorPeople->GetStatus(), 1024, 293, 20, White, true);
    Text(TEXT("TRIGGER / MARK   A / REPLACE SENSOR   B / VISIBILITY"), 1024, 332, 16, White, true);
    if (HUDDensity == EWallhackHUDDensity::Minimal) return;
    const FVector2D Center(540, 775); const float Radius = 155;
    auto Line = [&](FVector2D A, FVector2D B, FLinearColor Color) { Canvas->K2_DrawLine(A, B, 2, Color); };
    FVector2D Previous = Center + FVector2D(Radius, 0);
    for (int32 I=1; I<=48; ++I)
    {
        const float Angle = I * 2 * UE_PI / 48;
        const FVector2D Next = Center + FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)) * Radius;
        Line(Previous, Next, White.CopyWithNewOpacity(.5f)); Previous = Next;
    }
    Line(Center + FVector2D(-8,10), Center + FVector2D(0,-8), Green);
    Line(Center + FVector2D(0,-8), Center + FVector2D(8,10), Green);
    Text(TEXT("HEADSET / 10 M"), 540, 950, 18, White, true);
    auto Dot = [&](const WallhackSpatialMath::FContactView& View, FLinearColor Color, float Size)
    {
        const FVector2D P = Center + WallhackSpatialMath::MapOffset(View, Radius, 10);
        Line(P + FVector2D(-Size,0), P + FVector2D(Size,0), Color);
        Line(P + FVector2D(0,-Size), P + FVector2D(0,Size), Color);
    };
    for (const auto& Radar : SensorPeople->GetRadarViews()) Dot(Radar, Blue, 4);
    float Row = 585;
    for (const auto& Person : SensorPeople->GetPeopleViews())
    {
        const auto Color = Person.bRadar ? Green : Amber;
        Dot(Person.View, Color, 7);
        Text(FString::Printf(TEXT("C%d %s / %.1f M / %+.0f DEG"), Person.Id,
            Person.bRadar ? TEXT("RADAR") : TEXT("ESTIMATED"), Person.View.GroundRangeMeters, Person.View.BearingDegrees),
            1160, Row, 18, Color);
        Row += 39;
    }
    Text(FString::Printf(TEXT("%d WITHOUT RANGE"), SensorPeople->GetUnpositionedCount()), 1160, 942, 16, White);
    Text(TEXT("GENERIC HEIGHT / POSTURE / FACING"), 1160, 970, 14, White);
    Text(TEXT("BLUE / UNCLASSIFIED RADAR"), 540, 982, 14, Blue, true);
}
