#include "WallhackVRHUDActor.h"
#include "WallhackNavigationSubsystem.h"
#include "WallhackPeopleSubsystem.h"
#include "InputActionValue.h"
#include "WallhackCanvasLabels.h"
#include "CanvasTypes.h"
#include "Engine/Canvas.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"

void AWallhackVRHUDActor::BeginNavigationAim(){GetWorld()->GetSubsystem<UWallhackNavigationSubsystem>()->BeginAim();}
void AWallhackVRHUDActor::EndNavigationAim(){GetWorld()->GetSubsystem<UWallhackNavigationSubsystem>()->EndAim();}
void AWallhackVRHUDActor::ConfirmNavigation()
{
    auto* People=GetWorld()->GetSubsystem<UWallhackPeopleSubsystem>();
    auto* Nav=GetWorld()->GetSubsystem<UWallhackNavigationSubsystem>();
    const auto& D=Nav->GetDisplaySnapshot();
    if(People->IsEditing() && D.bGuidance)
    {
        if(D.bAiming)People->PlaceFromAim();else People->NavigateToSelected();
    }
    else if(!D.bAiming && D.bGuidance && People->GetSelected()) People->NavigateToSelected();
    else Nav->Confirm(); // Room setup keeps priority, including in person mode.
}
void AWallhackVRHUDActor::CancelNavigation()
{
    auto* People=GetWorld()->GetSubsystem<UWallhackPeopleSubsystem>();
    if(People->IsEditing()) People->RemoveSelected();
    else GetWorld()->GetSubsystem<UWallhackNavigationSubsystem>()->CancelNavigation();
}
void AWallhackVRHUDActor::TogglePeopleEditing(){GetWorld()->GetSubsystem<UWallhackPeopleSubsystem>()->ToggleEditing();}
void AWallhackVRHUDActor::SelectNextPerson(){GetWorld()->GetSubsystem<UWallhackPeopleSubsystem>()->SelectNext();}
void AWallhackVRHUDActor::MoveSelectedPerson(){GetWorld()->GetSubsystem<UWallhackPeopleSubsystem>()->MoveSelectedFromAim();}
namespace
{
float PersonAxis(const FInputActionValue& Value)
{
    const float Axis=Value.Get<float>();
    return FMath::Abs(Axis)<.25f?0.f:FMath::Sign(Axis)*(FMath::Abs(Axis)-.25f)/.75f;
}
}
void AWallhackVRHUDActor::AdjustPersonHeight(const FInputActionValue& Value)
{
    GetWorld()->GetSubsystem<UWallhackPeopleSubsystem>()->AdjustSelected(PersonAxis(Value)*.25f*FMath::Min(GetWorld()->GetDeltaSeconds(),.05f),0);
}
void AWallhackVRHUDActor::AdjustPersonFacing(const FInputActionValue& Value)
{
    GetWorld()->GetSubsystem<UWallhackPeopleSubsystem>()->AdjustSelected(0,PersonAxis(Value)*90.f*FMath::Min(GetWorld()->GetDeltaSeconds(),.05f));
}

