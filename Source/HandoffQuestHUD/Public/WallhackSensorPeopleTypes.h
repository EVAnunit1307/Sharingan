#pragma once

#include "CoreMinimal.h"

struct FWallhackSensorPerson
{
    int32 Id = INDEX_NONE;
    int32 CameraGeneration = 0;
    int32 RadarId = INDEX_NONE;
    int32 RadarGeneration = 0;
    float Confidence = 0;
    FVector2D Position = FVector2D::ZeroVector; // right, forward; metres
    FVector2D Fallback = FVector2D::ZeroVector;
    bool bRadar = false;
    bool bHasFallback = false;
    double CameraExpires = 0;
    double RadarExpires = 0;
};

struct FWallhackSensorDot
{
    int32 Id = INDEX_NONE;
    int32 Generation = 0;
    FVector2D Position = FVector2D::ZeroVector;
    double Expires = 0;
};

struct FWallhackTrackedPerson
{
    int32 Id = INDEX_NONE;
    FVector2D Position = FVector2D::ZeroVector, Velocity = FVector2D::ZeroVector;
    float Height = 1.65f, Facing = 0, PoseYaw = 0;
    FString Source, HeightSource, FacingSource, PersonEvidence;
    TArray<FVector> Joints; // Camera-relative UE axes, metres, hip origin.
    TArray<float> JointQuality;
    double Expires = 0, PoseExpires = 0;
    double ObservedAt = 0, PoseObservedAt = 0;
    FString SampleKey;
};

/** Presentation only: blend fresh positions for 120 ms, never predict motion or
 * keep a contact alive. Call BeginFrame/EndFrame around the current fresh set. */
class HANDOFFQUESTHUD_API FWallhackSensorPositionInterpolator
{
public:
    void BeginFrame(double Now);
    FVector2D Sample(const FString& Key, FVector2D Position);
    void EndFrame();
    void Reset();
    int32 Num() const { return Tracks.Num(); }
private:
    struct FTrack
    {
        FVector2D From = FVector2D::ZeroVector, To = FVector2D::ZeroVector;
        double StartedAt = 0;
        bool bSeen = false;
    };
    TMap<FString, FTrack> Tracks;
    double Time = -1;
    FVector2D Evaluate(const FTrack& Track) const;
};

struct FWallhackSensorPeopleFrame
{
    FString RegistrationKey;
    FString Status = TEXT("WAITING FOR GROUND STATION");
    bool bReplay = false;
    int32 Unpositioned = 0;
    TArray<FWallhackSensorPerson> People;
    TArray<FWallhackSensorDot> Radar;
    bool bHasTracks = false;
    bool bRigPoseValid = true;
    FString RigMotionMode = TEXT("stationary");
    TArray<FWallhackTrackedPerson> Tracks;
};

/** Validates the additive spatial_people protocol independently of the legacy rig schema. */
class HANDOFFQUESTHUD_API FWallhackSensorPeopleStream
{
public:
    bool Ingest(const FString& Json, double Now);
    FWallhackSensorPeopleFrame GetFrame(double Now) const;
    void Reset();
private:
    struct FExpiry { int32 Generation = -1, Frame = -1; double At = 0; };
    FWallhackSensorPeopleFrame Last;
    FString RelaySession;
    int64 Sequence = -1;
    FExpiry CameraExpiry;
    int32 RadarGeneration = -1;
    int32 RadarHighWater = -1;
    TMap<int32, double> RadarExpiries;
    TMap<FString, double> TrackExpiries;
    double CameraDeadline(int32 Generation, int32 Frame, double AgeMs, double Now);
    double RadarDeadline(int32 Generation, int32 Frame, double AgeMs, double Now);
};

namespace WallhackSensorPeopleMath
{
    /** Fixed ground plane, +X forward and +Y right in Unreal world. */
    inline FVector ToWorld(FVector2D RightForward, const FTransform& Reference, float WorldToMeters)
    {
        const float Yaw = Reference.Rotator().Yaw;
        return Reference.GetLocation() + FRotator(0, Yaw, 0).RotateVector(
            FVector(RightForward.Y, RightForward.X, 0) * WorldToMeters);
    }
    inline bool FloorAim(FVector Origin, FVector Direction, float FloorZ, FVector& Out)
    {
        if (Origin.ContainsNaN() || Direction.ContainsNaN() || !FMath::IsFinite(FloorZ) || Direction.Z >= -.01f) return false;
        const double Distance = (FloorZ - Origin.Z) / Direction.Z;
        if (!FMath::IsFinite(Distance) || Distance <= 0) return false;
        Out = Origin + Direction * Distance;
        return !Out.ContainsNaN();
    }
}
