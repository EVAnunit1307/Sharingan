#include "WallhackPeopleRenderer.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ProceduralMeshComponent.h"
#include "WallhackPeopleStyle.h"
#include "CanvasTypes.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "UObject/ConstructorHelpers.h"
#include "Components/PoseableMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "WallhackHumanPose.h"

AWallhackPeopleRenderer::AWallhackPeopleRenderer()
{
    PrimaryActorTick.bCanEverTick = false;
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Mesh(TEXT("/Game/People/SM_HumanSilhouette.SM_HumanSilhouette"));
    static ConstructorHelpers::FObjectFinder<UMaterialInterface> Material(TEXT("/Game/Materials/M_HumanSilhouette.M_HumanSilhouette"));
    static ConstructorHelpers::FObjectFinder<UMaterialInterface> Outline(TEXT("/Game/Materials/M_WallhackTrail.M_WallhackTrail"));
    BodyMesh = Mesh.Object;
    BodyMaterial = Material.Object;
    OutlineMaterial = Outline.Object;
    static ConstructorHelpers::FObjectFinder<UMaterialInterface> Label(TEXT("/Game/Materials/M_PersonLabel.M_PersonLabel"));
    LabelMaterial = Label.Object;
    static ConstructorHelpers::FObjectFinder<USkeletalMesh> Rig(TEXT("/Game/People/SK_HumanSilhouette.SK_HumanSilhouette"));
    ArticulatedMesh=Rig.Object;
}

bool AWallhackPeopleRenderer::PrepareArticulatedAsset(USkeletalMesh* Mesh)
{
#if WITH_EDITOR
    if(!Mesh||Mesh->GetLODNum()<3)return false;
    Mesh->Modify();
    for(int32 I=0;I<Mesh->GetLODNum();++I)Mesh->GetLODInfo(I)->bAllowCPUAccess=true;
    Mesh->SetSupportLODStreaming(FPerPlatformBool(false));
    Mesh->PostEditChange();Mesh->MarkPackageDirty();
    return true;
#else
    return false;
#endif
}

UPoseableMeshComponent* AWallhackPeopleRenderer::CreateArticulatedBody()
{
    auto* Body=NewObject<UPoseableMeshComponent>(this);AddInstanceComponent(Body);
    Body->SetupAttachment(GetRootComponent());Body->SetMobility(EComponentMobility::Movable);
    if(ArticulatedMesh&&ArticulatedMesh->GetSkeleton())Body->SetSkinnedAssetAndUpdate(ArticulatedMesh);
    Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);Body->SetGenerateOverlapEvents(false);
    Body->SetCanEverAffectNavigation(false);Body->SetCastShadow(false);Body->bReceivesDecals=false;
    Body->SetBoundsScale(2.f); // Raised arms must not be culled by the bind-pose box.
    // The configured GPU skin path fails the isolated paired-eye render check.
    // Use the CPU skin path, capped at LOD1 (3,711 vertices per person).
    // CPU buffers are retained explicitly by import_articulated_human.py.
    if(Body->GetSkinnedAsset())
    {
        Body->OverrideMinLOD(1);
        Body->SetCPUSkinningEnabled(true,false);
    }
    auto* Material=UMaterialInstanceDynamic::Create(BodyMaterial,this);
    // FBX section material indices need not start at zero. Share one identity
    // instance across every slot, including preserved/reimported slot mappings.
    for(int32 I=0;I<FMath::Max(1,Body->GetNumMaterials());++I)Body->SetMaterial(I,Material);
    Body->RegisterComponent();
    // Keep the component's normal tick for LOD and render-state maintenance.
    // Joint targets are supplied together with the sensor actor below.
    if(ReferenceBones.IsEmpty()&&ArticulatedMesh)
    {
        const auto& Skeleton=ArticulatedMesh->GetRefSkeleton();TArray<FTransform> Components;
        for(int32 I=0;I<Skeleton.GetNum();++I)
        {
            FTransform T=Skeleton.GetRefBonePose()[I];const int32 Parent=Skeleton.GetParentIndex(I);
            if(Parent>=0)T=T*Components[Parent];
            Components.Add(T);ReferenceBones.Add(Skeleton.GetBoneName(I),T);
        }
    }
    return Body;
}

