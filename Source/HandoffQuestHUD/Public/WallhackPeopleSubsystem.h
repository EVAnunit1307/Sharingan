#pragma once
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "WallhackPeopleSubsystem.generated.h"

class AWallhackPeopleRenderer;

/** Renderer pose in the shared world frame (metres). The manual subsystem and
 * live sensor actor keep separate stores; body height/facing may be assumptions. */
struct FWallhackPersonPose
{
    int32 Id = INDEX_NONE;
    FVector Feet = FVector::ZeroVector;
    float Height = 1.75f;
    float Facing = 0.f; // World yaw, +X forward. Compass calibration does not alter this.
    int32 ColorSlot = INDEX_NONE; // Assigned once; unique among active session people.
    FLinearColor Tint = FLinearColor::Transparent; // Optional sensor-source accent.
    FString SourceLabel; // Empty for manual poses; RADAR / ESTIMATED / RADAR ONLY for sensors.
    bool bRadarOnly = false; // Radar IDs use R, independently of the camera's C identities.
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
    void SetNorthReference(float Offset) { NorthReference=Offset; }
    AWallhackPeopleRenderer* GetRenderer() const { return Renderer; }
private:
    TArray<FWallhackPersonPose> People;
    int32 SelectedId = INDEX_NONE;
    int32 NextId = 1;
    float PlacementHeight = 1.75f;
    float PlacementFacing = 0.f;
    bool bEditing = false;
    float NorthReference=0;
    double PresentationTime=0;
    UPROPERTY(Transient) TObjectPtr<AWallhackPeopleRenderer> Renderer;
};
