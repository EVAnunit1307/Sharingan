#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "WallhackHUDSettings.generated.h"

/** Deliberately-visible controls for the unverified ARKit-to-fusion bearing convention. */
UCLASS(Config=Game, DefaultConfig, meta=(DisplayName="Wallhack HUD"))
class HANDOFFQUESTHUD_API UWallhackHUDSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    /**
     * Plain WebSocket endpoint served by ground/bridge.py. Change this per
     * rig/network.
     *
     * 5 Sept: switched from 192.168.2.185 (larp-pi's IP, the Raspberry Pi)
     * to 192.168.2.28 -- the Windows laptop currently running this Unreal
     * project ("flamingfist") -- so it can act as ground station for
     * testing. This only repoints where the Quest app looks for the bridge;
     * it doesn't by itself make bridge.py run on the laptop. Something still
     * has to run ground/bridge.py on 192.168.2.28:8765 with the flight
     * controller's serial/radio link plugged into that machine, or this
     * will just sit at "ACQUIRING" forever like it does with no bridge at
     * all. Revert to the Pi's IP (or whichever machine ends up wired to the
     * FC) once the rig layout is settled.
     */
    UPROPERTY(Config, EditAnywhere, Category="Connection")
    FString BridgeUrl = TEXT("ws://192.168.2.28:8765/");

    /** Contacts are never rendered after this interval without a fresh packet. */
    UPROPERTY(Config, EditAnywhere, Category="Safety", meta=(ClampMin="0.1", ClampMax="10.0"))
    float StaleAfterSeconds = 1.0f;

    /** Flip this after live testing if bearings are mirrored. */
    UPROPERTY(Config, EditAnywhere, Category="Bearing Convention")
    bool bInvertBearing = false;

    /** Swap dx and dy after live testing if the axes are rotated. */
    UPROPERTY(Config, EditAnywhere, Category="Bearing Convention")
    bool bSwapBearingAxes = false;

    /** Operational trim; leave at zero until a measured calibration calls for it. */
    UPROPERTY(Config, EditAnywhere, Category="Bearing Convention", meta=(ClampMin="-180.0", ClampMax="180.0"))
    float BearingOffsetDegrees = 0.0f;

    /**
     * Horizontal field of view the contact sensor actually covers, centered on
     * the rig's forward heading. 360 means true all-around coverage (e.g. a
     * spinning scanner). Anything less draws the tactical map as a wedge
     * instead of a full circle, because a full 360 ring implies omnidirectional
     * coverage the sensor may not have -- exactly the kind of instrument
     * dishonesty this HUD otherwise refuses (see the stale-data gate in
     * GetDisplayFrame()).
     *
     * Set to 62 deg: the published horizontal FOV of the IMX219 (stock
     * Raspberry Pi Camera Module v2), which is what's actually mounted and
     * live-tested as of this build. NOT the previous 120 deg placeholder,
     * and NOT the target Arducam OV9281's ~80 deg horizontal FOV either
     * (89.5 deg is that sensor's *diagonal* FOV at 1280x800 -- see
     * WALLHACK.md's "Sensing" section for the f=762px derivation; 89.5 deg
     * diagonal works out to ~80 deg horizontal, not the same number).
     * If the rig swaps to the OV9281 before the event, change this to 80,
     * not 89.5 -- and re-derive/re-measure rather than trusting either
     * number blindly once real hardware is in hand.
     */
    UPROPERTY(Config, EditAnywhere, Category="Sensor", meta=(ClampMin="10.0", ClampMax="360.0"))
    float SensorFOVDegrees = 62.0f;
};
