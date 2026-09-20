#pragma once

#include "CoreMinimal.h"
#include "WallhackNavigationTypes.h"
#include "WallhackPeopleSubsystem.h"

namespace WallhackNavPresentation
{
// Shortest signed angle keeps the tape continuous across the 359 / 000 seam.
inline float CompassDelta(float Bearing,float Yaw,float NorthOffset)
{
    return FMath::FindDeltaAngleDegrees(FRotator::ClampAxis(Yaw+NorthOffset),Bearing);
}
inline int32 HeadingNumber(float Yaw,float NorthOffset)
{
    return FMath::RoundToInt(FRotator::ClampAxis(Yaw+NorthOffset))%360;
}
struct FCompassPerson
{
    int32 PersonIndex=INDEX_NONE,Id=INDEX_NONE;
    float Delta=0,AnchorX=0,LabelX=0;
    bool bOutside=false;
};
inline TArray<FCompassPerson> CompassPeople(const TArray<FWallhackPersonPose>& People,FVector Viewer,float Yaw)
{
    TArray<FCompassPerson> Pins;
    if(Viewer.ContainsNaN()||!FMath::IsFinite(Yaw))return Pins;
    for(int32 I=0;I<People.Num();++I)
    {
        const FVector Delta=People[I].Feet-Viewer;
        if(Delta.ContainsNaN()||Delta.SizeSquared2D()<.0001)continue;
        // Calibration changes north labels, never a person's physical bearing.
        const float Bearing=FMath::RadiansToDegrees(FMath::Atan2(Delta.Y,Delta.X));
        const float Angle=CompassDelta(Bearing,Yaw,0);
        const float Anchor=1024+FMath::Clamp(Angle,-70.f,70.f)*6;
        Pins.Add({I,People[I].Id,Angle,Anchor,FMath::Clamp(Anchor,624.f,1424.f),FMath::Abs(Angle)>70});
    }
    Pins.Sort([](const FCompassPerson& A,const FCompassPerson& B){return A.AnchorX==B.AnchorX?A.Id<B.Id:A.AnchorX<B.AnchorX;});
    // Up to eight IDs fit in one row. Thin leaders retain exact bearings when
    // near-coincident contacts need spacing, including contacts at the rim.
    for(int32 I=1;I<Pins.Num();++I)Pins[I].LabelX=FMath::Max(Pins[I].LabelX,Pins[I-1].LabelX+40);
    if(!Pins.IsEmpty())Pins.Last().LabelX=FMath::Min(Pins.Last().LabelX,1424.f);
    for(int32 I=Pins.Num()-2;I>=0;--I)Pins[I].LabelX=FMath::Min(Pins[I].LabelX,Pins[I+1].LabelX-40);
    return Pins;
}
// Project onto a segment, not its nearest endpoint. Sparse/short routes must
// retain their remaining arrows when the viewer passes a segment's midpoint.
struct FRouteCursor
{
    int32 Segment=INDEX_NONE;
    FVector Position=FVector::ZeroVector;
    float DistanceSquared=FLT_MAX,Remaining=0;
};
inline FRouteCursor ProjectRoute(const WallhackNav::FRoute& Route,FVector Viewer)
{
    FRouteCursor Cursor;
    for(int32 I=0;I+1<Route.Points.Num();++I)
    {
        if(Route.bAttachedToViewer&&I>0)break;
        const FVector A=Route.Points[I].Position,B=Route.Points[I+1].Position;
        const FVector Delta=B-A;
        if(Delta.SizeSquared2D()<UE_SMALL_NUMBER)continue;
        const double T=FMath::Clamp(((Viewer.X-A.X)*Delta.X+(Viewer.Y-A.Y)*Delta.Y)/Delta.SizeSquared2D(),0.,1.);
        const FVector P=FMath::Lerp(A,B,T);
        const float Distance=FVector::DistSquared2D(Viewer,P);
        if(Distance<Cursor.DistanceSquared){Cursor.Segment=I;Cursor.Position=P;Cursor.DistanceSquared=Distance;}
    }
    if(Cursor.Segment!=INDEX_NONE)
    {
        Cursor.Remaining=FVector::Dist(Cursor.Position,Route.Points[Cursor.Segment+1].Position);
        for(int32 I=Cursor.Segment+2;I<Route.Points.Num();++I)
            Cursor.Remaining+=FVector::Dist(Route.Points[I-1].Position,Route.Points[I].Position);
    }
    return Cursor;
}
inline WallhackNav::FRoutePoint SampleRouteAhead(const WallhackNav::FRoute& Route,const FRouteCursor& Cursor,float Distance)
{
    WallhackNav::FRoutePoint Out{Cursor.Position,false};
    if(Cursor.Segment==INDEX_NONE)return Out;
    FVector Start=Cursor.Position;
    for(int32 I=Cursor.Segment+1;I<Route.Points.Num();++I)
    {
        const auto& End=Route.Points[I];
        Out.bEstimated|=Route.Points[I-1].bEstimated||End.bEstimated;
        const float Length=FVector::Dist(Start,End.Position);
        if(Length>UE_SMALL_NUMBER&&Distance<=Length)
        {Out.Position=FMath::Lerp(Start,End.Position,FMath::Clamp(Distance/Length,0.f,1.f));return Out;}
        Distance-=Length;Start=End.Position;Out.Position=Start;
    }
    return Out;
}
enum class EDirectionCue : uint8 { Hidden,Route,TargetDirection,Arrived };
struct FDirectionCue
{
    EDirectionCue Kind=EDirectionCue::Hidden;
    FVector2D Direction=FVector2D(0,-1);
    bool bEstimated=false,bPartial=false;
};
inline FDirectionCue BuildDirectionCue(const WallhackNav::FDisplaySnapshot& D)
{
    FDirectionCue Cue;
    if(!D.bGuidance||D.bHidden||!D.bHasTarget)return Cue;
    if(D.State==WallhackNav::ERouteState::Arrived){Cue.Kind=EDirectionCue::Arrived;return Cue;}
    FVector Aim=D.Target.Standing;
    const auto Cursor=ProjectRoute(D.Route,D.Viewer);
    Cue.Kind=EDirectionCue::TargetDirection;
    if(Cursor.Segment!=INDEX_NONE&&Cursor.Remaining>.08f&&Cursor.DistanceSquared<=.25f)
    {
        const auto Ahead=SampleRouteAhead(D.Route,Cursor,.9f);
        Aim=Ahead.Position;Cue.Kind=EDirectionCue::Route;Cue.bEstimated=Ahead.bEstimated;Cue.bPartial=!D.Route.bComplete;
    }
    const FVector Delta=Aim-D.Viewer;
    const float Bearing=FMath::Atan2(Delta.Y,Delta.X)-FMath::DegreesToRadians(D.Orientation.Rotator().Yaw);
    Cue.Direction={FMath::Sin(Bearing),-FMath::Cos(Bearing)};
    return Cue;
}

// A heading-up map uses yaw only. Pitch/roll must never tip or shrink the floor plan.
struct FLocalMap
{
    FVector Viewer = FVector::ZeroVector;
    float Yaw = 0;
    float Range = 5;
    FVector2D Center = FVector2D(450,820);
    float Radius = 154;
    FVector2D Project(FVector World) const
    {
        const FVector Local = FRotator(0,Yaw,0).UnrotateVector(World-Viewer);
        return Center + FVector2D(Local.Y,-Local.X) * Radius / FMath::Max(Range,.1f);
    }
    FVector2D Compass(float Bearing, float NorthOffset, float AtRadius) const
    {
        const float Angle = FMath::DegreesToRadians(Bearing-NorthOffset-Yaw);
        return Center+FVector2D(FMath::Sin(Angle),-FMath::Cos(Angle))*AtRadius;
    }
};

// Clip the whole segment, including crossings whose TWO endpoints are outside.
// This prevents floor/wall lines and long routes from escaping the circular map.
inline bool ClipToCircle(FVector2D& A,FVector2D& B,FVector2D Center,float Radius)
{
    if(A.ContainsNaN()||B.ContainsNaN()||!FMath::IsFinite(Radius)||Radius<=0)return false;
    const FVector2D D=B-A,P=A-Center;
    const double Length=D.SizeSquared();
    if(Length<UE_SMALL_NUMBER)return P.SizeSquared()<=Radius*Radius;
    const double Dot=FVector2D::DotProduct(P,D);
    const double Disc=Dot*Dot-Length*(P.SizeSquared()-Radius*Radius);
    if(Disc<0)return false;
    const double Root=FMath::Sqrt(Disc);
    const double T0=FMath::Max(0.,(-Dot-Root)/Length),T1=FMath::Min(1.,(-Dot+Root)/Length);
    if(T0>T1)return false;
    B=A+D*T1;A+=D*T0;return true;
}

struct FMapMarker
{
    FVector2D Position=FVector2D::ZeroVector,Direction=FVector2D::ZeroVector;
    bool bOutside=false;
};
inline FMapMarker ProjectMarker(const FLocalMap& Map,FVector World)
{
    FMapMarker Marker;
    const FVector2D Delta=Map.Project(World)-Map.Center;
    Marker.Direction=Delta.GetSafeNormal();
    Marker.bOutside=Delta.Size()>Map.Radius;
    // Reserve enough room for the entire dot/arrow and its selected ring.
    Marker.Position=Map.Center+Marker.Direction*FMath::Min(Delta.Size(),double(Map.Radius-12));
    return Marker;
}
}
