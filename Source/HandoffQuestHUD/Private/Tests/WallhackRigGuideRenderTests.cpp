#include "WallhackTrackedRig.h"
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR && !PLATFORM_ANDROID
#include "Misc/AutomationTest.h"
#include "WallhackRuntimeTestWorld.h"
#include "WallhackSensorPeopleActor.h"
#include "ProceduralMeshComponent.h"
#include "AssetCompilingManager.h"
#include "Engine/SceneCapture2D.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RenderingThread.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRigGuideRenderTest,"Wallhack.SensorPeople.OriginGuidePairedEyeAndHidden",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FRigGuideRenderTest::RunTest(const FString&)
{
    FScopedWallhackRuntimeWorld World(100,false);
    if(!TestNotNull(TEXT("Render world"),World.World))return false;
    auto* Actor=World.World->SpawnActor<AWallhackSensorPeopleActor>();
    auto* Mesh=Actor->FindComponentByClass<UProceduralMeshComponent>();
    if(!TestNotNull(TEXT("Production guide component"),Mesh)||!TestNotNull(TEXT("Cooked guide material"),Mesh->GetMaterial(0)))return false;
    const auto G=BuildWallhackRigCalibrationGuides(FTransform::Identity);
    Mesh->CreateMeshSection_LinearColor(0,G.Vertices,G.Indices,{}, {},G.Colors,{},false);
    Mesh->SetWorldTransform(FTransform(FRotator(0,60,0),FVector(180,0,100),FVector(100)));
    Mesh->SetVisibility(true);
    FAssetCompilingManager::Get().FinishAllCompilation();
    auto* Camera=World.World->SpawnActor<ASceneCapture2D>();
    auto* Capture=Camera->GetCaptureComponent2D();
    Capture->bCaptureEveryFrame=false;Capture->bCaptureOnMovement=false;
    Capture->CaptureSource=SCS_SceneColorHDR;Capture->FOVAngle=40;
    Capture->PrimitiveRenderMode=ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
    Capture->ShowOnlyComponent(Mesh);
    auto* RT=NewObject<UTextureRenderTarget2D>(Camera);
    RT->RenderTargetFormat=RTF_RGBA16f;RT->ClearColor=FLinearColor::Black;
    RT->InitAutoFormat(768,768);RT->UpdateResourceImmediate(true);Capture->TextureTarget=RT;
    int32 Ink[2]={};
    for(int32 Eye=0;Eye<3;++Eye)
    {
        if(Eye==2)Actor->SetPresentationHidden(true);
        Capture->SetWorldLocationAndRotation(FVector(0,Eye==1?3.2:-3.2,100),FRotator::ZeroRotator);
        World.World->SendAllEndOfFrameUpdates();
        for(int32 I=0;I<4;++I){Capture->CaptureScene();FlushRenderingCommands();}
        TArray<FColor> Pixels;RT->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
        int32 White=0,Mint=0,MintTop=768,MintBottom=0;
        for(int32 Pixel=0;Pixel<Pixels.Num();++Pixel)
        {
            const FColor C=Pixels[Pixel];
            White+=C.R>30&&FMath::Abs(int32(C.G)-C.R)<25;
            if(C.G>30&&C.G>C.R*1.1)
            {++Mint;MintTop=FMath::Min(MintTop,Pixel/768);MintBottom=FMath::Max(MintBottom,Pixel/768);}
        }
        if(Eye<2)
        {
            Ink[Eye]=White+Mint;
            TestTrue(TEXT("White radar-centre guide visible in each eye"),White>50);
            TestTrue(TEXT("Mint forward arrow visible in each eye"),Mint>50);
            TestTrue(TEXT("Arrowhead retains height when viewed level with the rig"),MintBottom-MintTop>12);
            TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(768,768,Pixels,PNG);
            const FString Path=FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("PersonPose"),
                FString::Printf(TEXT("sensor-origin-%s.png"),Eye?TEXT("right"):TEXT("left")));
            FFileHelper::SaveArrayToFile(PNG,*Path);
        }
        else TestEqual(TEXT("Hidden immediately removes calibration guides"),White+Mint,0);
        AddInfo(FString::Printf(TEXT("Guide view=%d white=%d mint=%d"),Eye,White,Mint));
    }
    TestTrue(TEXT("Both eye views retain guide"),FMath::Min(Ink[0],Ink[1])>FMath::Max(Ink[0],Ink[1])*.7);
    return true;
}
#endif
