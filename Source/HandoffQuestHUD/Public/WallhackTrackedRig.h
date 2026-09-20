#pragma once
#include "CoreMinimal.h"

/** Session mount calibration, in metres. The left aim ray is physically aligned
 * with sensor forward. An upright startup mark supplies origin and roll offset. */
class HANDOFFQUESTHUD_API FWallhackRigAlignment
{
public:
    bool Align(FVector RadarCentre, const FTransform& ControllerAim, FString& Error);
    bool SetMount(const FTransform& InMount, FString& Error);
    void Reset() { bReady=false; Mount=FTransform::Identity; }
    bool IsReady() const { return bReady; }
    FTransform Resolve(const FTransform& Controller) const { return Mount*Controller; }
    const FTransform& GetMount() const { return Mount; }
private:
    bool bReady=false;
    FTransform Mount=FTransform::Identity;
};

/** Samples the controller-to-radar offset while the rig and right probe are
 * still. The result needs explicit confirmation; it never changes XR origin. */
class HANDOFFQUESTHUD_API FWallhackRigCalibrationCapture
{
public:
    void Start() { Reset(); bActive=true; }
    void Reset();
    void LoseTracking();
    bool Observe(double At, const FTransform& LeftAim, FVector RadarCentre, FString& Status,bool bEstimated=false);
    bool IsActive() const { return bActive; }
    bool IsReady() const { return bReady; }
    bool IsEstimated() const { return bReady?bResultEstimated:bWindowEstimated; }
    float Progress() const;
    const FTransform& GetMount() const { return Result; }
private:
    void ClearWindow();
    bool bActive=false,bReady=false;
    bool bWindowEstimated=false,bResultEstimated=false;
    double FirstAt=-1,LastAt=-1;
    int32 Count=0;
    FTransform FirstAim=FTransform::Identity,FirstMount=FTransform::Identity,Result=FTransform::Identity;
    FVector SumOffset=FVector::ZeroVector;
    FQuat SumRotation=FQuat(0,0,0,0);
};

struct FWallhackRigGuideGeometry
{
    TArray<FVector> Vertices;
    TArray<int32> Indices;
    TArray<FLinearColor> Colors;
};

/** White origin and mint forward arrow, in world metres. */
HANDOFFQUESTHUD_API FWallhackRigGuideGeometry BuildWallhackRigCalibrationGuides(const FTransform& SensorMetres);

/** Bounded pose history. Never extrapolate or bridge a tracking gap. */
class HANDOFFQUESTHUD_API FWallhackRigHistory
{
public:
    void Add(double At,const FTransform& Pose,bool bEstimated=false);
    bool Sample(double At,FTransform& Out,bool* bEstimated=nullptr) const;
    void Reset() { Samples.Reset(); }
private:
    struct FSample { double At; FTransform Pose; bool bEstimated; };
    TArray<FSample> Samples;
};
