#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "WallhackTelemetrySubsystem.h"
#include "WallhackHUD.generated.h"

UCLASS()
class HANDOFFQUESTHUD_API AWallhackHUD : public AHUD
{
    GENERATED_BODY()

public:
    virtual void DrawHUD() override;

private:
    void DrawDesktopOperatorPreview(const FWallhackDisplayFrame& Frame, float Width, float Height);
    void DrawCompass(const FWallhackDisplayFrame& Frame, float Width, float Height);
    void DrawContactTag(const FWallhackContact& Contact, float X, float Y, float Size);
    void DrawCornerBrackets(float Left, float Top, float Size, const FLinearColor& Color);
    void DrawStickFigure(float X, float Y, float Scale, const FLinearColor& Color);
    static FLinearColor ColorFor(EWallhackContactState State);
    static FString LinkText(EWallhackLinkState State);
};
