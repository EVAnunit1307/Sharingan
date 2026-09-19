#include "WallhackPeopleRenderer.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"

AWallhackPeopleRenderer::AWallhackPeopleRenderer()
{
    PrimaryActorTick.bCanEverTick = false;
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Mesh(TEXT("/Game/People/SM_HumanSilhouette.SM_HumanSilhouette"));
    static ConstructorHelpers::FObjectFinder<UMaterialInterface> Material(TEXT("/Game/Materials/M_HumanSilhouette.M_HumanSilhouette"));
    BodyMesh = Mesh.Object;
    BodyMaterial = Material.Object;
}

UStaticMeshComponent* AWallhackPeopleRenderer::CreateBody()
{
    auto* Body = NewObject<UStaticMeshComponent>(this);
    AddInstanceComponent(Body);
    Body->SetupAttachment(GetRootComponent());
    Body->SetMobility(EComponentMobility::Movable);
    Body->SetStaticMesh(BodyMesh);
    Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Body->SetGenerateOverlapEvents(false);
    Body->SetCanEverAffectNavigation(false);
    Body->SetCastShadow(false);
    Body->bReceivesDecals = false;
    Body->SetMaterial(0, UMaterialInstanceDynamic::Create(BodyMaterial, this));
    Body->RegisterComponent();
    return Body;
}

void AWallhackPeopleRenderer::Present(const TArray<FWallhackPersonPose>& People, int32 SelectedId,
    const FWallhackPersonPose* Preview, bool bVisible, float WorldToMeters)
{
    SetActorHiddenInGame(!bVisible);
    if (!bVisible || !BodyMesh || !FMath::IsFinite(WorldToMeters) || WorldToMeters <= 0) return;
    const int32 Count = People.Num() + (Preview ? 1 : 0);
    while (Bodies.Num() < Count) Bodies.Add(CreateBody());
    const float MeshHeight = BodyMesh->GetBoundingBox().GetSize().Z;
    if (MeshHeight <= 0) return;
    for (int32 I = 0; I < Bodies.Num(); ++I)
    {
        auto* Body = Bodies[I].Get();
        Body->SetHiddenInGame(I >= Count);
        if (I >= Count) continue;
        const bool bPreview = I == People.Num();
        const auto& Person = bPreview ? *Preview : People[I];
        // Uniform scaling preserves head, hand and limb proportions at every height.
        const FTransform Pose(FRotator(0, Person.Facing, 0), Person.Feet * WorldToMeters,
            FVector(Person.Height * WorldToMeters / MeshHeight));
        if (!Body->GetComponentTransform().Equals(Pose)) Body->SetWorldTransform(Pose);
        auto* Material = Cast<UMaterialInstanceDynamic>(Body->GetMaterial(0));
        if (Material) Material->SetScalarParameterValue(TEXT("Opacity"), bPreview ? .12f : Person.Id == SelectedId ? .26f : .16f);
    }
}
