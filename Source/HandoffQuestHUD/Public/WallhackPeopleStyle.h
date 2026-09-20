#pragma once
#include "CoreMinimal.h"
#include "WallhackPeopleSubsystem.h"

namespace WallhackPeopleStyle
{
// Identity colors, not classifications. Numeric IDs remain visible as well.
inline FLinearColor Color(const FWallhackPersonPose& Person)
{
    static const FColor Palette[] = {
        {241,92,92}, {88,186,244}, {181,137,250}, {247,203,97},
        {71,216,194}, {245,142,202}, {170,213,90}, {250,156,80}
    };
    if(Person.ColorSlot==INDEX_NONE&&Person.Id==INDEX_NONE)return FLinearColor(.72,.78,.80);
    const int32 Slot=Person.ColorSlot!=INDEX_NONE?Person.ColorSlot:FMath::Max(Person.Id-1,0);
    return FLinearColor(Palette[Slot%UE_ARRAY_COUNT(Palette)]);
}
}
