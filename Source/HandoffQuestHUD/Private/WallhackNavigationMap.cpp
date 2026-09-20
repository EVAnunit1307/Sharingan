#include "WallhackVRHUDActor.h"
#include "WallhackNavigationSubsystem.h"
#include "WallhackPeopleSubsystem.h"
#include "WallhackPeopleStyle.h"
#include "WallhackNavigationPresentation.h"
#include "WallhackCanvasLabels.h"
#include "Engine/World.h"

namespace WallhackMapDrawing
{
struct FCanvasTools
{
    UCanvas* Canvas;
    UFont* Font;
    UTexture2D* Atlas;
    void Text(const FString& Value,FVector2D P,float Height,FLinearColor Color,bool Center=false) const
    {
        if(!Font){WallhackCanvasLabels::Draw(Canvas,Atlas,Value,P.X,P.Y,Height,Color,Center);return;}
        float W=0,H=0;Canvas->StrLen(Font,Value,W,H);
        const bool Alpha=Canvas->Canvas->IsWriteDestinationAlphaSet();Canvas->Canvas->SetWriteDestinationAlpha(true);
        Canvas->K2_DrawText(Font,Value,P,FVector2D(Height/FMath::Max(H,1.f)),Color,0,
            FLinearColor(0,0,0,Color.A),{1,1},Center,false,true,FLinearColor(0,0,0,Color.A));
        Canvas->Canvas->SetWriteDestinationAlpha(Alpha);
    }
    void Line(FVector2D A,FVector2D B,FLinearColor C,float Width=1.5f) const
    {
        Canvas->K2_DrawLine(A,B,Width+1.2f,FLinearColor(0,0,0,C.A*.5f));
        Canvas->K2_DrawLine(A,B,Width,C);
    }
    void Triangle(FVector2D A,FVector2D B,FVector2D C,FLinearColor CA,FLinearColor CB,FLinearColor CC) const
    {
        if(!Atlas||!Atlas->GetResource())return;
        FCanvasUVTri T;T.V0_Pos=A;T.V1_Pos=B;T.V2_Pos=C;
        // A known white texel in the shared runtime atlas avoids another asset.
        T.V0_UV=T.V1_UV=T.V2_UV=FVector2D(2.5/128.,.5/32.);
        T.V0_Color=CA;T.V1_Color=CB;T.V2_Color=CC;
        FCanvasTriangleItem Item(T,Atlas->GetResource());Item.BlendMode=SE_BLEND_AlphaBlend;
        Canvas->DrawItem(Item);
    }
    void Circle(FVector2D P,float R,FLinearColor C,float Width=1.5f) const
    {
        constexpr int32 Steps=72;
        for(int32 I=0;I<Steps;++I)
        {
            const double A=UE_TWO_PI*I/Steps,B=UE_TWO_PI*(I+1)/Steps;
            Line(P+FVector2D(FMath::Cos(A),FMath::Sin(A))*R,P+FVector2D(FMath::Cos(B),FMath::Sin(B))*R,C,Width);
        }
    }
    void Disc(FVector2D P,float R,FLinearColor C,int32 Steps=24) const
    {
        for(int32 I=0;I<Steps;++I)
        {
            const double A=UE_TWO_PI*I/Steps,B=UE_TWO_PI*(I+1)/Steps;
            Triangle(P,P+FVector2D(FMath::Cos(A),FMath::Sin(A))*R,P+FVector2D(FMath::Cos(B),FMath::Sin(B))*R,C,C,C);
        }
    }
    void Arrow(FVector2D Tip,FVector2D Direction,FLinearColor C,float Size=9) const
    {
        const FVector2D Side(-Direction.Y,Direction.X);
        Line(Tip-Direction*Size+Side*Size*.55,Tip,C,2);
        Line(Tip-Direction*Size-Side*Size*.55,Tip,C,2);
    }
};
}

