#include "WallhackPeopleSubsystem.h"
#include "WallhackPeopleRenderer.h"
#include "WallhackNavigationSubsystem.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"

namespace
{
bool ValidPose(FVector Feet, float Height, float Facing)
{
    return !Feet.ContainsNaN() && FMath::IsFinite(Height) && FMath::IsFinite(Facing);
}
float ClampedHeight(float Height) { return FMath::Clamp(Height, 1.f, 2.3f); }
}

void UWallhackPeopleSubsystem::Tick(float) { RefreshPresentation(); }
void UWallhackPeopleSubsystem::Deinitialize()
{
    if (Renderer) Renderer->Destroy();
    Renderer = nullptr;
    People.Reset();
    Super::Deinitialize();
}
const FWallhackPersonPose* UWallhackPeopleSubsystem::GetSelected() const
{
    return People.FindByPredicate([&](const FWallhackPersonPose& P) { return P.Id == SelectedId; });
}
void UWallhackPeopleSubsystem::ToggleEditing()
{
    bEditing = !bEditing;
    auto* Nav = GetWorld()->GetSubsystem<UWallhackNavigationSubsystem>();
    Nav->EndAim(); // A held navigation preview must not accidentally place a person on mode change.
    if (bEditing && !GetSelected()) PlacementFacing = FRotator::ClampAxis(Nav->GetDisplaySnapshot().Orientation.Rotator().Yaw + 180.f);
    RefreshPresentation();
    UE_LOG(LogTemp, Display, TEXT("Wallhack people: edit=%d count=%d"), bEditing, People.Num());
}
int32 UWallhackPeopleSubsystem::AddPerson(FVector Feet, float Height, float Facing)
{
    if (!ValidPose(Feet, Height, Facing) || People.Num() >= MaxPeople) return INDEX_NONE;
    const int32 Id = NextId++;
    People.Add({Id, Feet, ClampedHeight(Height), float(FRotator::ClampAxis(Facing))});
    SelectedId = Id;
    PlacementHeight = People.Last().Height;
    PlacementFacing = People.Last().Facing;
    RefreshPresentation();
    UE_LOG(LogTemp, Display, TEXT("Wallhack people: placed id=%d feet=%s height=%.2fm yaw=%.1f"), Id, *Feet.ToString(), PlacementHeight, PlacementFacing);
    return Id;
}
bool UWallhackPeopleSubsystem::UpdatePerson(int32 Id, FVector Feet, float Height, float Facing)
{
    if (!ValidPose(Feet, Height, Facing)) return false;
    auto* Person = People.FindByPredicate([&](const FWallhackPersonPose& P) { return P.Id == Id; });
    if (!Person) return false;
    Person->Feet = Feet;
    Person->Height = ClampedHeight(Height);
    Person->Facing = FRotator::ClampAxis(Facing);
    if (SelectedId == Id) { PlacementHeight = Person->Height; PlacementFacing = Person->Facing; }
    auto* Nav = GetWorld()->GetSubsystem<UWallhackNavigationSubsystem>();
    if (Nav->GetDisplaySnapshot().Target.PersonId == Id) Nav->SetPersonDestination(Id, Feet);
    RefreshPresentation();
    return true;
}
bool UWallhackPeopleSubsystem::PlaceFromAim()
{
    const auto& D = GetWorld()->GetSubsystem<UWallhackNavigationSubsystem>()->GetDisplaySnapshot();
    if (!bEditing || !D.bGuidance || D.bHidden || !D.bAiming || !D.bPreviewValid) return false;
    // Preview.Standing is floor-supported even if the ray hits an object above it.
    if (AddPerson(D.Preview.Standing, PlacementHeight, PlacementFacing) == INDEX_NONE) return false;
    return NavigateToSelected();
}
bool UWallhackPeopleSubsystem::NavigateToSelected()
{
    const auto* Person = GetSelected();
    auto* Nav = GetWorld()->GetSubsystem<UWallhackNavigationSubsystem>();
    const auto& D = Nav->GetDisplaySnapshot();
    if (!Person || !D.bGuidance || D.bHidden) return false;
    return Nav->SetPersonDestination(Person->Id, Person->Feet);
}
bool UWallhackPeopleSubsystem::MoveSelectedFromAim()
{
    const auto& D = GetWorld()->GetSubsystem<UWallhackNavigationSubsystem>()->GetDisplaySnapshot();
    const auto* P = GetSelected();
    if (!bEditing || !P || !D.bGuidance || D.bHidden || !D.bAiming || !D.bPreviewValid) return false;
    if (!UpdatePerson(P->Id, D.Preview.Standing, P->Height, P->Facing)) return false;
    return NavigateToSelected();
}
void UWallhackPeopleSubsystem::AdjustSelected(float HeightDelta, float FacingDelta)
{
    if (!bEditing || !FMath::IsFinite(HeightDelta) || !FMath::IsFinite(FacingDelta)) return;
    const auto& D = GetWorld()->GetSubsystem<UWallhackNavigationSubsystem>()->GetDisplaySnapshot();
    if (D.bHidden || !D.bGuidance) return;
    PlacementHeight = ClampedHeight(PlacementHeight + HeightDelta);
    PlacementFacing = FRotator::ClampAxis(PlacementFacing + FacingDelta);
    if (const auto* P = GetSelected()) UpdatePerson(P->Id, P->Feet, PlacementHeight, PlacementFacing);
    else RefreshPresentation();
}
void UWallhackPeopleSubsystem::SelectNext()
{
    if (People.IsEmpty()) return;
    const int32 Index = People.IndexOfByPredicate([&](const FWallhackPersonPose& P) { return P.Id == SelectedId; });
    const auto& P = People[(Index + 1) % People.Num()];
    SelectedId = P.Id;
    PlacementHeight = P.Height;
    PlacementFacing = P.Facing;
    NavigateToSelected();
    RefreshPresentation();
}
void UWallhackPeopleSubsystem::RemoveSelected()
{
    if (!bEditing) return;
    auto* Nav = GetWorld()->GetSubsystem<UWallhackNavigationSubsystem>();
    if (SelectedId != INDEX_NONE && Nav->GetDisplaySnapshot().Target.PersonId == SelectedId) Nav->CancelNavigation();
    People.RemoveAll([&](const FWallhackPersonPose& P) { return P.Id == SelectedId; });
    SelectedId = INDEX_NONE;
    SelectNext();
    RefreshPresentation();
}
FString UWallhackPeopleSubsystem::GetHint() const
{
    const auto* Nav = GetWorld()->GetSubsystem<UWallhackNavigationSubsystem>();
    const auto& D = Nav->GetDisplaySnapshot();
    if (!D.bGuidance || (D.bAiming && !D.bPreviewValid)) return Nav->GetInteractionHint();
    if (D.bAiming && People.Num() >= MaxPeople) return TEXT("8 PEOPLE / A REMOVES SELECTED");
    return D.bAiming ? TEXT("TRIGGER / PLACE + NAVIGATE") : GetSelected() ? TEXT("TRIGGER / NAVIGATE TO PERSON") : TEXT("HOLD GRIP / POINT AT THEIR FEET");
}
void UWallhackPeopleSubsystem::RefreshPresentation()
{
    auto* Nav = GetWorld()->GetSubsystem<UWallhackNavigationSubsystem>();
    if (!Nav) return;
    const auto& D = Nav->GetDisplaySnapshot();
    const bool bVisible = D.bGuidance && !D.bHidden;
    FWallhackPersonPose Preview{INDEX_NONE, D.Preview.Standing, PlacementHeight, PlacementFacing};
    const bool bPreview = bEditing && D.bAiming && D.bPreviewValid && People.Num() < MaxPeople;
    if (!Renderer && bVisible && (People.Num() > 0 || bPreview)) Renderer = GetWorld()->SpawnActor<AWallhackPeopleRenderer>();
    if (Renderer) Renderer->Present(People, bEditing ? SelectedId : D.Target.PersonId,
        bPreview ? &Preview : nullptr, bVisible, GetWorld()->GetWorldSettings()->WorldToMeters);
}
