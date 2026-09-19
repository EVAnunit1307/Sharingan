#include "WallhackNavigationRenderer.h"
#include "WallhackNavigationSubsystem.h"
#include "ProceduralMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

namespace WallhackNav
{
FTrailGeometry BuildEstimatedLabel(FVector Position,FVector Viewer)
{
    // Small geometry glyphs share the unlit trail material. They remain visible
    // in passthrough without scene lights or a separate distance-field font cook.
    static constexpr uint8 Glyphs[][7]={
        {31,16,16,30,16,16,31},{15,16,16,14,1,1,30},{31,4,4,4,4,4,4},
        {14,4,4,4,4,4,14},{17,27,21,21,17,17,17},{14,17,17,31,17,17,17},
        {31,4,4,4,4,4,4},{31,16,16,30,16,16,31},{30,17,17,17,17,17,30}};
    FTrailGeometry G;
    const FVector Forward=(Viewer-Position).GetSafeNormal();
    const FVector Right=FVector::CrossProduct(Forward,FVector::UpVector).GetSafeNormal();
    const FVector Up=FVector::CrossProduct(Right,Forward).GetSafeNormal();
    constexpr float Pixel=.008f;
    for(int32 Letter=0;Letter<9;++Letter)for(int32 Row=0;Row<7;++Row)for(int32 Col=0;Col<5;++Col)
    {
        if(!(Glyphs[Letter][Row]&(1<<(4-Col))))continue;
        const FVector P=Position+Right*(Letter*6+Col-26)*Pixel+Up*(3-Row)*Pixel;
        const FVector R=Right*Pixel*.46f,U=Up*Pixel*.46f;const int32 N=G.Vertices.Num();
        G.Vertices.Append({P-R-U,P+R-U,P+R+U,P-R+U});G.Indices.Append({N,N+1,N+2,N,N+2,N+3});
        for(int32 I=0;I<4;++I)G.Colors.Add(FLinearColor(.8,.48,.14,.45));
    }
    return G;
}
FTrailGeometry BuildTrail(const FDisplaySnapshot& D)
{
    FTrailGeometry G;
    if(!D.bGuidance||D.bHidden)return G;
    const FLinearColor Mint(.32f,.7f,.55f,.30f),Amber(.8f,.48f,.14f,.18f);
    constexpr float ArrowHalfSpan=.11f,ArrowDepth=.05f,StrokeWidth=.006f;
    auto Stroke=[&](FVector A,FVector B,float Width,FLinearColor C)
    {
        const FVector Side=FVector::CrossProduct((B-A).GetSafeNormal(),FVector::UpVector).GetSafeNormal()*Width*.5f;
        if(Side.IsNearlyZero())return;
        const int32 N=G.Vertices.Num();
        G.Vertices.Append({A-Side,A+Side,B+Side,B-Side});
        G.Indices.Append({N,N+1,N+2,N,N+2,N+3});for(int32 I=0;I<4;++I)G.Colors.Add(C);
    };
    auto Ring=[&](FVector P,float Radius,FLinearColor Color)
    {
        P.Z+=.03f;for(int32 I=0;I<32;++I)
        {const float A=I*UE_TWO_PI/32,B=(I+1)*UE_TWO_PI/32;Stroke(P+FVector(FMath::Cos(A),FMath::Sin(A),0)*Radius,P+FVector(FMath::Cos(B),FMath::Sin(B),0)*Radius,StrokeWidth,Color);}
    };
    int32 Closest=0;float Best=FLT_MAX;
    for(int32 I=0;I<D.Route.Points.Num();++I){float Distance=FVector::DistSquared2D(D.Viewer,D.Route.Points[I].Position);if(Distance<Best){Best=Distance;Closest=I;}}
    float Along=0,NextArrow=.55f;
    for(int32 I=Closest+1;I<D.Route.Points.Num();++I)
    {
        FVector A=D.Route.Points[I-1].Position,B=D.Route.Points[I].Position;A.Z+=.03f;B.Z+=.03f;
        const bool Est=D.Route.Points[I-1].bEstimated||D.Route.Points[I].bEstimated;
        const float Length=FVector::Dist(A,B);if(Length<.001f)continue;
        FLinearColor C=Est?Amber:Mint;C.A*=FMath::Clamp(1.f-(Along-8)/12,.08f,1.f);
        // Sparse, shallow chevrons follow the planner's curve. No connecting
        // ribbon or arrow shaft. Amber/ESTIMATED still distinguishes unknowns.
        while(NextArrow<Along+Length)
        {
            if(NextArrow>=Along)
            {
                const FVector Tip=FMath::Lerp(A,B,(NextArrow-Along)/Length);
                const FVector Forward=(B-A).GetSafeNormal(),Side=FVector::CrossProduct(Forward,FVector::UpVector);
                Stroke(Tip-Forward*ArrowDepth+Side*ArrowHalfSpan,Tip,StrokeWidth,C);
                Stroke(Tip-Forward*ArrowDepth-Side*ArrowHalfSpan,Tip,StrokeWidth,C);
            }
            NextArrow+=.85f;
        }
        Along+=Length;
    }
    if(D.bHasTarget)Ring(D.Target.Standing,.12f,Mint);
    if(D.bAiming&&D.bAimTracked)
    {
        Stroke(D.AimOrigin,D.AimEnd,.006f,FLinearColor(.65,.72,.69,.3));
        if(D.bPreviewValid)Ring(D.Preview.Standing,.22f,Mint);
    }
    return G;
}
}
AWallhackNavigationRenderer::AWallhackNavigationRenderer()
{
    PrimaryActorTick.bCanEverTick=true;PrimaryActorTick.TickGroup=TG_PostUpdateWork;
    Trail=CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("FloorTrail"));SetRootComponent(Trail);
    Trail->SetCollisionEnabled(ECollisionEnabled::NoCollision);Trail->SetCastShadow(false);
    static ConstructorHelpers::FObjectFinder<UMaterialInterface> Material(TEXT("/Game/Materials/M_WallhackTrail.M_WallhackTrail"));
    if(Material.Succeeded())Trail->SetMaterial(0,Material.Object);
    EstimatedLabel=CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("EstimatedTransition"));EstimatedLabel->SetupAttachment(Trail);
    EstimatedLabel->SetCastShadow(false);EstimatedLabel->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    if(Material.Succeeded())EstimatedLabel->SetMaterial(0,Material.Object);
}
void AWallhackNavigationRenderer::Tick(float Dt)
{
    Super::Tick(Dt);const auto* Nav=GetWorld()->GetSubsystem<UWallhackNavigationSubsystem>();if(!Nav)return;
    auto D=Nav->GetDisplaySnapshot();
    const bool Show=D.bGuidance&&!D.bHidden;SetActorHiddenInGame(!Show);if(!Show)return;
    Refresh+=Dt;if(Refresh<.1f&&!D.bAiming)return;Refresh=0;
    // Guidance is intentionally visible through scanned and live geometry.
    // Its translucent material ignores depth; map collision still constrains
    // the actual route, independently of this presentation choice.
    auto G=WallhackNav::BuildTrail(D);
    const float Scale=GetWorld()->GetWorldSettings()->WorldToMeters;
    for(auto& V:G.Vertices)V*=Scale;
    Trail->CreateMeshSection_LinearColor(0,G.Vertices,G.Indices,{}, {},G.Colors,{},false);
    EstimatedLabel->SetVisibility(false);
    for(int32 I=0;I<D.Route.Points.Num();++I)if(D.Route.Points[I].bEstimated&&(I==0||!D.Route.Points[I-1].bEstimated))
    {
        const FVector P=D.Route.Points[I].Position+FVector(0,0,.15);
        if(FVector::Dist2D(P,D.Viewer)<8)
        {
            auto Label=WallhackNav::BuildEstimatedLabel(P,D.Viewer);for(auto& V:Label.Vertices)V*=Scale;
            EstimatedLabel->CreateMeshSection_LinearColor(0,Label.Vertices,Label.Indices,{}, {},Label.Colors,{},false);
            EstimatedLabel->SetVisibility(true);
        }
        break;
    }
}
