#pragma once

#include "CoreMinimal.h"

namespace WallhackMountedController
{
    struct FAimSample
    {
        bool bFocused = false;
        bool bActive = false;
        bool bPositionValid = false;
        bool bOrientationValid = false;
        bool bPositionTracked = false;
        bool bOrientationTracked = false;
        FTransform TrackingPose = FTransform::Identity;
    };

    // A runtime-valid physical pose may be inferred/last-known while optical
    // tracking is unavailable. Preserve it as an explicit estimate, independently
    // of held/detached classification. Never invent or cache a missing pose.
    inline bool ResolveAvailable(const FAimSample& Sample, const FTransform& TrackingToWorld,
        FTransform& Out, bool& bEstimated)
    {
        Out = FTransform::Identity;bEstimated=false;
        if (!Sample.bFocused || !Sample.bActive || !Sample.bPositionValid || !Sample.bOrientationValid
            || Sample.TrackingPose.ContainsNaN() || !Sample.TrackingPose.GetRotation().IsNormalized()
            || TrackingToWorld.ContainsNaN() || !TrackingToWorld.GetRotation().IsNormalized()) return false;
        Out = Sample.TrackingPose * TrackingToWorld;
        bEstimated=!Sample.bPositionTracked||!Sample.bOrientationTracked;
        return !Out.ContainsNaN();
    }

    // Strict consumer API remains available for operations requiring a measured pose.
    inline bool Resolve(const FAimSample& Sample, const FTransform& TrackingToWorld, FTransform& Out)
    {
        bool bEstimated=false;
        if(!ResolveAvailable(Sample,TrackingToWorld,Out,bEstimated)||bEstimated)
        {Out=FTransform::Identity;return false;}
        return true;
    }

    WALLHACKXR_API bool GetAim(FTransform& WorldPose, bool& bEstimated);
    WALLHACKXR_API FString GetDiagnostics();
}
