#pragma once

#include "CoreMinimal.h"

/** One synthetic observation in the same Unreal world frame as the viewer. */
struct FWallhackSpatialContact
{
    int32 Id = INDEX_NONE;
    // Deliberately zero when invalid: consumers must not display an old point.
    FVector WorldPosition = FVector::ZeroVector;
    float AgeSeconds = TNumericLimits<float>::Max();
    bool bPositionValid = false;
    bool bStale = true;
};