void AWallhackVRHUDActor::DrawNavigationCompass(UCanvas* Canvas,UFont* Font)
{
    using namespace WallhackNavPresentation;
    const auto& D=GetWorld()->GetSubsystem<UWallhackNavigationSubsystem>()->GetDisplaySnapshot();
    if(D.bHidden||HUDDensity==EWallhackHUDDensity::Hidden)return;
    const WallhackMapDrawing::FCanvasTools Draw{Canvas,Font,SpatialLabelAtlas};
    const FLinearColor Ink(.92,.94,.95,.92),Soft(.67,.71,.74,.75);
    Draw.Text(TEXT("REL"),{1024,94},13,Soft,true);
    Draw.Text(D.bGuidance?FString::Printf(TEXT("%03d"),HeadingNumber(D.Orientation.Rotator().Yaw,NorthOffsetDegrees)):TEXT("---"),
        {1024,116},30,D.bGuidance?Ink:FLinearColor(.85,.59,.23,.9),true);
    if(!D.bGuidance)return;
    // Fixed centre pointer with a yaw-driven tape, like a game compass.
    Draw.Arrow({1024,169},{0,1},Ink,8);
    static const TCHAR* Directions[]={TEXT("N"),TEXT("NE"),TEXT("E"),TEXT("SE"),TEXT("S"),TEXT("SW"),TEXT("W"),TEXT("NW")};
    for(int32 Bearing=0;Bearing<360;Bearing+=5)
    {
        const float Delta=CompassDelta(Bearing,D.Orientation.Rotator().Yaw,NorthOffsetDegrees);
        if(FMath::Abs(Delta)>70)continue;
        const float X=1024+Delta*6;
        const float Fade=FMath::Clamp((70-FMath::Abs(Delta))/14.f,0.f,1.f);
        const bool Cardinal=Bearing%45==0,Major=Bearing%15==0;
        FLinearColor Color=Cardinal?Ink:Soft;Color.A*=Fade;
        Draw.Line({X,190.-(Cardinal?17:Major?12:6)},{X,190},Color,Cardinal?2:1);
        if(Cardinal)Draw.Text(Directions[Bearing/45],{X,204},25,Color,true);
        else if(Major)Draw.Text(FString::FromInt(Bearing),{X,208},17,Color,true);
    }
    const auto* People=GetWorld()->GetSubsystem<UWallhackPeopleSubsystem>();
    const auto Pins=CompassPeople(People->GetPeople(),D.Viewer,D.Orientation.Rotator().Yaw);
    for(const auto& Pin:Pins)
    {
        const auto& Person=People->GetPeople()[Pin.PersonIndex];
        FLinearColor Color=WallhackPeopleStyle::Color(Person);Color.A=.95f;
        const bool Selected=D.bHasTarget&&D.Target.PersonId==Person.Id;
        Draw.Line({Pin.AnchorX,235},{Pin.LabelX,244},FLinearColor(Color.R,Color.G,Color.B,.30),1);
        const FVector2D P(Pin.LabelX,251);
        if(Pin.bOutside)Draw.Arrow(P,{Pin.Delta<0?-1.:1.,0},Color,8);
        else
        {
            Draw.Triangle(P+FVector2D(0,-5),P+FVector2D(5,0),P+FVector2D(0,5),Color,Color,Color);
            Draw.Triangle(P+FVector2D(0,-5),P+FVector2D(0,5),P+FVector2D(-5,0),Color,Color,Color);
        }
        if(Selected)Draw.Circle(P,9,Ink,1);
        Draw.Text(FString::Printf(TEXT("%02d"),Person.Id),{Pin.LabelX,267},16,Color,true);
    }

}

