#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WallhackNavigationTypes.h"
#include "WallhackNavigationRenderer.generated.h"
class UProceduralMeshComponent;
namespace WallhackNav
{
struct FTrailGeometry
{
    TArray<FVector> Vertices;
    TArray<int32> Indices;
    TArray<FLinearColor> Colors;
};
HANDOFFQUESTHUD_API FTrailGeometry BuildTrail(const FDisplaySnapshot& Display);
HANDOFFQUESTHUD_API FTrailGeometry BuildEstimatedLabel(FVector Position,FVector Viewer);
}
UCLASS()
class HANDOFFQUESTHUD_API AWallhackNavigationRenderer : public AActor
{
    GENERATED_BODY()
public:
    AWallhackNavigationRenderer();
    virtual void Tick(float DeltaTime) override;
    void InvalidateTrail() { Refresh=1; }
private:
    UPROPERTY(VisibleAnywhere) TObjectPtr<UProceduralMeshComponent> Trail;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UProceduralMeshComponent> EstimatedLabel;
    float Refresh=0;
};
