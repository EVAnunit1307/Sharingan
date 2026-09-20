#include "WallhackNavigationProviders.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "GameFramework/WorldSettings.h"
#if PLATFORM_ANDROID
#include "MRUtilityKitSubsystem.h"
#include "MRUtilityKitAnchor.h"
#include "MRUtilityKitRoom.h"
#include "OculusXRFunctionLibrary.h"
#include "OculusXRAnchorBPFunctionLibrary.h"
#endif

namespace WallhackNav
{
bool FWearerDepthMask::Contains(FVector Point) const
{
    // The camera also sees the standing wearer's waist and legs. Mask only
    // a narrow body column; the floor/feet band and nearby external obstacles
    // remain observable. Unknown self-occlusion never clears cached geometry.
    if(Head.Z-Floor>=1.2f && Point.Z>=Floor+.18f
        && Point.Z<=Head.Z-.12 && FVector::DistSquared2D(Point,Head)<=FMath::Square(.23f))return true;
    if(Point.Z>Floor+.25f)for(const FVector& Hand:TrackedHands)
    {
        if(FVector::DistSquared(Point,Hand)<=FMath::Square(.12f))return true;
        if(Head.Z-Floor>=1.2f)
        {
            const FVector Shoulder=Head+(Hand-Head).GetSafeNormal2D()*.18-FVector(0,0,.3);
            const FVector Arm=Hand-Shoulder;
            const double T=FMath::Clamp(FVector::DotProduct(Point-Shoulder,Arm)/FMath::Max(Arm.SizeSquared(),.0001),0.0,1.0);
            if(FVector::DistSquared(Point,Shoulder+T*Arm)<=FMath::Square(.09f))return true;
        }
    }
    return false;
}
namespace
{
bool InPolygon(FVector2D P,const TArray<FVector2D>& V)
{
    bool In=false;
    for (int32 I=0,J=V.Num()-1;I<V.Num();J=I++)
        if ((V[I].Y>P.Y)!=(V[J].Y>P.Y) && P.X<(V[J].X-V[I].X)*(P.Y-V[I].Y)/(V[J].Y-V[I].Y)+V[I].X) In=!In;
    return In;
}
bool BoxRay(const FBox& B,FVector O,FVector D,float Max,float& T,FVector& Normal)
{
    double Near=0,Far=Max; FVector N=FVector::ZeroVector;
    for (int32 I=0;I<3;++I)
    {
        if (FMath::Abs(D[I])<1e-8) { if(O[I]<B.Min[I]||O[I]>B.Max[I])return false; continue; }
        double A=(B.Min[I]-O[I])/D[I],Z=(B.Max[I]-O[I])/D[I];
        float Sign=-1; if(A>Z){Swap(A,Z);Sign=1;}
        if(A>Near){Near=A;N=FVector::ZeroVector;N[I]=Sign;}
        Far=FMath::Min(Far,Z); if(Near>Far)return false;
    }
    if(Near<.001||Near>Max)return false;
    T=Near;Normal=N;return true;
}
}
bool FScene::Sample(FVector P,FCell& Out) const
{
    // The active floor is not necessarily world Z=0. Outside a floor polygon
    // its elevation must still be used to rasterize the enclosing walls.
    bool Floor=false; float Z=P.Z,Best=FLT_MAX;
    for(const auto& F:Floors) if(InPolygon(FVector2D(P),F.Polygon)&&FMath::Abs(F.Z-P.Z)<Best)
    {Z=F.Z;Best=FMath::Abs(F.Z-P.Z);Floor=true;}
    // Include walls outside the floor's polygon so unknown routes cannot bypass
    // scanned boundaries by stepping into the first unseeded cell.
    for(const auto& O:Obstacles)
    {
        if(O.LocalBox.IsValid)
        {
            const FVector Local=O.LocalToWorld.InverseTransformPosition(FVector(P.X,P.Y,FMath::Clamp(double(Z+.9f),O.Box.Min.Z,O.Box.Max.Z)));
            if(!O.LocalBox.ExpandBy(.071f).IsInsideOrOn(Local))continue;
        }
        if(P.X>=O.Box.Min.X-.05 && P.X<=O.Box.Max.X+.05 && P.Y>=O.Box.Min.Y-.05 && P.Y<=O.Box.Max.Y+.05
            && O.Box.Max.Z>Z+.05 && O.Box.Min.Z<Z+StandingHeight)
        { Out={};Out.Floor=Z;Out.bFloor=Floor;Out.ObservedAt=LoadedAt;Out.Occupancy=EOccupancy::Occupied;
          Out.Evidence=O.bWall?EEvidence::SceneWall:EEvidence::SceneObject;return true; }
    }
    if(!Floor)return false;
    Out={};Out.Floor=Z;Out.bFloor=true;Out.bClearance=true;Out.ObservedAt=LoadedAt;
    Out.Occupancy=EOccupancy::Free;Out.Evidence=EEvidence::SceneFloor;return true;
}
bool FScene::Raycast(FVector O,FVector D,float Distance,FSurfaceHit& Out) const
{
    bool Hit=false;float Nearest=Distance;
    for(const auto& B:Obstacles)
    {
        float T;FVector N;
        const bool Valid=B.LocalBox.IsValid
            ? BoxRay(B.LocalBox,B.LocalToWorld.InverseTransformPosition(O),B.LocalToWorld.InverseTransformVectorNoScale(D),Nearest,T,N)
            : BoxRay(B.Box,O,D,Nearest,T,N);
        if(Valid){Nearest=T;Out={O+D*T,B.LocalBox.IsValid?B.LocalToWorld.TransformVectorNoScale(N):N,false};Hit=true;}
    }
    if(FMath::Abs(D.Z)>1e-6)for(const auto& F:Floors)
    {
        const float T=(F.Z-O.Z)/D.Z;
        if(T>.001f&&T<Nearest&&InPolygon(FVector2D(O+D*T),F.Polygon))
        {Nearest=T;Out={O+D*T,FVector::UpVector,true};Hit=true;}
    }
    return Hit;
}
TSharedPtr<FDeterministicProvider> FDeterministicProvider::MakeRoom()
{
    auto P=MakeShared<FDeterministicProvider>();
    P->Scanned.Floors.Add({{{-3,-3},{5,-3},{5,3},{-3,3}},0});
    P->Scanned.Bounds=FBox(FVector(-3.1,-3.1,0),FVector(5.1,3.1,2.5));
    auto Wall=[&](FVector A,FVector B){P->Scanned.Obstacles.Add({FBox(A,B),true});};
    Wall({-3.1,-3.1,0},{-3,3.1,2.5});Wall({-3,-3.1,0},{5.1,-3,2.5});Wall({-3,3,0},{5.1,3.1,2.5});
    Wall({5,-3,0},{5.1,-.65,2.5});Wall({5,.65,0},{5.1,3,2.5});
    P->Scanned.Obstacles.Add({FBox(FVector(1.5,-.5,0),FVector(2.1,.5,1.1)),false});
    P->Live=P->Scanned;
    P->Live.Floors.Add({{{5,-1},{12,-1},{12,1},{5,1}},0});
    P->Live.Obstacles.Add({FBox(FVector(5,-1.1,0),FVector(12,-1,2.5)),true});
    P->Live.Obstacles.Add({FBox(FVector(5,1,0),FVector(12,1.1,2.5)),true});
    return P;
}
#if PLATFORM_ANDROID
namespace
{
class FMetaScene : public ISceneProvider
{
    TWeakObjectPtr<UMRUKSubsystem> MRUK;
    float Scale=100;
    bool bCaptured=false;
    FScene Cached;
public:
    explicit FMetaScene(UWorld* W) : MRUK(W->GetGameInstance()->GetSubsystem<UMRUKSubsystem>()),Scale(W->GetWorldSettings()->WorldToMeters) {}
    virtual void Start() override { bCaptured=false;if(MRUK.IsValid()){MRUK->EnableWorldLock=true;MRUK->LoadSceneFromDevice();} }
    virtual bool RequestScan() override { bCaptured=false;return MRUK.IsValid()&&MRUK->LaunchSceneCapture(); }
    virtual bool IsLocalized() const override
    {
        if(!MRUK.IsValid())return false;
        const AMRUKRoom* Room=MRUK->GetCurrentRoom();if(!IsValid(Room))return false;
        for(AMRUKAnchor* Anchor:Room->AllAnchors)
        {
            if(!IsValid(Anchor)||!Anchor->HasLabel(TEXT("FLOOR")))continue;
            FTransform Pose;FOculusXRAnchorLocationFlags Flags;
            if(UOculusXRAnchorBPFunctionLibrary::TryGetAnchorTransformByHandle(Anchor->SpaceHandle,Pose,Flags,EOculusXRAnchorSpace::World)
                &&Flags.IsValid()&&!Pose.ContainsNaN())return true;
        }
        return false;
    }
    virtual ESceneStatus Poll(FScene& Scene) override
    {
        if(!MRUK.IsValid())return ESceneStatus::Failed;
        if(MRUK->SceneLoadStatus==EMRUKInitStatus::Failed)return ESceneStatus::Missing;
        if(MRUK->SceneLoadStatus!=EMRUKInitStatus::Complete)return ESceneStatus::Loading;
        if(MRUK->Rooms.IsEmpty())return ESceneStatus::Missing;
        if(!bCaptured)
        {
            Cached={};
            for(AMRUKRoom* Room:MRUK->Rooms)
            {
                if(!IsValid(Room))continue;
                for(AMRUKAnchor* A:Room->AllAnchors)
                {
                    if(!IsValid(A))continue;
                    const auto T=A->GetActorTransform();
                    if(A->HasLabel(TEXT("FLOOR")))
                    {
                        FFloor F;F.Z=A->GetActorLocation().Z/Scale;
                        for(const auto& V:A->PlaneBoundary2D){const FVector P=T.TransformPosition(FVector(0,V.X,V.Y))/Scale;F.Polygon.Add(FVector2D(P));Cached.Bounds+=P;}
                        if(F.Polygon.Num()>=3)Cached.Floors.Add(MoveTemp(F));
                    }
                    else if(A->HasLabel(TEXT("CEILING"))&&A->PlaneBounds.bIsValid)
                    {
                        FObstacle Ceiling;Ceiling.bWall=true;
                        Ceiling.LocalBox=FBox(FVector(-1,A->PlaneBounds.Min.X,A->PlaneBounds.Min.Y)/Scale,FVector(1,A->PlaneBounds.Max.X,A->PlaneBounds.Max.Y)/Scale);
                        Ceiling.LocalToWorld=FTransform(T.GetRotation(),T.GetTranslation()/Scale);
                        for(int32 I=0;I<8;++I)Ceiling.Box+=Ceiling.LocalToWorld.TransformPosition(FVector(I&1?Ceiling.LocalBox.Max.X:Ceiling.LocalBox.Min.X,I&2?Ceiling.LocalBox.Max.Y:Ceiling.LocalBox.Min.Y,I&4?Ceiling.LocalBox.Max.Z:Ceiling.LocalBox.Min.Z));
                        Cached.Obstacles.Add(Ceiling);
                    }
                    else if(A->HasLabel(TEXT("WALL_FACE"))&&A->PlaneBounds.bIsValid)
                    {
                        // Split the wall around full-height scanned door openings.
                        TArray<FVector2D> Openings;
                        for(AMRUKAnchor* Child:A->ChildAnchors)if(IsValid(Child)&&Child->HasLabel(TEXT("DOOR_FRAME"))&&Child->PlaneBounds.bIsValid)
                        {
                            const FVector C=T.InverseTransformPosition(Child->GetActorLocation());
                            const float Half=Child->PlaneBounds.GetSize().X*.5f;
                            // Only openings that reach the floor and standing clearance.
                            FBox Door(ForceInit);
                            for(int32 I=0;I<4;++I)Door+=Child->GetActorTransform().TransformPosition(FVector(0,I&1?Child->PlaneBounds.Max.X:Child->PlaneBounds.Min.X,I&2?Child->PlaneBounds.Max.Y:Child->PlaneBounds.Min.Y))/Scale;
                            const float Bottom=T.TransformPosition(FVector(0,A->PlaneBounds.Min.X,A->PlaneBounds.Min.Y)).Z/Scale;
                            if(Door.Min.Z<=Bottom+.2f&&Door.Max.Z>=Bottom+StandingHeight)Openings.Add({C.Y-Half,C.Y+Half});
                        }
                        Openings.Sort([](const auto& A,const auto& B){return A.X<B.X;});
                        float Left=A->PlaneBounds.Min.X;
                        auto AddWall=[&](float L,float R)
                        {
                            if(R<=L)return;FBox B(ForceInit);
                            for(int32 I=0;I<8;++I)B+=T.TransformPosition(FVector(I&1?2.5:-2.5,I&2?R:L,I&4?A->PlaneBounds.Max.Y:A->PlaneBounds.Min.Y))/Scale;
                            FObstacle Obstacle{B,true};
                            Obstacle.LocalBox=FBox(FVector(-2.5,L,A->PlaneBounds.Min.Y)/Scale,FVector(2.5,R,A->PlaneBounds.Max.Y)/Scale);
                            Obstacle.LocalToWorld=FTransform(T.GetRotation(),T.GetTranslation()/Scale);
                            Cached.Obstacles.Add(Obstacle);Cached.Bounds+=B;
                        };
                        for(auto O:Openings){AddWall(Left,FMath::Min(float(O.X),float(A->PlaneBounds.Max.X)));Left=FMath::Max(Left,float(O.Y));}
                        AddWall(Left,A->PlaneBounds.Max.X);
                    }
                    else if(A->VolumeBounds.IsValid)
                    {
                        FBox B(ForceInit);for(int32 I=0;I<8;++I)B+=T.TransformPosition(FVector(I&1?A->VolumeBounds.Max.X:A->VolumeBounds.Min.X,I&2?A->VolumeBounds.Max.Y:A->VolumeBounds.Min.Y,I&4?A->VolumeBounds.Max.Z:A->VolumeBounds.Min.Z))/Scale;
                        FObstacle Obstacle{B,false};
                        Obstacle.LocalBox=FBox(A->VolumeBounds.Min/Scale,A->VolumeBounds.Max/Scale);
                        Obstacle.LocalToWorld=FTransform(T.GetRotation(),T.GetTranslation()/Scale);
                        Cached.Obstacles.Add(Obstacle);Cached.Bounds+=B;
                    }
                }
            }
            bCaptured=true;
            UE_LOG(LogTemp,Display,TEXT("Wallhack navigation: MRUK loaded floors=%d obstacles=%d"),Cached.Floors.Num(),Cached.Obstacles.Num());
        }
        Scene=Cached;return Cached.Floors.IsEmpty()?ESceneStatus::Missing:ESceneStatus::Ready;
    }
    virtual bool Raycast(FVector O,FVector D,float L,FSurfaceHit& H) override { return Cached.Raycast(O,D,L,H); }
};
class FMetaDepth : public IDepthProvider
{
    TWeakObjectPtr<UMRUKSubsystem> MRUK;float Scale;
public:
    explicit FMetaDepth(UWorld* W):MRUK(W->GetGameInstance()->GetSubsystem<UMRUKSubsystem>()),Scale(W->GetWorldSettings()->WorldToMeters){}
    virtual ~FMetaDepth() override {if(MRUK.IsValid())MRUK->DestroyEnvironmentRaycaster();}
    virtual void Start() override
    {
        if(MRUK.IsValid())MRUK->CreateEnvironmentRaycaster();
        UOculusXRFunctionLibrary::StartEnvironmentDepth();
        // The installed Epic engine supports the SDK's hard occlusion pass.
        // Soft occlusion requires Meta's engine fork and asserts in this build.
        // The translucent guidance material bypasses its depth buffer so the
        // trail stays visible through walls; depth queries still feed the map.
        UOculusXRFunctionLibrary::SetXROcclusionsMode(MRUK.Get(),EOculusXROcclusionsMode::HardOcclusions_Deprecated);
    }
    virtual EDepthResult Query(FVector O,FVector D,float L,FSurfaceHit& H) override
    {
        if(!MRUK.IsValid()||MRUK->EnvironmentRaycasterStatus()!=EMRUKEnvironmentRaycasterStatus::Ready)return EDepthResult::Unavailable;
        const auto R=MRUK->RaycastEnvironment(O*Scale,D,L*Scale);
        if(R.status==EMRUKEnvironmentRaycastHitStatus::Failure)return EDepthResult::Unavailable;
        if(R.status!=EMRUKEnvironmentRaycastHitStatus::Hit)return EDepthResult::Unknown;
        H={R.point/Scale,R.normal,R.normal.Z>.85};return EDepthResult::Hit;
    }
};
}
#endif
TSharedPtr<ISceneProvider> MakeSceneProvider(UWorld* W)
{
#if PLATFORM_ANDROID
    return MakeShared<FMetaScene>(W);
#else
    return FDeterministicProvider::MakeRoom();
#endif
}
TSharedPtr<IDepthProvider> MakeDepthProvider(UWorld* W)
{
#if PLATFORM_ANDROID
    return MakeShared<FMetaDepth>(W);
#else
    return FDeterministicProvider::MakeRoom();
#endif
}
}