void AWallhackVRHUDActor::DrawNavigationMap(UCanvas* Canvas,UFont* Font)
{
    using namespace WallhackNavPresentation;
    const auto* Nav=GetWorld()->GetSubsystem<UWallhackNavigationSubsystem>();
    const auto* People=GetWorld()->GetSubsystem<UWallhackPeopleSubsystem>();
    const auto& D=Nav->GetDisplaySnapshot();
    if(D.bHidden||HUDDensity==EWallhackHUDDensity::Hidden)return;
    const WallhackMapDrawing::FCanvasTools Draw{Canvas,Font,SpatialLabelAtlas};
    const FLinearColor White(.93,.95,.97,.88),Quiet(.70,.74,.78,.48),Red(.96,.07,.055,.95),Amber(.85,.59,.23,.85);
    FLocalMap Map;Map.Viewer=D.Viewer;Map.Yaw=D.Orientation.Rotator().Yaw;Map.Range=SpatialMapRangeMeters;
    if(HUDDensity==EWallhackHUDDensity::Full&&D.bGuidance)
    {
        // Full retains the complete route overview, including distant targets.
        for(const auto& P:D.Route.Points)Map.Range=FMath::Max(Map.Range,float(FVector::Dist2D(D.Viewer,P.Position)*1.1));
        if(D.bHasTarget)Map.Range=FMath::Max(Map.Range,float(FVector::Dist2D(D.Viewer,D.Target.Standing)*1.1));
    }
    const FVector2D Center=Map.Center;
    Draw.Disc(Center,Map.Radius+10,FLinearColor(.015,.02,.025,.16),72);
    Draw.Circle(Center,Map.Radius+10,White,1.8f);
    Draw.Circle(Center,Map.Radius,FLinearColor(.75,.8,.85,.18),1);
    Draw.Text(FString::Printf(TEXT("%s / %.0f M"),HUDDensity==EWallhackHUDDensity::Full?TEXT("ROUTE"):TEXT("LOCAL"),Map.Range),
        {Center.X,Center.Y-Map.Radius-58},18,White,true);
    if(!D.bGuidance)
    {
        Draw.Text(TEXT("LOCALIZING"),Center-FVector2D(0,8),20,Amber,true);
        return; // No stale geometry, north labels, contacts or directional cues.
    }
    auto MapLine=[&](FVector A,FVector B,FLinearColor C,float Width=1.f)
    {
        FVector2D PA=Map.Project(A),PB=Map.Project(B);
        if(ClipToCircle(PA,PB,Center,Map.Radius-1))Draw.Line(PA,PB,C,Width);
    };
    // A subtle view sector is a heading cue, not a visibility/occlusion claim.
    constexpr float HalfCone=36.f;
    for(int32 I=0;I<18;++I)
    {
        const float A=FMath::DegreesToRadians(-HalfCone+I*HalfCone/9),B=FMath::DegreesToRadians(-HalfCone+(I+1)*HalfCone/9);
        const auto PA=Center+FVector2D(FMath::Sin(A),-FMath::Cos(A))*Map.Radius;
        const auto PB=Center+FVector2D(FMath::Sin(B),-FMath::Cos(B))*Map.Radius;
        Draw.Triangle(Center,PA,PB,FLinearColor(1,1,1,.20),FLinearColor(1,1,1,.015),FLinearColor(1,1,1,.015));
    }
    Draw.Circle(Center,Map.Radius*.5f,FLinearColor(.7,.75,.8,.12),.8f);
    for(const auto& Floor:Nav->GetScene().Floors)
        for(int32 I=0;I<Floor.Polygon.Num();++I)
            MapLine(FVector(Floor.Polygon[I],Floor.Z),FVector(Floor.Polygon[(I+1)%Floor.Polygon.Num()],Floor.Z),FLinearColor(.9,.92,.95,.50),1.4f);
    for(const auto& Obstacle:Nav->GetScene().Obstacles)
    {
        const FBox Box=Obstacle.LocalBox.IsValid?Obstacle.LocalBox:Obstacle.Box;
        if(!Box.IsValid)continue;
        FVector Corners[4]={{Box.Min.X,Box.Min.Y,Box.Min.Z},{Box.Max.X,Box.Min.Y,Box.Min.Z},
            {Box.Max.X,Box.Max.Y,Box.Min.Z},{Box.Min.X,Box.Max.Y,Box.Min.Z}};
        if(Obstacle.LocalBox.IsValid)for(auto& P:Corners)P=Obstacle.LocalToWorld.TransformPosition(P);
        for(int32 I=0;I<4;++I)MapLine(Corners[I],Corners[(I+1)%4],FLinearColor(.84,.88,.92,Obstacle.bWall?.65:.28),Obstacle.bWall?1.8f:1.f);
    }
    for(int32 I=1;I<D.Route.Points.Num();++I)
    {
        const auto& A=D.Route.Points[I-1];const auto& B=D.Route.Points[I];
        MapLine(A.Position,B.Position,A.bEstimated||B.bEstimated?Amber:FLinearColor(.92,.95,.97,.92),3.5f);
    }
    // Compass ticks and labels rotate with the map; viewer remains heading-up.
    for(int32 Bearing=0;Bearing<360;Bearing+=15)
    {
        const float Outer=Map.Radius+10,Inner=Outer-(Bearing%90==0?9:4);
        Draw.Line(Map.Compass(Bearing,NorthOffsetDegrees,Inner),Map.Compass(Bearing,NorthOffsetDegrees,Outer),Quiet,1);
    }
    const TCHAR* Directions[]={TEXT("N"),TEXT("E"),TEXT("S"),TEXT("W")};
    for(int32 I=0;I<4;++I)Draw.Text(Directions[I],Map.Compass(I*90,NorthOffsetDegrees,Map.Radius+28)-FVector2D(0,10),20,I==0?White:Quiet,true);
    TArray<FVector2D> UsedLabels;
    auto Marker=[&](FVector World,const FString& Id,bool Selected,FLinearColor Color)
    {
        const FMapMarker Pin=ProjectMarker(Map,World);
        if(Pin.bOutside)Draw.Arrow(Pin.Position+Pin.Direction*5,Pin.Direction,Color,11);
        else
        {
            Draw.Disc(Pin.Position,Selected?5.5f:4.f,Color,14);
            if(Selected)Draw.Circle(Pin.Position,9,White,1);
        }
        FVector2D Label=Pin.Position+FVector2D(Pin.Position.X>Center.X? -15:15,-20);
        for(const auto& Used:UsedLabels)if(FVector2D::Distance(Label,Used)<28)Label.Y+=22;
        const FVector2D LabelDelta=Label-Center;
        if(LabelDelta.Size()>Map.Radius-28)Label=Center+LabelDelta.GetSafeNormal()*(Map.Radius-28);
        UsedLabels.Add(Label);Draw.Text(Id,Label,16,Selected?White:Color,true);
    };
    for(const auto& P:People->GetPeople())Marker(P.Feet,FString::Printf(TEXT("%02d"),P.Id),D.bHasTarget&&D.Target.PersonId==P.Id,WallhackPeopleStyle::Color(P));
    if(D.bHasTarget&&D.Target.PersonId==INDEX_NONE)Marker(D.Target.Standing,TEXT("T"),true,Red);
    // Fixed self chevron sits above the map details. Its nose matches the cone.
    Draw.Disc(Center,12,FLinearColor(.015,.02,.025,.8),20);
    Draw.Triangle(Center+FVector2D(0,-9),Center+FVector2D(-6,6),Center+FVector2D(6,6),White,White,White);

}

