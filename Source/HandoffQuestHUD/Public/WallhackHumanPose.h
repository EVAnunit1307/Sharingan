#pragma once
#include "CoreMinimal.h"
#include "WallhackPeopleSubsystem.h"

namespace WallhackHumanPose
{
/** Component-space bones in the mesh's centimetres. All limb lengths come from
 * the reference rig; sensor landmarks supply directions, never bone scale. */
HANDOFFQUESTHUD_API TMap<FName,FTransform> Solve(const FWallhackPersonPose& Person,
    const TMap<FName,FTransform>& Reference,double Now);
}
