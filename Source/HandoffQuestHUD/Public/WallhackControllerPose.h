#pragma once

#include "CoreMinimal.h"
#include "HeadMountedDisplayTypes.h"

namespace WallhackControllerPose
{
    // bValid alone can mean that a pose was supplied in a previous frame.
    // Never use an inertial/stale pose or a hand interaction as the rig mount.
    inline bool Resolve(const FXRMotionControllerState& State, FTransform& Out)
    {
        if (!State.bValid || State.TrackingStatus != ETrackingStatus::Tracked
            || State.DeviceName.IsNone() || State.DeviceName.ToString().Contains(TEXT("hand_interaction"))
            || State.ControllerLocation.ContainsNaN() || State.ControllerRotation.ContainsNaN()
            || !State.ControllerRotation.IsNormalized()) return false;
        Out = FTransform(State.ControllerRotation, State.ControllerLocation);
        return true;
    }
}
