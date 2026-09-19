#pragma once

#include "CoreMinimal.h"

namespace WallhackNav
{
// All navigation math uses metres in MRUK's world frame. Conversion to Unreal
// units happens only at provider, pose, and renderer boundaries.
constexpr float CellSize = .1f;
constexpr float StandingHeight = 1.8f;
constexpr float MaxStep = .15f;
constexpr int32 TileSize = 16;
enum class EOccupancy : uint8 { Unknown, Free, Occupied, Unsupported };
enum class EEvidence : uint8 { None, SceneFloor, SceneWall, SceneObject, Depth };
enum class ERouteState : uint8 { Idle, Planning, Mapped, Estimated, Incomplete, Blocked, Arrived, Relocalizing, StartBlocked };
struct FCell
{
    float Floor = 0;
    float ObservedHeight = 0, SurfaceNormalZ = 0;
    FVector3f ObserverOffset=FVector3f::ZeroVector;
    float ObserverEyeHeight=0;
    double ObservedAt = -1;
    EOccupancy Occupancy = EOccupancy::Unknown;
    EEvidence Evidence = EEvidence::None;
    uint8 ClearVotes = 0;
    bool bFloor = false;
    bool bClearance = false;
};
struct FTile { FCell Cells[TileSize * TileSize]; };
struct FMapSnapshot
{
    TMap<FIntPoint, TSharedPtr<FTile, ESPMode::ThreadSafe>> Tiles;
    uint64 Revision = 0;
    float Floor = 0;
    double Now = 0;
    static FIntPoint Key(const FVector& P) { return {FMath::FloorToInt32(P.X / CellSize), FMath::FloorToInt32(P.Y / CellSize)}; }
    static FVector Center(FIntPoint K, float Z) { return {(K.X + .5) * CellSize, (K.Y + .5) * CellSize, Z}; }
    static FIntPoint TileKey(FIntPoint K) { return {FMath::FloorToInt32(double(K.X) / TileSize), FMath::FloorToInt32(double(K.Y) / TileSize)}; }
    const FCell* Find(FIntPoint K) const;
    EOccupancy State(FIntPoint K) const;
    // Point navigation: only the containing cell, with no body-radius inflation.
    // Floor evidence and same-floor checks still apply.
    EOccupancy Walkability(FIntPoint K, FIntPoint* FirstBlockingCell = nullptr) const;
    EOccupancy WalkabilityAt(FVector Position, FIntPoint* FirstBlockingCell = nullptr) const;
};
class FMap : public FMapSnapshot
{
public:
    bool Observe(FIntPoint K, const FCell& Observation);
    void Reset() { Tiles.Reset(); ++Revision; }
};
struct FRoutePoint { FVector Position = FVector::ZeroVector; bool bEstimated = false; };
struct FRoute
{
    TArray<FRoutePoint> Points;
    bool bComplete = false;
    bool bStartBlocked = false;
    int32 Expanded = 0;
    float ObservedMeters = 0, EstimatedMeters = 0;
    double PlannerMs = 0;
    uint64 Revision = 0;
    float Length() const { return ObservedMeters + EstimatedMeters; }
};
struct FTarget
{
    FVector Surface = FVector::ZeroVector;
    FVector Standing = FVector::ZeroVector;
    // Session marker identity. Ordinary floor targets have no person link.
    int32 PersonId = INDEX_NONE;
};
struct FDisplaySnapshot
{
    FTarget Target;
    FRoute Route;
    ERouteState State = ERouteState::Idle;
    FString Tracking = TEXT("LOADING ROOM");
    FString NextTurn = TEXT("--");
    FString AimStatus = TEXT("NOT AIMING");
    FVector Viewer = FVector::ZeroVector;
    FQuat Orientation = FQuat::Identity;
    float Range = 0, Bearing = 0, Height = 0, Walking = 0;
    float ObservationAge = -1, MappingMs = 0, QueryMs = 0;
    float FPS = 0;
    int32 Battery = -1;
    int32 CellCount = 0;
    bool bHasTarget = false, bGuidance = false, bDepthAvailable = false;
    bool bAiming = false, bAimTracked = false, bPreviewValid = false, bHidden = false;
    FVector AimOrigin = FVector::ZeroVector, AimEnd = FVector::ZeroVector;
    FTarget Preview;
};
HANDOFFQUESTHUD_API FRoute Plan(const FMapSnapshot& Map, FVector Start, FVector Goal, int32 ExpansionLimit = 50000);
HANDOFFQUESTHUD_API bool ValidateRoute(const FMapSnapshot& Map, const FRoute& Route, int32* FirstBlocked = nullptr);
// Rounded, densely sampled geometry is shared by guidance, metrics and the map.
HANDOFFQUESTHUD_API void SmoothRoute(const FMapSnapshot& Map, FRoute& Route);
HANDOFFQUESTHUD_API void TrimTraversedRoute(FRoute& Route, FVector Viewer);
HANDOFFQUESTHUD_API bool SelectStanding(const FMapSnapshot& Map, FVector Surface, bool bFloorHit, FVector Viewer, FVector& Out);
HANDOFFQUESTHUD_API bool HasArrived(const FMapSnapshot& Map, const FTarget& Target, FVector Viewer);
HANDOFFQUESTHUD_API const TCHAR* StateLabel(ERouteState State);
}
