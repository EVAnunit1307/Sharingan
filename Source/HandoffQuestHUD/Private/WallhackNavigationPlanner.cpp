#include "WallhackNavigationTypes.h"
#include "HAL/PlatformTime.h"
#include <queue>
#include <vector>

namespace WallhackNav
{
const FCell* FMapSnapshot::Find(FIntPoint K) const
{
    const FIntPoint T = TileKey(K);
    const auto* Tile = Tiles.Find(T);
    return Tile ? &(*Tile)->Cells[(K.Y - T.Y * TileSize) * TileSize + K.X - T.X * TileSize] : nullptr;
}
EOccupancy FMapSnapshot::State(FIntPoint K) const
{
    const FCell* C = Find(K);
    if (!C) return EOccupancy::Unknown;
    // Looking away is not evidence that an obstacle disappeared. Keep both
    // supported floor and obstructions until positive observations change them.
    if (C->Occupancy == EOccupancy::Free && (!C->bFloor || !C->bClearance)) return EOccupancy::Unknown;
    return C->Occupancy;
}
bool FMapSnapshot::IsEstimated(FIntPoint K) const
{
    const auto* C=Find(K);
    return State(K)!=EOccupancy::Free || (C&&C->Evidence==EEvidence::Depth&&Now-C->ObservedAt>LiveEvidenceSeconds);
}
EOccupancy FMapSnapshot::Walkability(FIntPoint K, FIntPoint* FirstBlockingCell) const
{
    const auto S=State(K);
    const FCell* C=Find(K);
    if(S==EOccupancy::Occupied || S==EOccupancy::Unsupported || (C&&C->bFloor&&FMath::Abs(C->Floor-Floor)>MaxStep))
    {if(FirstBlockingCell)*FirstBlockingCell=K;return EOccupancy::Occupied;}
    return S;
}
EOccupancy FMapSnapshot::WalkabilityAt(FVector Position, FIntPoint* FirstBlockingCell) const
{
    return Walkability(Key(Position),FirstBlockingCell);
}
bool FMap::Observe(FIntPoint K, const FCell& O)
{
    if (O.Occupancy == EOccupancy::Unknown) return false; // A miss is not evidence.
    const FIntPoint T = TileKey(K);
    auto& Tile = Tiles.FindOrAdd(T);
    if (!Tile) Tile = MakeShared<FTile, ESPMode::ThreadSafe>();
    else if (!Tile.IsUnique()) Tile = MakeShared<FTile, ESPMode::ThreadSafe>(*Tile);
    FCell& C = Tile->Cells[(K.Y - T.Y * TileSize) * TileSize + K.X - T.X * TileSize];
    const FCell Before = C;
    // A scanned wall stays structural until repeated positive floor AND full
    // clearance observations prove an opening. Misses can never erase it.
    const bool Structural = C.Evidence == EEvidence::SceneWall;
    if (Structural && O.Occupancy != EOccupancy::Free && O.Evidence != EEvidence::SceneWall) return false;
    if (O.Occupancy == EOccupancy::Free && (C.Occupancy == EOccupancy::Occupied || C.Occupancy == EOccupancy::Unsupported))
    {
        if (O.Evidence != EEvidence::Depth || !O.bFloor || !O.bClearance || O.ObservedAt - C.ObservedAt < .08) return false;
        C.ObservedAt = O.ObservedAt;
        if (++C.ClearVotes < (Structural ? 5 : 3)) return false;
    }
    C = O;
    // A confirming depth sample does not invalidate the independent room scan
    // when that live sample ages out. A contradictory obstacle/level change
    // still replaces the scan immediately and must be positively cleared.
    if (Before.Evidence == EEvidence::SceneFloor && Before.Occupancy == EOccupancy::Free
        && Before.bFloor && Before.bClearance && O.Evidence == EEvidence::Depth
        && O.Occupancy == EOccupancy::Free && O.bFloor && O.bClearance
        && FMath::Abs(O.Floor - Before.Floor) <= MaxStep)
    {
        C.Evidence = EEvidence::SceneFloor;
        C.Floor = Before.Floor;
    }
    C.ClearVotes = 0;
    const bool Changed = Before.Occupancy != C.Occupancy || Before.Evidence != C.Evidence
        || Before.bFloor != C.bFloor || Before.bClearance != C.bClearance || FMath::Abs(Before.Floor - C.Floor) > .02f
        || (Before.Evidence == EEvidence::Depth && O.ObservedAt - Before.ObservedAt > LiveEvidenceSeconds);
    if (Changed) ++Revision;
    return Changed;
}
TArray<FMapEdge> BuildMapOutline(const FMapSnapshot& Map)
{
    // Integer grid edges merge into long strokes, instead of thousands of
    // canvas lines. Both the minimap and planner use the same fused memory;
    // a positively cleared doorway cannot remain drawn as a scanned wall.
    struct FEdge { int32 Axis,Line,Start,End; bool Blocked; };
    TArray<FEdge> Edges;
    auto Kind=[&](FIntPoint K)
    {
        const auto S=Map.Walkability(K);
        return S==EOccupancy::Occupied?2:S==EOccupancy::Free?1:0;
    };
    for(const auto& Tile:Map.Tiles)for(int32 Y=0;Y<TileSize;++Y)for(int32 X=0;X<TileSize;++X)
    {
        const FIntPoint K=Tile.Key*TileSize+FIntPoint(X,Y);
        const int32 C=Kind(K);if(!C)continue;
        auto Boundary=[&](FIntPoint N){const int32 V=Kind(N);return C==2?V!=2:V==0;};
        if(Boundary(K+FIntPoint(0,-1)))Edges.Add({0,K.Y,K.X,K.X+1,C==2});
        if(Boundary(K+FIntPoint(0,1)))Edges.Add({0,K.Y+1,K.X,K.X+1,C==2});
        if(Boundary(K+FIntPoint(-1,0)))Edges.Add({1,K.X,K.Y,K.Y+1,C==2});
        if(Boundary(K+FIntPoint(1,0)))Edges.Add({1,K.X+1,K.Y,K.Y+1,C==2});
    }
    Edges.Sort([](const FEdge& A,const FEdge& B)
    {
        if(A.Axis!=B.Axis)return A.Axis<B.Axis;
        if(A.Line!=B.Line)return A.Line<B.Line;
        if(A.Blocked!=B.Blocked)return A.Blocked<B.Blocked;
        return A.Start<B.Start;
    });
    TArray<FMapEdge> Out;
    for(int32 I=0;I<Edges.Num();)
    {
        FEdge E=Edges[I++];
        while(I<Edges.Num()&&Edges[I].Axis==E.Axis&&Edges[I].Line==E.Line&&Edges[I].Blocked==E.Blocked&&Edges[I].Start==E.End)
            E.End=Edges[I++].End;
        auto P=[&](int32 T){return E.Axis==0?FVector(T*double(CellSize),E.Line*double(CellSize),Map.Floor)
            :FVector(E.Line*double(CellSize),T*double(CellSize),Map.Floor);};
        Out.Add({P(E.Start),P(E.End),E.Blocked});
    }
    return Out;
}
namespace
{
struct FOpen { FIntPoint Key; float G, F; uint64 Order; };
struct FCompare { bool operator()(const FOpen& A, const FOpen& B) const { return A.F == B.F ? A.Order > B.Order : A.F > B.F; } };
struct FNode { float G = TNumericLimits<float>::Max(); FIntPoint Parent; bool Closed = false; };
float H(FIntPoint A, FIntPoint B) { return (FVector2D(A.X - B.X, A.Y - B.Y)).Size() * CellSize; }
bool SegmentAllowed(const FMapSnapshot& M,FVector A,FVector B,bool Unknown)
{
    // Visit every crossed grid cell. Fixed-distance samples can miss a tiny
    // wall-corner intersection now that routes have no inflated body radius.
    auto Allowed=[&](FIntPoint K)
    {
        const auto S=M.Walkability(K);
        return S!=EOccupancy::Occupied&&(Unknown||S==EOccupancy::Free);
    };
    FIntPoint K=FMapSnapshot::Key(A);const FIntPoint End=FMapSnapshot::Key(B);
    if(!Allowed(K)||!Allowed(End))return false;
    const FVector D=B-A;
    const int32 SX=D.X>0?1:D.X<0?-1:0,SY=D.Y>0?1:D.Y<0?-1:0;
    const double Infinity=TNumericLimits<double>::Max();
    const double DX=SX?double(CellSize)/FMath::Abs(D.X):Infinity;
    const double DY=SY?double(CellSize)/FMath::Abs(D.Y):Infinity;
    double TX=SX?((K.X+(SX>0?1:0))*double(CellSize)-A.X)/D.X:Infinity;
    double TY=SY?((K.Y+(SY>0?1:0))*double(CellSize)-A.Y)/D.Y:Infinity;
    const int32 Limit=FMath::Abs(End.X-K.X)+FMath::Abs(End.Y-K.Y)+2;
    for(int32 I=0;K!=End&&I<Limit;++I)
    {
        if(FMath::Min(TX,TY)>=1.0)return true; // End cell was checked above.
        // Match A*'s no-corner-cutting rule at an exact grid intersection.
        if(FMath::Abs(TX-TY)<1e-9)
        {
            if(!Allowed(K+FIntPoint(SX,0))||!Allowed(K+FIntPoint(0,SY)))return false;
            K+=FIntPoint(SX,SY);TX+=DX;TY+=DY;
        }
        else if(TX<TY){K.X+=SX;TX+=DX;}
        else {K.Y+=SY;TY+=DY;}
        if(!Allowed(K))return false;
    }
    return K==End;
}
FRoute Search(const FMapSnapshot& M, FVector Start, FVector Goal, bool Unknown, int32 Limit)
{
    FRoute Out; Out.Revision = M.Revision;
    const FIntPoint S = FMapSnapshot::Key(Start), G = FMapSnapshot::Key(Goal);
    if (M.WalkabilityAt(Start) == EOccupancy::Occupied) { Out.bStartBlocked = true; return Out; }
    std::priority_queue<FOpen, std::vector<FOpen>, FCompare> Open;
    TMap<FIntPoint, FNode> Nodes;
    TMap<FIntPoint, EOccupancy> WalkCache;
    auto Position = [&](FIntPoint K) { return K==S?FVector(Start.X,Start.Y,M.Floor):K==G?FVector(Goal.X,Goal.Y,M.Floor):FMapSnapshot::Center(K,M.Floor); };
    auto Walk = [&](FIntPoint K) { if (const auto* Found = WalkCache.Find(K)) return *Found; return WalkCache.Add(K, M.WalkabilityAt(Position(K))); };
    Nodes.FindOrAdd(S).G = 0;
    uint64 Order = 0;
    Open.push({S, 0, H(S,G), Order++});
    FIntPoint Best = S;
    float BestH = H(S, G);
    // Finite same-floor search extent; the expansion cap is shared by both passes.
    const int32 MinX = FMath::Min(S.X,G.X)-100, MaxX = FMath::Max(S.X,G.X)+100;
    const int32 MinY = FMath::Min(S.Y,G.Y)-100, MaxY = FMath::Max(S.Y,G.Y)+100;
    while (!Open.empty() && Out.Expanded < Limit)
    {
        const FOpen Current = Open.top(); Open.pop();
        FNode& N = Nodes.FindChecked(Current.Key);
        if (N.Closed || Current.G > N.G) continue;
        N.Closed = true; ++Out.Expanded;
        const float Distance = H(Current.Key, G);
        if (Distance < BestH) { BestH = Distance; Best = Current.Key; }
        if (Current.Key == G) { Best = G; Out.bComplete = true; break; }
        for (int32 Y = -1; Y <= 1; ++Y) for (int32 X = -1; X <= 1; ++X)
        {
            if (!X && !Y) continue;
            const FIntPoint K = Current.Key + FIntPoint(X,Y);
            if (K.X < MinX || K.X > MaxX || K.Y < MinY || K.Y > MaxY) continue;
            const auto State = Walk(K);
            auto Allowed = [Unknown](EOccupancy V) { return V == EOccupancy::Free || (Unknown && V == EOccupancy::Unknown); };
            if (!Allowed(State)) continue;
            if (X && Y && (!Allowed(Walk(Current.Key + FIntPoint(X,0))) || !Allowed(Walk(Current.Key + FIntPoint(0,Y))))) continue;
            if((Current.Key==S||K==G)&&!SegmentAllowed(M,Position(Current.Key),Position(K),Unknown))continue;
            const float Cost = Current.G + CellSize * (X && Y ? 1.41421356f : 1.f) * (State == EOccupancy::Unknown ? 5.f : 1.f);
            FNode& Next = Nodes.FindOrAdd(K);
            if (Cost >= Next.G) continue;
            Next.G = Cost; Next.Parent = Current.Key;
            Open.push({K,Cost,Cost+H(K,G),Order++});
        }
    }
    TArray<FIntPoint> Reverse;
    for (FIntPoint K = Best;; K = Nodes.FindChecked(K).Parent) { Reverse.Add(K); if (K == S) break; }
    float Z = Start.Z;
    for (int32 I = Reverse.Num()-1; I >= 0; --I)
    {
        const auto K = Reverse[I];
        const bool Estimated = M.IsEstimated(K);
        if (Walk(K)==EOccupancy::Free) if (const auto* C = M.Find(K)) Z = C->Floor;
        Out.Points.Add({FMapSnapshot::Center(K,Z),Estimated});
    }
    if (Out.Points.Num()) Out.Points[0].Position = FVector(Start.X,Start.Y,Out.Points[0].Position.Z);
    if (Out.bComplete && Out.Points.Num()) Out.Points.Last().Position = FVector(Goal.X,Goal.Y,Out.Points.Last().Position.Z);
    for (int32 I = 1; I < Out.Points.Num(); ++I)
    {
        const float Length = FVector::Dist(Out.Points[I-1].Position,Out.Points[I].Position);
        (Out.Points[I].bEstimated || Out.Points[I-1].bEstimated ? Out.EstimatedMeters : Out.ObservedMeters) += Length;
    }
    return Out;
}
}
FRoute Plan(const FMapSnapshot& Map, FVector Start, FVector Goal, int32 ExpansionLimit)
{
    const double Begin = FPlatformTime::Seconds();
    FRoute Result = Search(Map,Start,Goal,false,ExpansionLimit);
    if (!Result.bComplete && Result.Expanded < ExpansionLimit)
    {
        FRoute Speculative = Search(Map,Start,Goal,true,ExpansionLimit-Result.Expanded);
        Speculative.Expanded += Result.Expanded;
        if (Speculative.bComplete || Speculative.Points.Num() > 1) Result = MoveTemp(Speculative);
    }
    SmoothRoute(Map,Result);
    Result.PlannerMs = (FPlatformTime::Seconds()-Begin)*1000;
    return Result;
}
bool ValidateRoute(const FMapSnapshot& Map, const FRoute& Route, int32* FirstBlocked)
{
    for (int32 I=0; I<Route.Points.Num(); ++I)
    {
        if (Map.WalkabilityAt(Route.Points[I].Position) == EOccupancy::Occupied
            ||(I>0&&!SegmentAllowed(Map,Route.Points[I-1].Position,Route.Points[I].Position,true)))
        { if (FirstBlocked) *FirstBlocked=I; return false; }
    }
    return true;
}
void SmoothRoute(const FMapSnapshot& Map,FRoute& Route)
{
    if(Route.Points.Num()<2)return;
    // Remove grid stair-steps only where the point route crosses clear cells.
    // Keep evidence boundaries: a shortcut cannot turn unknown floor mint.
    TArray<FRoutePoint> Corners;
    for(int32 I=0;I<Route.Points.Num();)
    {
        Corners.Add(Route.Points[I]);if(I==Route.Points.Num()-1)break;
        int32 Best=I+1;
        for(int32 J=I+2;J<Route.Points.Num()&&J<=I+20;++J)
        {
            if(Route.Points[J-1].bEstimated!=Route.Points[I].bEstimated||Route.Points[J].bEstimated!=Route.Points[I].bEstimated)break;
            if(SegmentAllowed(Map,Route.Points[I].Position,Route.Points[J].Position,Map.WalkabilityAt(Route.Points[I].Position)==EOccupancy::Unknown))Best=J;
        }
        I=Best;
    }
    TArray<FRoutePoint> Rounded;Rounded.Add(Corners[0]);
    for(int32 I=1;I<Corners.Num()-1;++I)
    {
        const auto& A=Corners[I-1];const auto& B=Corners[I];const auto& C=Corners[I+1];
        const float Radius=FMath::Min(.4f,float(FMath::Min(FVector::Dist(A.Position,B.Position),FVector::Dist(B.Position,C.Position))*.45));
        const FVector In=B.Position+(A.Position-B.Position).GetSafeNormal()*Radius;
        const FVector Out=B.Position+(C.Position-B.Position).GetSafeNormal()*Radius;
        bool Valid=A.bEstimated==B.bEstimated&&B.bEstimated==C.bEstimated
            &&FMath::Abs(A.Position.Z-B.Position.Z)<.02&&FMath::Abs(C.Position.Z-B.Position.Z)<.02;
        TArray<FRoutePoint> Curve;Curve.Add({In,B.bEstimated});
        const int32 Steps=FMath::Max(2,FMath::CeilToInt(2*Radius/.025f));
        for(int32 J=1;J<=Steps&&Valid;++J)
        {
            const double T=double(J)/Steps;
            const FVector P=FMath::Lerp(FMath::Lerp(In,B.Position,T),FMath::Lerp(B.Position,Out,T),T);
            Valid=SegmentAllowed(Map,Curve.Last().Position,P,Map.WalkabilityAt(B.Position)==EOccupancy::Unknown);Curve.Add({P,B.bEstimated});
        }
        if(Valid)Rounded.Append(Curve);else Rounded.Add(B);
    }
    Rounded.Add(Corners.Last());Route.Points.Reset();Route.Points.Add(Rounded[0]);
    Route.ObservedMeters=Route.EstimatedMeters=0;
    for(int32 I=1;I<Rounded.Num();++I)
    {
        const auto& A=Rounded[I-1];const auto& B=Rounded[I];
        const float Length=FVector::Dist(A.Position,B.Position);
        if(Length<.0001f)continue;
        const int32 Steps=FMath::Max(1,FMath::CeilToInt(Length/.05f));
        for(int32 J=1;J<=Steps;++J)
        {
            const FVector P=FMath::Lerp(A.Position,B.Position,double(J)/Steps);
            const bool Est=A.bEstimated||B.bEstimated||Map.IsEstimated(FMapSnapshot::Key(P));
            Route.Points.Add({P,Est});
            (Est?Route.EstimatedMeters:Route.ObservedMeters)+=Length/Steps;
        }
    }
}
void TrimTraversedRoute(FRoute& Route,FVector Viewer,const FMapSnapshot* Map)
{
    Route.bAttachedToViewer=false;
    int32 Closest=0;float Best=FLT_MAX,Nearest=FLT_MAX;
    for(int32 I=0;I<Route.Points.Num();++I)
    {
        const float D=FVector::DistSquared2D(Viewer,Route.Points[I].Position);
        Nearest=FMath::Min(Nearest,D);
        if(D<Best&&(!Map||(D<=.25f&&SegmentAllowed(*Map,Viewer,Route.Points[I].Position,true)))){Best=D;Closest=I;}
    }
    if(Map&&Best>.25f&&Nearest<=.25f)
    {Route.Points.Reset();Route.bComplete=false;return;} // Nearby route is behind a wall: replan.
    // Only advance on the current route; an off-route pose must be replanned.
    if(Closest>0&&Best<=.25f)Route.Points.RemoveAt(0,Closest,EAllowShrinking::No);
    if(Map&&Best<=.25f&&!Route.Points.IsEmpty())
    {
        Viewer.Z=Route.Points[0].Position.Z;
        const FRoutePoint Start{Viewer,Map->IsEstimated(FMapSnapshot::Key(Viewer))};
        if(Route.Points.Num()>1&&SegmentAllowed(*Map,Viewer,Route.Points[1].Position,true))Route.Points[0]=Start;
        else if(!Route.Points[0].Position.Equals(Viewer,.001))Route.Points.Insert(Start,0);
        Route.bAttachedToViewer=true;
    }
}
bool SelectStanding(const FMapSnapshot& Map, FVector Surface, bool bFloorHit, FVector Viewer, FVector& Out)
{
    if (bFloorHit)
    {
        if (FMath::Abs(Surface.Z-Map.Floor)>MaxStep || Map.WalkabilityAt(Surface) != EOccupancy::Free) return false;
        Out=Surface; return true;
    }
    float Best = TNumericLimits<float>::Max(); bool Found=false;
    const float Toward = FMath::Atan2(Viewer.Y-Surface.Y,Viewer.X-Surface.X);
    for (int32 I=0;I<24;++I)
    {
        const float A=Toward+I*UE_TWO_PI/24;
        const FVector P(Surface.X+.6f*FMath::Cos(A),Surface.Y+.6f*FMath::Sin(A),Map.Floor);
        // Stand on the visible side of the hit, never around its far side.
        if(FVector::DotProduct((P-Surface).GetSafeNormal2D(),(Viewer-Surface).GetSafeNormal2D())<.25f)continue;
        if (Map.WalkabilityAt(P) != EOccupancy::Free) continue;
        // Do not choose a point through the object from the hit surface.
        bool Clear=true;
        for (float T=.2f;T<=1.f;T+=.1f)
        {
            const auto S=Map.State(FMapSnapshot::Key(FMath::Lerp(FVector(Surface.X,Surface.Y,Map.Floor),P,T)));
            if (S==EOccupancy::Occupied || S==EOccupancy::Unsupported) { Clear=false; break; }
        }
        if (!Clear) continue;
        const float D=FVector::DistSquared2D(P,Viewer);
        if (D<Best) { Best=D; Out=P; Found=true; }
    }
    return Found;
}
bool HasArrived(const FMapSnapshot& Map, const FTarget& T, FVector Viewer)
{
    return FVector::Dist2D(Viewer,T.Standing)<=.6f && Map.WalkabilityAt(T.Standing)==EOccupancy::Free
        && Map.WalkabilityAt(Viewer)==EOccupancy::Free
        && !Map.IsEstimated(FMapSnapshot::Key(T.Standing)) && !Map.IsEstimated(FMapSnapshot::Key(Viewer))
        && SegmentAllowed(Map,Viewer,T.Standing,false);
}
const TCHAR* StateLabel(ERouteState S)
{
    switch (S) {
    case ERouteState::Planning:return TEXT("PLANNING"); case ERouteState::Mapped:return TEXT("MAPPED ROUTE");
    case ERouteState::Estimated:return TEXT("ESTIMATED"); case ERouteState::Incomplete:return TEXT("INCOMPLETE ROUTE");
    case ERouteState::Blocked:return TEXT("BLOCKED / REPLANNING"); case ERouteState::Arrived:return TEXT("ARRIVED");
    case ERouteState::Relocalizing:return TEXT("RELOCALIZING"); case ERouteState::StartBlocked:return TEXT("START POINT BLOCKED"); default:return TEXT("MARK A DESTINATION"); }
}
}
