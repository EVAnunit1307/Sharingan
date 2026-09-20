#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WallhackPeopleSubsystem.h"
#include "WallhackPeopleRenderer.generated.h"

class UStaticMesh;
class UStaticMeshComponent;
class UProceduralMeshComponent;
class UMaterialInterface;
class UTextureRenderTarget2D;

/** Anatomical world-space meshes. No collision, depth sensing, or navigation obstacles. */
UCLASS()
class HANDOFFQUESTHUD_API AWallhackPeopleRenderer : public AActor
{
    GENERATED_BODY()
public:
    AWallhackPeopleRenderer();
    void Present(const TArray<FWallhackPersonPose>& People, int32 SelectedId,
        const FWallhackPersonPose* Preview, bool bVisible, float WorldToMeters,
        FVector ViewerMeters=FVector::ZeroVector,FQuat ViewOrientation=FQuat::Identity,float NorthOffset=0,double Now=0);
    const TArray<TObjectPtr<UStaticMeshComponent>>& GetBodyComponents() const { return Bodies; }
    const TArray<TObjectPtr<UProceduralMeshComponent>>& GetOutlineComponents() const { return Outlines; }
    const TArray<TObjectPtr<UProceduralMeshComponent>>& GetTelemetryComponents() const { return Telemetry; }
    const TArray<FString>& GetTelemetryText() const { return TelemetryText; }
    const TArray<TObjectPtr<UTextureRenderTarget2D>>& GetTelemetryTextures() const { return TelemetryTextures; }
private:
    UStaticMeshComponent* CreateBody();
    UProceduralMeshComponent* CreateOutline(UStaticMeshComponent* Body);
    UProceduralMeshComponent* CreateTelemetry();
    void BuildTelemetry(int32 Slot,const FWallhackPersonPose& Person,float Distance,float NorthOffset);
    UPROPERTY() TObjectPtr<UStaticMesh> BodyMesh;
    UPROPERTY() TObjectPtr<UMaterialInterface> BodyMaterial;
    UPROPERTY() TObjectPtr<UMaterialInterface> OutlineMaterial;
    UPROPERTY() TObjectPtr<UMaterialInterface> LabelMaterial;
    UPROPERTY(Transient) TArray<TObjectPtr<UStaticMeshComponent>> Bodies;
    UPROPERTY(Transient) TArray<TObjectPtr<UProceduralMeshComponent>> Outlines;
    UPROPERTY(Transient) TArray<TObjectPtr<UProceduralMeshComponent>> Telemetry;
    UPROPERTY(Transient) TArray<TObjectPtr<UTextureRenderTarget2D>> TelemetryTextures;
    TArray<FLinearColor> PresentationColors;
    TArray<FString> TelemetryText;
    TArray<double> TelemetryTimes;
    TArray<int32> TelemetryIds;
};
