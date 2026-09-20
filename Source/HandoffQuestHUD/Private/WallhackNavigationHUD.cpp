#include "WallhackVRHUDActor.h"
#include "WallhackNavigationSubsystem.h"
#include "WallhackPeopleSubsystem.h"
#include "InputActionValue.h"
#include "WallhackCanvasLabels.h"
#include "CanvasTypes.h"
#include "Engine/Canvas.h"
#include "Engine/World.h"
#include "WallhackNavigationPresentation.h"
#include "WallhackPeopleStyle.h"

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
    auto* People=GetWorld()->GetSubsystem<UWallhackPeopleSubsystem>();
    const bool bPeople=People->IsEditing();
    const FLinearColor Ink(.92,.94,.95,.92),Soft(.67,.71,.74,.82),Amber(.85,.59,.23,.9);
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
    People->SetNorthReference(NorthOffsetDegrees);
    DrawNavigationCompass(Canvas,Font);
    // Only actionable exceptions stay on the normal HUD; healthy tracking,
    // sensing ages, timings and setup/debug instructions stay off the visor.
    if(!D.bGuidance)Label(Nav->GetInteractionHint(),450,1040,16,Amber,true);
    else if(!D.bDepthAvailable)Label(TEXT("LIVE CHECKS UNAVAILABLE"),450,1040,16,Amber,true);

    constexpr float PanelX=1392;
    const auto* Selected=People->GetSelected();
    const auto* TargetPerson=People->GetPeople().FindByPredicate([&](const FWallhackPersonPose& P){return P.Id==D.Target.PersonId;});
    const auto* AccentPerson=bPeople?Selected:TargetPerson;
    FLinearColor Accent=AccentPerson?WallhackPeopleStyle::Color(*AccentPerson):Ink;Accent.A=.85f;
    if(D.bGuidance&&(bPeople||D.bHasTarget||D.bAiming))
        Canvas->K2_DrawLine({PanelX-16,778},{PanelX-16,889},2,Accent);
    if(bPeople&&D.bGuidance)
    {
        Label(Selected?FString::Printf(TEXT("EDIT PERSON %02d"),Selected->Id):TEXT("PLACE PERSON"),PanelX,775,20,Ink);
        Label(FString::Printf(TEXT("%.2f M HEIGHT"),People->GetPlacementHeight()),PanelX,811,25,Ink);
        Label(FString::Printf(TEXT("%03d FACING"),WallhackNavPresentation::HeadingNumber(People->GetPlacementFacing(),NorthOffsetDegrees)),PanelX,848,19,Soft);
        if(D.bAiming)Label(D.bPreviewValid?TEXT("TRIGGER / PLACE"):TEXT("POINT AT OPEN FLOOR"),PanelX,886,17,D.bPreviewValid?Soft:Amber);
    }
    else if(D.bAiming&&D.bGuidance)
    {
        Label(TEXT("SET TARGET"),PanelX,775,20,Ink);
        Label(D.bPreviewValid?TEXT("TRIGGER / CONFIRM"):TEXT("POINT AT OPEN FLOOR"),PanelX,816,19,D.bPreviewValid?Soft:Amber);
    }
    else if(D.bHasTarget&&D.bGuidance)
    {
        Label(D.Target.PersonId!=INDEX_NONE?FString::Printf(TEXT("PERSON %02d"),D.Target.PersonId):TEXT("TARGET"),PanelX,775,20,Ink);
        Label(FString::Printf(TEXT("%.1f M"),D.Range),PanelX,808,32,Ink);
        Label(FString::Printf(TEXT("RANGE / %+.1f M HEIGHT"),D.Height),PanelX,856,17,Soft);
        if(HUDDensity==EWallhackHUDDensity::Full&&D.Route.EstimatedMeters>0)
            Label(FString::Printf(TEXT("%.1f M OBSERVED / %.1f M EST."),D.Route.ObservedMeters,D.Route.EstimatedMeters),PanelX,917,16,Amber);
    }
    DrawNavigationMap(Canvas,Font);
    DrawNavigationDirection(Canvas,Font);
}
