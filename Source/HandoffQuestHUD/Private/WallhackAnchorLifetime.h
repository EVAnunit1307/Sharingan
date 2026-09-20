#pragma once

#if PLATFORM_ANDROID
#include "OculusXRAnchorBPFunctionLibrary.h"
#include "OculusXRAnchorComponent.h"
#include "OculusXRAnchors.h"

/** Shared teardown for live sensor registration and synthetic test anchors. */
inline void DestroyWallhackAnchor(UOculusXRAnchorComponent* Anchor)
{
    if (!IsValid(Anchor)) return;
    Anchor->SetComponentTickEnabled(false);
    if (Anchor->HasValidHandle())
    {
        // Late creation may follow owner EndPlay. Release explicitly and clear
        // the handle only on success; component EndPlay can retry a failure.
        EOculusXRAnchorResult::Type Result = EOculusXRAnchorResult::Failure;
        if (OculusXRAnchors::FOculusXRAnchors::DestroyAnchor(Anchor->GetHandle().GetValue(), Result))
            Anchor->SetHandle(FOculusXRUInt64(0));
        else
            UE_LOG(LogTemp, Warning, TEXT("Wallhack anchor release failed: handle=%llu result=%d"),
                Anchor->GetHandle().GetValue(), static_cast<int32>(Result));
    }
    Anchor->DestroyComponent();
}
#endif
