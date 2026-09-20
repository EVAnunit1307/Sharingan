#pragma once
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "WallhackPeopleSubsystem.generated.h"

class AWallhackPeopleRenderer;

/** A manually supplied person pose in the shared MRUK navigation frame (metres).
 * This is a generic body representation, not recognition or live tracking. */
struct FWallhackPersonPose
{
    int32 Id = INDEX_NONE;
    FVector Feet = FVector::ZeroVector;
    float Height = 1.75f;
    float Facing = 0.f; // World yaw, +X forward. Compass calibration does not alter this.
    FLinearColor Tint = FLinearColor(.32f, .7f, .55f, 1.f);
};

UCLASS()
class HANDOFFQUESTHUD_API UWallhackPeopleSubsystem : public UTickableWorldSubsystem
{
    GENERATED_BODY()
public:
    static constexpr int32 MaxPeople = 8;
    virtual void Tick(float DeltaTime) override;
    virtual void Deinitialize() override;
    virtual bool IsTickable() const override { return !IsTemplate() && (bEditing || People.Num() > 0); }
    virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(UWallhackPeopleSubsystem, STATGROUP_Tickables); }
    void ToggleEditing();
    bool IsEditing() const { return bEditing; }
    bool PlaceFromAim();
    bool MoveSelectedFromAim();
    bool NavigateToSelected();
    // Game-thread pose interface; callers supply foot position, height and yaw independently.
    int32 AddPerson(FVector FeetMeters, float HeightMeters, float FacingDegrees);
    bool UpdatePerson(int32 Id, FVector FeetMeters, float HeightMeters, float FacingDegrees);
    void AdjustSelected(float HeightDelta, float FacingDelta);
    void SelectNext();
    void RemoveSelected();
    const TArray<FWallhackPersonPose>& GetPeople() const { return People; }
    const FWallhackPersonPose* GetSelected() const;
    float GetPlacementHeight() const { return PlacementHeight; }
    float GetPlacementFacing() const { return PlacementFacing; }
    FString GetHint() const;
    void RefreshPresentation();
    AWallhackPeopleRenderer* GetRenderer() const { return Renderer; }
private:
    TArray<FWallhackPersonPose> People;
    int32 SelectedId = INDEX_NONE;
    int32 NextId = 1;
    float PlacementHeight = 1.75f;
    float PlacementFacing = 0.f;
    bool bEditing = false;
    UPROPERTY(Transient) TObjectPtr<AWallhackPeopleRenderer> Renderer;
};
