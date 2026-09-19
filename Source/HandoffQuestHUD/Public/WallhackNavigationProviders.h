#pragma once
#include "WallhackNavigationTypes.h"

namespace WallhackNav
{
struct FSurfaceHit { FVector Point=FVector::ZeroVector, Normal=FVector::UpVector; bool bFloor=false; };
// A narrowly bounded wearer volume, not an obstacle-clearance override. Hits
// inside it are unobservable environment: never floor/clearance evidence.
struct HANDOFFQUESTHUD_API FWearerDepthMask
{
    FVector Head=FVector::ZeroVector;
    float Floor=0;
    TArray<FVector> TrackedHands;
    bool Contains(FVector Point) const;
};
struct FFloor { TArray<FVector2D> Polygon; float Z=0; };
struct FObstacle
{
    FBox Box=FBox(ForceInit);
    bool bWall=false;
    // Oriented anchor geometry prevents diagonal walls from filling their AABB.
    FBox LocalBox=FBox(ForceInit);
    FTransform LocalToWorld=FTransform::Identity;
};
struct FScene
{
    TArray<FFloor> Floors;
    TArray<FObstacle> Obstacles;
    FBox Bounds=FBox(ForceInit);
    double LoadedAt=0;
    bool Sample(FVector P, FCell& Out) const;
    bool Raycast(FVector Origin, FVector Direction, float Distance, FSurfaceHit& Out) const;
};
enum class ESceneStatus : uint8 { Loading, Ready, Missing, Failed };
enum class EDepthResult : uint8 { Hit, Unknown, Unavailable };
class ISceneProvider
{
public:
    virtual ~ISceneProvider()=default;
    virtual void Start()=0;
    virtual ESceneStatus Poll(FScene& Scene)=0;
    virtual bool Raycast(FVector Origin,FVector Direction,float Distance,FSurfaceHit& Out)=0;
    virtual bool RequestScan()=0;
    virtual bool IsLocalized() const { return true; }
};
class IDepthProvider
{
public:
    virtual ~IDepthProvider()=default;
    virtual void Start()=0;
    virtual EDepthResult Query(FVector Origin,FVector Direction,float Distance,FSurfaceHit& Out)=0;
};
// Test and desktop geometry uses precisely the same interfaces as Quest.
class HANDOFFQUESTHUD_API FDeterministicProvider : public ISceneProvider, public IDepthProvider
{
public:
    FScene Scanned, Live;
    bool bAvailable=true,bLocalized=true;
    virtual bool IsLocalized() const override { return bLocalized; }
    virtual void Start() override {}
    virtual ESceneStatus Poll(FScene& Scene) override { Scene=Scanned; return ESceneStatus::Ready; }
    virtual bool Raycast(FVector O,FVector D,float L,FSurfaceHit& H) override { return Live.Raycast(O,D,L,H); }
    virtual bool RequestScan() override { return false; }
    virtual EDepthResult Query(FVector O,FVector D,float L,FSurfaceHit& H) override
    { return !bAvailable ? EDepthResult::Unavailable : (Live.Raycast(O,D,L,H) ? EDepthResult::Hit : EDepthResult::Unknown); }
    static TSharedPtr<FDeterministicProvider> MakeRoom();
};
HANDOFFQUESTHUD_API TSharedPtr<ISceneProvider> MakeSceneProvider(UWorld* World);
HANDOFFQUESTHUD_API TSharedPtr<IDepthProvider> MakeDepthProvider(UWorld* World);
}
