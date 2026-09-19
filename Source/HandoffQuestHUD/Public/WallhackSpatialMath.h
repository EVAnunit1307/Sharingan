#pragma once

#include "CoreMinimal.h"

/** Coordinate conversions shared by the world marker and the heading-up minimap. */
namespace WallhackSpatialMath
{
    struct FContactView
    {
        float ForwardMeters = 0.f;
        float RightMeters = 0.f;
        float HeightMeters = 0.f;
        float GroundRangeMeters = 0.f;
        float RangeMeters = 0.f;
        float BearingDegrees = 0.f;
    };

    inline bool IsFiniteVector(const FVector& Value)
    {
        return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z);
    }

    /**
     * Project a fixed world contact into the viewer's horizontal frame, in metres.
     * UE +X is forward, +Y is right; bearing is positive to the viewer's right.
     * ViewerYawDegrees is the camera's WORLD yaw, never a calibrated compass label.
     * Pitch and roll do not rotate the ground map. Invalid data clears Out.
     */
    inline bool ProjectContact(const FVector& ContactWorld, const FVector& ViewerWorld,
        float ViewerYawDegrees, float WorldToMeters, FContactView& Out)
    {
        Out = FContactView();
        if (!IsFiniteVector(ContactWorld) || !IsFiniteVector(ViewerWorld)
            || !FMath::IsFinite(ViewerYawDegrees) || !FMath::IsFinite(WorldToMeters)
            || WorldToMeters <= 0.f)
        {
            return false;
        }

        const double DeltaX = (ContactWorld.X - ViewerWorld.X) / static_cast<double>(WorldToMeters);
        const double DeltaY = (ContactWorld.Y - ViewerWorld.Y) / static_cast<double>(WorldToMeters);
        const double DeltaZ = (ContactWorld.Z - ViewerWorld.Z) / static_cast<double>(WorldToMeters);
        if (!FMath::IsFinite(DeltaX) || !FMath::IsFinite(DeltaY) || !FMath::IsFinite(DeltaZ))
        {
            return false;
        }

        float SinYaw = 0.f;
        float CosYaw = 1.f;
        FMath::SinCos(&SinYaw, &CosYaw,
            FMath::DegreesToRadians(FMath::Fmod(ViewerYawDegrees, 360.f)));

        FContactView Result;
        Result.ForwardMeters = static_cast<float>(DeltaX * CosYaw + DeltaY * SinYaw);
        Result.RightMeters = static_cast<float>(-DeltaX * SinYaw + DeltaY * CosYaw);
        Result.HeightMeters = static_cast<float>(DeltaZ);
        const double GroundRangeSquared = DeltaX * DeltaX + DeltaY * DeltaY;
        Result.GroundRangeMeters = static_cast<float>(FMath::Sqrt(GroundRangeSquared));
        Result.RangeMeters = static_cast<float>(FMath::Sqrt(GroundRangeSquared + DeltaZ * DeltaZ));
        Result.BearingDegrees = FMath::RadiansToDegrees(FMath::Atan2(Result.RightMeters, Result.ForwardMeters));

        if (!FMath::IsFinite(Result.ForwardMeters) || !FMath::IsFinite(Result.RightMeters)
            || !FMath::IsFinite(Result.HeightMeters) || !FMath::IsFinite(Result.GroundRangeMeters)
            || !FMath::IsFinite(Result.RangeMeters) || !FMath::IsFinite(Result.BearingDegrees))
        {
            return false;
        }

        Out = Result;
        return true;
    }

    /** Pixel offset from the map centre: right is +X and forward is -Y. */
    inline FVector2D MapOffset(const FContactView& Contact, float MapRadiusPixels, float MapRangeMeters)
    {
        if (!FMath::IsFinite(MapRadiusPixels) || !FMath::IsFinite(MapRangeMeters)
            || MapRadiusPixels <= 0.f || MapRangeMeters <= 0.f
            || !FMath::IsFinite(Contact.ForwardMeters) || !FMath::IsFinite(Contact.RightMeters))
        {
            return FVector2D::ZeroVector;
        }

        const FVector2D Direction(Contact.RightMeters, -Contact.ForwardMeters);
        const double GroundRange = Direction.Size();
        if (GroundRange <= 0.)
        {
            return FVector2D::ZeroVector;
        }

        // Clamp in metres first so distant contacts preserve their exact bearing.
        const double Denominator = FMath::Max(GroundRange, static_cast<double>(MapRangeMeters));
        return Direction * (static_cast<double>(MapRadiusPixels) / Denominator);
    }

    /**
     * Place a contact at eye height along the normalized horizontal look direction.
     * Call once to seed/reseed an anchor; calling every frame would make it head-locked.
     * If looking exactly vertical, infer horizontal heading from the camera's right axis.
     * Invalid data leaves the anchor at the valid viewer position, or at zero.
     */
    inline FVector PlaceAhead(const FVector& ViewerWorld, const FQuat& ViewerOrientation,
        float RangeMeters = 3.f, float WorldToMeters = 100.f)
    {
        if (!IsFiniteVector(ViewerWorld))
        {
            return FVector::ZeroVector;
        }
        if (!FMath::IsFinite(RangeMeters) || RangeMeters < 0.f
            || !FMath::IsFinite(WorldToMeters) || WorldToMeters <= 0.f
            || ViewerOrientation.ContainsNaN()
            || !FMath::IsFinite(ViewerOrientation.SizeSquared())
            || ViewerOrientation.SizeSquared() <= UE_SMALL_NUMBER)
        {
            return ViewerWorld;
        }

        const FQuat Orientation = ViewerOrientation.GetNormalized();
        FVector Heading = Orientation.RotateVector(FVector::ForwardVector);
        Heading.Z = 0.;
        if (!Heading.Normalize())
        {
            FVector Right = Orientation.RotateVector(FVector::RightVector);
            Right.Z = 0.;
            Heading = FVector::CrossProduct(Right, FVector::UpVector).GetSafeNormal();
            if (Heading.IsNearlyZero())
            {
                Heading = FVector::ForwardVector;
            }
        }

        const FVector Result = ViewerWorld + Heading * (static_cast<double>(RangeMeters) * WorldToMeters);
        return IsFiniteVector(Result) ? Result : ViewerWorld;
    }
}
