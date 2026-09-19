#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "WallhackGameMode.generated.h"

class AWallhackVRHUDActor;
class AWallhackWorldContact;

UCLASS()
class HANDOFFQUESTHUD_API AWallhackGameMode : public AGameModeBase
{
    GENERATED_BODY()
public:
    AWallhackGameMode();
    virtual void BeginPlay() override;

private:
    UPROPERTY(Transient)
    TObjectPtr<AWallhackVRHUDActor> VRHUDActor;

    UPROPERTY(Transient)
    TObjectPtr<AWallhackWorldContact> WorldContact;
};
