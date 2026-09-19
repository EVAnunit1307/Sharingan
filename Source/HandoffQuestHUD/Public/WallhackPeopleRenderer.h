#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WallhackPeopleSubsystem.h"
#include "WallhackPeopleRenderer.generated.h"

class UStaticMesh;
class UStaticMeshComponent;
class UMaterialInterface;

/** Anatomical world-space meshes. No collision, depth sensing, or navigation obstacles. */
UCLASS()
class HANDOFFQUESTHUD_API AWallhackPeopleRenderer : public AActor
{
    GENERATED_BODY()
public:
    AWallhackPeopleRenderer();
    void Present(const TArray<FWallhackPersonPose>& People, int32 SelectedId,
        const FWallhackPersonPose* Preview, bool bVisible, float WorldToMeters);
    const TArray<TObjectPtr<UStaticMeshComponent>>& GetBodyComponents() const { return Bodies; }
private:
    UStaticMeshComponent* CreateBody();
    UPROPERTY() TObjectPtr<UStaticMesh> BodyMesh;
    UPROPERTY() TObjectPtr<UMaterialInterface> BodyMaterial;
    UPROPERTY(Transient) TArray<TObjectPtr<UStaticMeshComponent>> Bodies;
};
