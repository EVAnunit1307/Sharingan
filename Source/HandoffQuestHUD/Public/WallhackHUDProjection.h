#pragma once

#include "CoreMinimal.h"

namespace WallhackHUDProjection
{
    struct FEdgeIndicator
    {
        bool bOutside = false;
        FVector2D Position = FVector2D::ZeroVector;
        FVector2D Direction = FVector2D::ZeroVector;
    };

    // Navigation arrows occupy a comfortable inner edge of the visor. These
    // are direction cues, not a projection used to position the actual meshes.
    inline FEdgeIndicator ProjectOffscreen(const FVector& Position, const FVector& Viewer,
        const FQuat& Orientation)
    {
        FEdgeIndicator Result;
        if (Position.ContainsNaN() || Viewer.ContainsNaN() || Orientation.ContainsNaN()
            || !Orientation.IsNormalized()) return Result;
        const FVector Local = Orientation.UnrotateVector(Position - Viewer);
        if (Local.IsNearlyZero()) return Result;
        const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(Local.Y, Local.X));
        const float Pitch = FMath::RadiansToDegrees(FMath::Atan2(Local.Z, FMath::Sqrt(Local.X * Local.X + Local.Y * Local.Y)));
        const FVector2D Angular(Yaw / 36.f, -Pitch / 20.f);
        const float Extent = FMath::Max(FMath::Abs(Angular.X), FMath::Abs(Angular.Y));
        if (Extent <= 1.f) return Result;
        Result.bOutside = true;
        const FVector2D Edge = Angular / Extent;
        Result.Position = FVector2D(1024.f + Edge.X * 850.f, 500.f + Edge.Y * 210.f);
        Result.Direction = (Result.Position - FVector2D(1024.f, 500.f)).GetSafeNormal();
        return Result;
    }
}