FBox AWallhackPeopleRenderer::UpdateArticulated(int32 Slot,const FWallhackPersonPose& Person,const FTransform& Pose,double Now)
{
    auto* Body=ArticulatedBodies[Slot].Get();Body->SetWorldTransform(Pose);
    const auto Solved=WallhackHumanPose::Solve(Person,ReferenceBones,Now);
    auto& Smooth=SmoothedBones[Slot];
    const bool Reset=BoneIds[Slot]!=Person.Id||Now<BoneTimes[Slot]||Now-BoneTimes[Slot]>.25;
    const float Alpha=Reset?1.f:1.f-FMath::Exp(-FMath::Max(0.,Now-BoneTimes[Slot])/.08);
    BoneTimes[Slot]=Now;BoneIds[Slot]=Person.Id;
    FBox Bounds(ForceInit);
    const auto& Skeleton=ArticulatedMesh->GetRefSkeleton();
    // Parent-first component transforms keep PoseableMesh's local conversions coherent.
    for(int32 I=0;I<Skeleton.GetNum();++I)
    {
        const FName Name=Skeleton.GetBoneName(I);const FTransform* Target=Solved.Find(Name);if(!Target)continue;
        FTransform T=*Target;
        if(!Reset)if(const auto* Previous=Smooth.Find(Name))
        {
            T.SetRotation(FQuat::Slerp(Previous->GetRotation(),T.GetRotation(),Alpha).GetNormalized());
            T.SetLocation(FMath::Lerp(Previous->GetLocation(),T.GetLocation(),Alpha));
        }
        Smooth.Add(Name,T);Body->SetBoneTransformByName(Name,T,EBoneSpaces::ComponentSpace);
        if(Name!=TEXT("root")&&Name!=TEXT("HumanSkeleton"))Bounds+=T.GetLocation();
    }
    Body->RefreshBoneTransforms();
    if(!Bounds.IsValid)return BodyMesh->GetBoundingBox();
    Bounds.Min-=FVector(6,6,5);Bounds.Max+=FVector(6,6,10);Bounds.Min.Z=FMath::Max(-1.,Bounds.Min.Z);
    return Bounds;
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

UProceduralMeshComponent* AWallhackPeopleRenderer::CreateOutline(UStaticMeshComponent* Body)
{
    auto* Outline = NewObject<UProceduralMeshComponent>(this);
    AddInstanceComponent(Outline);
    Outline->SetupAttachment(Body);
    Outline->SetMobility(EComponentMobility::Movable);
    Outline->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Outline->SetGenerateOverlapEvents(false);
    Outline->SetCanEverAffectNavigation(false);
    Outline->SetCastShadow(false);
    Outline->bReceivesDecals = false;
    Outline->SetMaterial(0, OutlineMaterial);
    Outline->RegisterComponent();

    // Local mesh bounds keep the box aligned with height, feet and facing.
    // Twelve slim square rods remain visible from every angle, with no faces
    // filling the box and no per-frame mesh rebuilds.
    const FBox Bounds = BodyMesh->GetBoundingBox();
    const float Height = Bounds.GetSize().Z;
    const float Padding = Height * .025f, Radius = Height * .0015f;
    const FVector Min = Bounds.Min - FVector(Padding, Padding, -Radius);
    const FVector Max = Bounds.Max + FVector(Padding);
    FVector Corners[8];
    for (int32 I = 0; I < 8; ++I)
        Corners[I] = FVector(I & 1 ? Max.X : Min.X, I & 2 ? Max.Y : Min.Y, I & 4 ? Max.Z : Min.Z);
    TArray<FVector> Vertices;
    TArray<int32> Indices;
    TArray<FLinearColor> Colors;
    static constexpr int32 Faces[] = {0,2,1,0,3,2,4,5,6,4,6,7,0,1,5,0,5,4,1,2,6,1,6,5,2,3,7,2,7,6,3,0,4,3,4,7};
    for (int32 I = 0; I < 8; ++I) for (int32 Axis = 0; Axis < 3; ++Axis)
    {
        const int32 Mask = 1 << Axis;
        if (I & Mask) continue;
        const FVector A = Corners[I], B = Corners[I | Mask], Direction = (B - A).GetSafeNormal();
        const FVector U = FVector::CrossProduct(Direction, Axis == 2 ? FVector::ForwardVector : FVector::UpVector).GetSafeNormal() * Radius;
        const FVector V = FVector::CrossProduct(Direction, U).GetSafeNormal() * Radius;
        const int32 Base = Vertices.Num();
        Vertices.Append({A-U-V, A+U-V, A+U+V, A-U+V, B-U-V, B+U-V, B+U+V, B-U+V});
        for (int32 Index : Faces) Indices.Add(Base + Index);
        for (int32 J = 0; J < 8; ++J) Colors.Add(FLinearColor(.85f, .035f, .025f, .38f));
    }
    Outline->CreateMeshSection_LinearColor(0, Vertices, Indices, {}, {}, Colors, {}, false);
    return Outline;
}

void AWallhackPeopleRenderer::Present(const TArray<FWallhackPersonPose>& People, int32 SelectedId,
    const FWallhackPersonPose* Preview, bool bVisible, float WorldToMeters,
    FVector ViewerMeters,FQuat ViewOrientation,float NorthOffset,double Now)
{
    SetActorHiddenInGame(!bVisible);
    if (!bVisible || !BodyMesh || !FMath::IsFinite(WorldToMeters) || WorldToMeters <= 0) return;
    const int32 Count = People.Num() + (Preview ? 1 : 0);
    while (Bodies.Num() < Count)
    {
        auto* Body = CreateBody();
        Bodies.Add(Body);
        ArticulatedBodies.Add(CreateArticulatedBody());
        SmoothedBones.Add({});BoneTimes.Add(-1);BoneIds.Add(INDEX_NONE);
        Outlines.Add(CreateOutline(Body));
        Telemetry.Add(CreateTelemetry());
        PresentationColors.Add(FLinearColor::Transparent);
        TelemetryText.Add(FString());TelemetryTimes.Add(-100);TelemetryIds.Add(INDEX_NONE);
    }
    const float MeshHeight = BodyMesh->GetBoundingBox().GetSize().Z;
    if (MeshHeight <= 0) return;
    for (int32 I = 0; I < Bodies.Num(); ++I)
    {
        auto* Body = Bodies[I].Get();
        const bool PoseChanged=I<Count&&TelemetryIds[I]!=(I<People.Num()?People[I].Id:INDEX_NONE);
        Outlines[I]->SetHiddenInGame(I>=Count);
        Telemetry[I]->SetHiddenInGame(I>=People.Num()); // Placement previews have no person ID yet.
        if (I >= Count)
        {
            Body->SetHiddenInGame(true,false);
            ArticulatedBodies[I]->SetHiddenInGame(true);
            continue;
        }
        const bool bPreview = I == People.Num();
        const auto& Person = bPreview ? *Preview : People[I];
        const bool bUseArticulated=Person.bArticulated&&ArticulatedMesh&&ArticulatedMesh->GetSkeleton();
        // A hide/show pair recreates the skeletal render object every frame and
        // discards its bone upload. Apply each final visibility state only once.
        Body->SetHiddenInGame(bUseArticulated,false);
        ArticulatedBodies[I]->SetHiddenInGame(!bUseArticulated);
        const FLinearColor IdentityColor=WallhackPeopleStyle::Color(Person);
        if(PresentationColors[I]!=IdentityColor)
        {
            PresentationColors[I]=IdentityColor;
            TArray<FVector> Vertices;
            for(const auto& V:Outlines[I]->GetProcMeshSection(0)->ProcVertexBuffer)Vertices.Add(V.Position);
            TArray<FLinearColor> Colors;
            Colors.Init(FLinearColor(IdentityColor.R,IdentityColor.G,IdentityColor.B,.48f),
                Outlines[I]->GetProcMeshSection(0)->ProcVertexBuffer.Num());
            Outlines[I]->UpdateMeshSection_LinearColor(0,Vertices, {}, {},Colors,{},false);
            TelemetryText[I].Reset(); // Pooled slots must not retain the previous person's accent.
        }
        // Uniform scaling preserves head, hand and limb proportions at every height.
        const FTransform Pose(FRotator(0, Person.Facing, 0), Person.Feet * WorldToMeters,
            FVector(Person.Height * WorldToMeters / MeshHeight));
        const bool Moved=!Body->GetComponentTransform().Equals(Pose);
        if (Moved) Body->SetWorldTransform(Pose);
        FBox LocalBounds=BodyMesh->GetBoundingBox();
        if(bUseArticulated)
        {
            LocalBounds=UpdateArticulated(I,Person,Pose,Now);
            if(auto* Skin=Cast<UMaterialInstanceDynamic>(ArticulatedBodies[I]->GetMaterial(0)))
            {Skin->SetVectorParameterValue(TEXT("Tint"),IdentityColor);Skin->SetScalarParameterValue(TEXT("Opacity"),.30f);}
            // Adapt the twelve existing outline rods to the articulated bounds.
            const FBox Base=BodyMesh->GetBoundingBox();const float Padding=MeshHeight*.025f;
            const FVector BaseMin=Base.Min-FVector(Padding,Padding,-MeshHeight*.0015f);
            const FVector BaseMax=Base.Max+FVector(Padding);
            const FVector NewMin=LocalBounds.Min-FVector(Padding),NewMax=LocalBounds.Max+FVector(Padding);
            // Keep rod geometry in its original coordinates; the outline's local
            // transform changes the bounds without accumulating vertex edits.
            const FVector Scale=(NewMax-NewMin)/(BaseMax-BaseMin);
            Outlines[I]->SetRelativeTransform(FTransform(FQuat::Identity,NewMin-BaseMin*Scale,Scale));
        }
        else Outlines[I]->SetRelativeTransform(FTransform::Identity);
        auto* Material = Cast<UMaterialInstanceDynamic>(Body->GetMaterial(0));
        if (Material)
        {
            Material->SetVectorParameterValue(TEXT("Tint"), IdentityColor);
            Material->SetScalarParameterValue(TEXT("Opacity"), bPreview ? .18f : Person.Id == SelectedId ? .36f : .24f);
        }
        if(!bPreview)
        {
            const float Distance=Person.SourceLabel.IsEmpty()?FVector::Dist(ViewerMeters,Person.Feet):FVector::Dist2D(ViewerMeters,Person.Feet);
            if(PoseChanged||Moved||TelemetryText[I].IsEmpty()||Now-TelemetryTimes[I]>=.2)
            {
                BuildTelemetry(I,Person,Distance,NorthOffset);
                TelemetryTimes[I]=Now;TelemetryIds[I]=Person.Id;
            }
            const FBox Bounds=LocalBounds;
            const float Padding=MeshHeight*.025f;
            const FVector Right=ViewOrientation.GetRightVector(),Up=ViewOrientation.GetUpVector();
            FVector Corner=FVector::ZeroVector;double Best=-DBL_MAX;
            for(int32 J=0;J<4;++J)
            {
                const FVector Candidate=Pose.TransformPosition(FVector(J&1?Bounds.Max.X+Padding:Bounds.Min.X-Padding,
                    J&2?Bounds.Max.Y+Padding:Bounds.Min.Y-Padding,Bounds.Max.Z+Padding));
                // Select the visible top-right box corner, with a small preference
                // for the nearer corner when two share the same horizontal edge.
                const double Score=FVector::DotProduct(Candidate-Pose.GetLocation(),Right)
                    -FVector::Dist(Candidate,ViewerMeters*WorldToMeters)*.001;
                if(Score>Best){Best=Score;Corner=Candidate;}
            }
            const float Height=FMath::Clamp(Distance*.018f,.04f,.13f);
            Telemetry[I]->SetWorldTransform(FTransform(ViewOrientation,
                Corner+(Right*.035f+Up*.025f)*WorldToMeters,FVector(Height*WorldToMeters)));
        }
    }
}

UProceduralMeshComponent* AWallhackPeopleRenderer::CreateTelemetry()
{
    auto* Label=NewObject<UProceduralMeshComponent>(this);
    AddInstanceComponent(Label);Label->SetupAttachment(GetRootComponent());
    Label->SetMobility(EComponentMobility::Movable);Label->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Label->SetGenerateOverlapEvents(false);Label->SetCanEverAffectNavigation(false);
    Label->SetCastShadow(false);Label->bReceivesDecals=false;
    auto* Texture=NewObject<UTextureRenderTarget2D>(this);
    Texture->RenderTargetFormat=RTF_RGBA8;
    Texture->ClearColor=FLinearColor::Transparent;
    Texture->InitAutoFormat(384,160);
    Texture->UpdateResourceImmediate(true);
    TelemetryTextures.Add(Texture);
    auto* Material=UMaterialInstanceDynamic::Create(LabelMaterial,this);
    Material->SetTextureParameterValue(TEXT("LabelTexture"),Texture);
    Label->SetMaterial(0,Material);
    Label->RegisterComponent();
    // One shared-eye billboard quad; the runtime font matches the compositor HUD.
    const TArray<FVector> Vertices={{0,-.15,-.2},{0,7.85,-.2},{0,7.85,3.133333},{0,-.15,3.133333}};
    Label->CreateMeshSection_LinearColor(0,Vertices,{0,1,2,0,2,3},{},
        {{0,1},{1,1},{1,0},{0,0}},{FLinearColor::White,FLinearColor::White,FLinearColor::White,FLinearColor::White},{},false);
    return Label;
}

void AWallhackPeopleRenderer::BuildTelemetry(int32 Slot,const FWallhackPersonPose& Person,float Distance,float NorthOffset)
{
    const FString Header=Person.bArticulated?FString::Printf(TEXT("P%d / %s"),Person.Id,*Person.SourceLabel)
        :Person.SourceLabel.IsEmpty()?FString::Printf(TEXT("PERSON %02d / MANUAL"),Person.Id)
        :FString::Printf(TEXT("%s%d / %s"),Person.bRadarOnly?TEXT("R"):TEXT("C"),Person.Id,*Person.SourceLabel);
    const FString Range=FString::Printf(TEXT("%.1f M"),Distance);
    const int32 Facing=FMath::RoundToInt(FRotator::ClampAxis(Person.Facing+NorthOffset))%360;
    const FString Detail=Person.bArticulated?FString::Printf(TEXT("%s H %.2f M / %s"),Person.bHeightEstimated?TEXT("EST."):TEXT("ASSUMED"),Person.Height,
        Person.bCameraPose?TEXT("VISIBLE JOINTS"):TEXT("INFERRED MOTION"))
        :Person.SourceLabel.IsEmpty()?FString::Printf(TEXT("H %.2f M / FACE %03d"),Person.Height,Facing)
        :FString::Printf(TEXT("ASSUMED H %.2f M / FACES SENSOR"),Person.Height);
    const FString Text=Header+TEXT("\n")+Range+TEXT("\n")+Detail;
    if(TelemetryText[Slot]==Text)return;
    TelemetryText[Slot]=Text;
    auto* Texture=TelemetryTextures[Slot].Get();
    UKismetRenderingLibrary::ClearRenderTarget2D(this,Texture,FLinearColor::Transparent);
    UCanvas* Canvas=nullptr;FVector2D Size;FDrawToRenderTargetContext Context;
    UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(this,Texture,Canvas,Size,Context);
    if(Canvas)
    {
        const uint32 Modes=Canvas->Canvas->GetAllowedModes();
        Canvas->Canvas->SetAllowedModes(Modes&~FCanvas::Allow_Flush);
        Canvas->Canvas->SetWriteDestinationAlpha(true);
        UFont* Font=GEngine?GEngine->GetSmallFont():nullptr;
        if(!Font&&GEngine)Font=GEngine->GetMediumFont();
        if(!Font&&GEngine)Font=GEngine->GetLargeFont();
        auto TextLine=[&](const FString& Value,float Y,float Height,FLinearColor Color)
        {
            if(!Font)return;
            float W=0,H=0;Canvas->StrLen(Font,Value,W,H);
            const float Scale=FMath::Min(Height/FMath::Max(H,1.f),350.f/FMath::Max(W,1.f));
            Canvas->K2_DrawText(Font,Value,{18,Y},FVector2D(Scale),Color,0,
                FLinearColor::Transparent,FVector2D::ZeroVector,false,false,false,FLinearColor::Transparent);
        };
        FCanvasTileItem Panel({0,0},Size,FLinearColor(.008,.012,.016,.50));
        Panel.BlendMode=SE_BLEND_AlphaBlend;Canvas->DrawItem(Panel);
        FLinearColor Accent=WallhackPeopleStyle::Color(Person);Accent.A=.95f;
        FCanvasTileItem Rule({0,0},{3,Size.Y},Accent);
        Rule.BlendMode=SE_BLEND_AlphaBlend;Canvas->DrawItem(Rule);
        TextLine(Header,13,22,FLinearColor(.92,.94,.95,.92));
        TextLine(Range,48,46,FLinearColor(.96,.97,.98,.98));
        TextLine(Detail,122,20,FLinearColor(.67,.71,.74,.88));
        Canvas->Canvas->SetAllowedModes(Modes);
    }
    UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(this,Context);
}
