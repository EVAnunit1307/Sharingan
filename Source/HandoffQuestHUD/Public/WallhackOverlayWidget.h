#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "WallhackOverlayWidget.generated.h"

/** A deliberately high-contrast status card that remains visible in the Quest view. */
UCLASS()
class HANDOFFQUESTHUD_API UWallhackOverlayWidget : public UUserWidget
{
    GENERATED_BODY()

protected:
    virtual void NativeConstruct() override;
    virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
    void UpdateMinimap(const struct FWallhackDisplayFrame& Frame);
    FString BuildCompassMarkers(const struct FWallhackDisplayFrame& Frame) const;

    class UTextBlock* HeadingText = nullptr;
    class UTextBlock* CompassText = nullptr;
    class UTextBlock* TimeText = nullptr;
    class UTextBlock* LinkText = nullptr;
    class UTextBlock* ContactText = nullptr;
    class UTextBlock* RigText = nullptr;
    class UTextBlock* PacketAgeText = nullptr;
    class UTextBlock* BatteryText = nullptr;
    class UTextBlock* NetworkText = nullptr;
    class UTextBlock* VideoStateText = nullptr;
    class UTextBlock* MapScaleText = nullptr;
    class UTextBlock* RigMarker = nullptr;
    class UCanvasPanel* MinimapCanvas = nullptr;
    TArray<TObjectPtr<class UTextBlock>> ContactBlips;
};