void AWallhackVRHUDActor::DrawNavigationHUD(UCanvas* Canvas,UFont* Font)
{
    if(HUDDensity==EWallhackHUDDensity::Hidden)return;
    const auto* Nav=GetWorld()->GetSubsystem<UWallhackNavigationSubsystem>();if(!Nav)return;
    const auto& D=Nav->GetDisplaySnapshot();
    const auto* People=GetWorld()->GetSubsystem<UWallhackPeopleSubsystem>();
    const bool bPeople=People->IsEditing();
    const FLinearColor Ink(.82,.88,.86,.92),Soft(.56,.65,.62,.82),Mint(.32,.7,.55,.8),Amber(.85,.59,.23,.9);
    auto Label=[&](const FString& Text,float X,float Y,float Height,FLinearColor Color,bool Center=false)
    {
        if(!Font){WallhackCanvasLabels::Draw(Canvas,SpatialLabelAtlas,Text,X,Y,Height,Color,Center);return;}
        float W=0,H=0;Canvas->StrLen(Font,Text,W,H);
        const bool Alpha=Canvas->Canvas->IsWriteDestinationAlphaSet();Canvas->Canvas->SetWriteDestinationAlpha(true);
        Canvas->K2_DrawText(Font,Text,{X,Y},FVector2D(Height/FMath::Max(H,1.f)),Color,0,
            FLinearColor(0,0,0,Color.A),{1,1},Center,false,true,FLinearColor(0,0,0,Color.A));
        Canvas->Canvas->SetWriteDestinationAlpha(Alpha);
    };
    if(!bHasAutoCalibratedNorth&&D.bGuidance){NorthOffsetDegrees=-D.Orientation.Rotator().Yaw;bHasAutoCalibratedNorth=true;}
    DisplayedHeading=FMath::Fmod(D.Orientation.Rotator().Yaw+NorthOffsetDegrees+360,360);
    Label(D.bGuidance?FString::Printf(TEXT("%03.0f / REL"),DisplayedHeading):TEXT("--- / REL"),1024,145,24,D.bGuidance?Ink:Amber,true);
    Label(D.Tracking,300,872,20,D.bGuidance&&D.bDepthAvailable?Soft:Amber);
    Label(D.ObservationAge>=0?FString::Printf(TEXT("GEOMETRY AGE %.1f s"),D.ObservationAge):TEXT("GEOMETRY / WAITING"),300,904,18,D.ObservationAge>2?Amber:Soft);
    const FString WalkText=D.Route.Points.Num()<2&&!D.Route.bComplete?FString(TEXT("WALK / NO ROUTE YET")):FString::Printf(TEXT("%sWALK %.1f m"),D.Route.bComplete?TEXT(""):TEXT("EST. "),D.Walking);
    const bool Estimate=D.State==WallhackNav::ERouteState::Estimated||D.State==WallhackNav::ERouteState::Incomplete||D.State==WallhackNav::ERouteState::Blocked||D.State==WallhackNav::ERouteState::Relocalizing||D.State==WallhackNav::ERouteState::StartBlocked;
    if(bPeople)
    {
        if(D.bHasTarget&&D.Target.PersonId!=INDEX_NONE)
        {
            Label(FString::Printf(TEXT("TO PERSON %02d / %s"),D.Target.PersonId,*WalkText),1280,673,20,D.Route.bComplete?Soft:Amber);
            Label(WallhackNav::StateLabel(D.State),1280,706,18,Estimate?Amber:Soft);
        }
        const auto* Selected=People->GetSelected();
        Label(Selected?FString::Printf(TEXT("PERSON %02d / MANUAL"),Selected->Id):TEXT("PLACE PERSON / MANUAL"),1280,748,21,Ink);
        Label(FString::Printf(TEXT("HEIGHT %.2f m / FACING %03.0f"),People->GetPlacementHeight(),
            FRotator::ClampAxis(People->GetPlacementFacing()+NorthOffsetDegrees)),1280,782,21,Soft);
        Label(People->GetHint(),1280,819,18,D.bGuidance?Soft:Amber);
        Label(TEXT("RIGHT STICK / HEIGHT + FACING"),1280,852,17,Soft);
        Label(TEXT("STICK CLICK / NEXT   A / REMOVE"),1280,882,17,Soft);
        Label(TEXT("LEFT TRIGGER / MOVE   Y / DONE"),1280,912,17,Soft);
    }
    if(D.bAiming&&!bPeople)Label(Nav->GetInteractionHint(),1280,730,20,D.bPreviewValid?Soft:Amber);
    if(D.bHasTarget&&!bPeople)
    {
        Label(D.Target.PersonId!=INDEX_NONE?FString::Printf(TEXT("PERSON %02d / MANUAL"),D.Target.PersonId):TEXT("TARGET"),1280,780,22,Ink);
        Label(FString::Printf(TEXT("%.1f m  /  %+.0f deg  /  %+.1f m UP"),D.Range,D.Bearing,D.Height),1280,816,22,Ink);
        Label(WalkText,1280,856,21,D.Route.bComplete?Soft:Amber);
        Label(WallhackNav::StateLabel(D.State),1280,890,19,Estimate?Amber:Soft);
        if(D.NextTurn!=TEXT("--"))Label(TEXT("NEXT ")+D.NextTurn,1280,922,19,Soft);
    }
    else if(!D.bAiming&&!bPeople)Label(D.bGuidance&&People->GetSelected()?TEXT("TRIGGER / NAVIGATE TO PERSON"):Nav->GetInteractionHint(),1280,840,20,D.bGuidance?Soft:Amber);
    if(!bPeople)Label(People->GetSelected()?TEXT("STICK CLICK / NEXT PERSON   Y / EDIT"):TEXT("Y / PERSON SILHOUETTES"),1280,959,17,Soft);
    if(HUDDensity!=EWallhackHUDDensity::Full)return;
    // Fit the COMPLETE route, including the destination, into the peripheral map.
    const FVector2D Center(460,635);const float Radius=155;
    float Range=3;
    for(const auto& P:D.Route.Points)Range=FMath::Max(Range,float(FVector::Dist2D(D.Viewer,P.Position)));
    if(D.bHasTarget)Range=FMath::Max(Range,float(FVector::Dist2D(D.Viewer,D.Target.Standing)));
    Range*=1.1f;
    auto Project=[&](FVector P){const FVector Local=D.Orientation.Inverse().RotateVector(P-D.Viewer);return Center+FVector2D(Local.Y,-Local.X)*Radius/Range;};
    auto Line=[&](FVector2D A,FVector2D B,FLinearColor Color,float Width=2.f){Canvas->K2_DrawLine(A,B,Width,Color);};
    Label(FString::Printf(TEXT("ROUTE MAP / %.0f m"),Range),305,435,20,Soft);
    for(const auto& F:Nav->GetScene().Floors)for(int32 I=0;I<F.Polygon.Num();++I)
    {
        const auto A=F.Polygon[I],B=F.Polygon[(I+1)%F.Polygon.Num()];
        const auto PA=Project(FVector(A,F.Z)),PB=Project(FVector(B,F.Z));
        if(FVector2D::Distance(PA,Center)<Radius&&FVector2D::Distance(PB,Center)<Radius)Line(PA,PB,FLinearColor(.4,.5,.45,.25),1);
    }
    if(D.bGuidance)for(int32 I=1;I<D.Route.Points.Num();++I)
    {
        const bool Est=D.Route.Points[I-1].bEstimated||D.Route.Points[I].bEstimated;
        Line(Project(D.Route.Points[I-1].Position),Project(D.Route.Points[I].Position),Est?Amber:Mint,Est?2:3);
    }
    Line(Center+FVector2D(-5,5),Center+FVector2D(0,-6),Ink);Line(Center+FVector2D(0,-6),Center+FVector2D(5,5),Ink);
    if(D.bHasTarget&&D.bGuidance)
    {const auto P=Project(D.Target.Standing);Line(P-FVector2D(4,0),P+FVector2D(4,0),Ink);Line(P-FVector2D(0,4),P+FVector2D(0,4),Ink);}
    Label(FString::Printf(TEXT("OBSERVED %.1f m / ESTIMATED %.1f m"),D.Route.ObservedMeters,D.Route.EstimatedMeters),300,807,18,D.Route.EstimatedMeters>0?Amber:Soft);
    const int32 Battery=D.Battery;
    Label(FString::Printf(TEXT("%.0f FPS / BATTERY %s"),D.FPS,Battery>=0?*FString::Printf(TEXT("%d%%"),Battery):TEXT("--")),1280,442,18,Soft);
    Label(FString::Printf(TEXT("MAP %.2f ms / QUERY %.2f ms"),D.MappingMs,D.QueryMs),1280,474,18,Soft);
    Label(FString::Printf(TEXT("PLAN %.2f ms / %d CELLS"),D.Route.PlannerMs,D.Route.Expanded),1280,506,18,Soft);
    Label(TEXT("ASSISTED NAVIGATION / SAME FLOOR"),1280,548,17,Soft);
    Label(TEXT("UNKNOWN CONNECTIONS ARE ESTIMATES"),1280,575,17,Amber);
}
