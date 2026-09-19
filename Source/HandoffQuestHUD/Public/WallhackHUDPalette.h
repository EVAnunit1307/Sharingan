#pragma once

#include "Math/Color.h"

// Saturated green foreground with a dark keyline remains legible over
// bright passthrough scenery as well as dark backgrounds. Warning/error
// colors stay distinct and must not be replaced by this presentation theme.
namespace WallhackHUDPalette
{
    inline const FLinearColor Accent(0.12f, 1.f, 0.005f, 1.f);
    inline const FLinearColor Secondary(0.30f, 1.f, 0.20f, 1.f);
    inline const FLinearColor Panel(0.002f, 0.008f, 0.002f, 0.92f);
}