void AWallhackVRHUDActor::DrawNavigationDirection(UCanvas* Canvas,UFont* Font)
{
    using namespace WallhackNavPresentation;
    if(HUDDensity==EWallhackHUDDensity::Hidden)return;
    const auto& D=GetWorld()->GetSubsystem<UWallhackNavigationSubsystem>()->GetDisplaySnapshot();
    const auto Cue=BuildDirectionCue(D);
    if(Cue.Kind==EDirectionCue::Hidden)return;
    const WallhackMapDrawing::FCanvasTools Draw{Canvas,Font,SpatialLabelAtlas};
    const bool Caution=Cue.Kind==EDirectionCue::TargetDirection||Cue.bEstimated||Cue.bPartial;
    const FLinearColor Ink=Caution?FLinearColor(.95,.64,.22,.96):FLinearColor(.64,1,.83,.98);
    const FVector2D Center(1024,920);
    Draw.Disc(Center,37,FLinearColor(.008,.012,.014,.58),32);
    Draw.Circle(Center,37,FLinearColor(Ink.R,Ink.G,Ink.B,.4),1.5f);
    if(Cue.Kind==EDirectionCue::Arrived)
    {
        Draw.Line(Center+FVector2D(-13,0),Center+FVector2D(-3,11),Ink,4);
        Draw.Line(Center+FVector2D(-3,11),Center+FVector2D(16,-12),Ink,4);
    }
    else
    {
        const FVector2D Tip=Center+Cue.Direction*20,Side(-Cue.Direction.Y,Cue.Direction.X);
        Draw.Line(Tip-Cue.Direction*25+Side*14,Tip,Ink,4);
        Draw.Line(Tip-Cue.Direction*25-Side*14,Tip,Ink,4);
        Draw.Line(Center-Cue.Direction*18,Tip-Cue.Direction*4,Ink,3);
    }
    const TCHAR* Instruction=Cue.Kind==EDirectionCue::Arrived?TEXT("ARRIVED"):
        Cue.Kind==EDirectionCue::TargetDirection?TEXT("TARGET DIRECTION"):
        Cue.bPartial?TEXT("PARTIAL ROUTE"):Cue.bEstimated?TEXT("ESTIMATED ROUTE"):TEXT("FOLLOW ARROWS");
    Draw.Text(Instruction,{1024,968},21,Ink,true);
    const FString Destination=D.Target.PersonId!=INDEX_NONE?FString::Printf(TEXT("PERSON %02d"),D.Target.PersonId):TEXT("TARGET");
    const FString Detail=Cue.Kind==EDirectionCue::TargetDirection?TEXT("NO WALKABLE ROUTE YET"):
        Cue.Kind==EDirectionCue::Arrived?Destination:
        FString::Printf(TEXT("%s%.1f M WALK"),D.Route.bComplete?TEXT(""):TEXT("EST. "),D.Walking);
    Draw.Text(Detail,{1024,999},19,Caution?Ink:FLinearColor(.87,.92,.9,.95),true);
}
