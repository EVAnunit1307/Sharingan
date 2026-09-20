#include "WallhackSensorPeopleTypes.h"
#include "WallhackHumanPose.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Json.h"

static FString PoseFixture()
{
    FString Text;FFileHelper::LoadFileToString(Text,*FPaths::Combine(FPaths::ProjectDir(),TEXT("GroundStation/tests/fixtures/articulated_people.json")));return Text;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTrackedPoseProtocolTest,"Wallhack.SensorPeople.ArticulatedProtocolAndExpiry",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FTrackedPoseProtocolTest::RunTest(const FString&)
{
    FWallhackSensorPeopleStream Stream;
    if(!TestTrue(TEXT("Versioned tracks parse alongside legacy observations"),Stream.Ingest(PoseFixture(),10)))return false;
    auto F=Stream.GetFrame(10);
    if(!TestEqual(TEXT("One persistent track"),F.Tracks.Num(),1))return false;
    TestEqual(TEXT("Model produced 33 joints"),F.Tracks[0].Joints.Num(),33);
    TestEqual(TEXT("Persistent ID"),F.Tracks[0].Id,41);
    TestTrue(TEXT("Joint data expires before root"),Stream.GetFrame(10.351).Tracks[0].Joints.IsEmpty());
    TSharedPtr<FJsonObject> Root;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(PoseFixture()),Root);
    Root->SetNumberField(TEXT("relay_sequence"),2);FString Repeat;
    FJsonSerializer::Serialize(Root.ToSharedRef(),TJsonWriterFactory<>::Create(&Repeat));
    TestTrue(TEXT("Repeated relay packet accepted"),Stream.Ingest(Repeat,10.4));
    TestTrue(TEXT("Repeated joint timestamp cannot renew pose"),Stream.GetFrame(10.4).Tracks[0].Joints.IsEmpty());
    TestTrue(TEXT("Repeated root sample cannot renew position"),Stream.GetFrame(10.501).Tracks.IsEmpty());
    Stream.Reset();Root->GetObjectField(TEXT("spatial_people"))->SetBoolField(TEXT("rig_pose_valid"),false);
    FJsonSerializer::Serialize(Root.ToSharedRef(),TJsonWriterFactory<>::Create(&Repeat));
    TestTrue(TEXT("Untracked rig status parsed"),Stream.Ingest(Repeat,20));
    TestFalse(TEXT("Moving rig cannot claim fixed world placement"),Stream.GetFrame(20).bRigPoseValid);
    Root->SetNumberField(TEXT("relay_sequence"),3);
    Root->GetObjectField(TEXT("spatial_people"))->SetStringField(TEXT("rig_motion_mode"),TEXT("left_controller"));
    FJsonSerializer::Serialize(Root.ToSharedRef(),TJsonWriterFactory<>::Create(&Repeat));
    TestTrue(TEXT("Controller mode parses"),Stream.Ingest(Repeat,20.1));
    TestEqual(TEXT("Controller alignment is explicitly required"),Stream.GetFrame(20.1).RigMotionMode,FString(TEXT("left_controller")));
    return true;
}
#endif

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR && !PLATFORM_ANDROID
#include "WallhackRuntimeTestWorld.h"
#include "WallhackPeopleRenderer.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "AssetCompilingManager.h"
#include "Engine/SceneCapture2D.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "ImageUtils.h"
#include "RenderingThread.h"
#include "SkeletalRenderPublic.h"
#include "Rendering/SkeletalMeshRenderData.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FArticulatedHumanTest,"Wallhack.SensorPeople.ArticulatedMeshAndFallback",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FArticulatedHumanTest::RunTest(const FString&)
{
    FScopedWallhackRuntimeWorld World;
    auto* Renderer=World.World->SpawnActor<AWallhackPeopleRenderer>();
    if(!TestNotNull(TEXT("Renderer"),Renderer))return false;
    FWallhackPersonPose P;P.Id=41;P.Height=1.65;P.bArticulated=true;P.Feet={3,0,0};P.SourceLabel=TEXT("ESTIMATED POSE");
    Renderer->Present({P},INDEX_NONE,nullptr,true,100,{0,0,1.7},FQuat::Identity,0,1);
    FAssetCompilingManager::Get().FinishAllCompilation();
    if(!TestEqual(TEXT("One articulated component"),Renderer->GetArticulatedComponents().Num(),1))return false;
    auto* Body=Renderer->GetArticulatedComponents()[0].Get();
    if(!TestNotNull(TEXT("Rigged asset"),Body->GetSkinnedAsset()))return false;
    TestTrue(TEXT("Human rig has at least 20 bones"),Body->GetNumBones()>=20);
    TestTrue(TEXT("CPU skin buffers survive cook"),Cast<USkeletalMesh>(Body->GetSkinnedAsset())->GetLODInfo(1)->bAllowCPUAccess);
    TestFalse(TEXT("Rig visible"),Body->bHiddenInGame);
    TestTrue(TEXT("Static duplicate hidden"),Renderer->GetBodyComponents()[0]->bHiddenInGame);
    const FVector Idle=Body->GetBoneLocationByName(TEXT("hand_l"),EBoneSpaces::ComponentSpace);
    FWallhackSensorPeopleStream Stream;Stream.Ingest(PoseFixture(),1);const auto F=Stream.GetFrame(1);
    if(F.Tracks.IsEmpty())return false;
    P.Facing=F.Tracks[0].Facing;
    for(const auto& Joint:F.Tracks[0].Joints)P.Joints.Add(FRotator(0,F.Tracks[0].PoseYaw-P.Facing,0).RotateVector(Joint));
    P.JointQuality=F.Tracks[0].JointQuality;P.bCameraPose=true;
    Renderer->Present({P},INDEX_NONE,nullptr,true,100,{0,0,1.7},FQuat::Identity,0,2);
    const FVector Seen=Body->GetBoneLocationByName(TEXT("hand_l"),EBoneSpaces::ComponentSpace);
    TestTrue(TEXT("Actual camera pose articulates the wrist"),FVector::Dist(Idle,Seen)>5);
    for(const auto& Pair:TArray<TPair<FName,FName>>{{TEXT("upperarm_l"),TEXT("lowerarm_l")},{TEXT("thigh_r"),TEXT("calf_r")}})
    {
        const auto A=Body->GetBoneLocationByName(Pair.Key,EBoneSpaces::ComponentSpace);
        const auto B=Body->GetBoneLocationByName(Pair.Value,EBoneSpaces::ComponentSpace);
        TestTrue(TEXT("Limb length stays plausible"),FVector::Dist(A,B)>10&&FVector::Dist(A,B)<30);
    }
    P.bCameraPose=false;P.Joints.Reset();P.JointQuality.Reset();P.Speed=1;P.LocalVelocity={1,0,0};
    Renderer->Present({P},INDEX_NONE,nullptr,true,100,{0,0,1.7},FQuat::Identity,0,3);
    TestTrue(TEXT("Radar fallback retains same component"),Renderer->GetArticulatedComponents()[0].Get()==Body);
    TestTrue(TEXT("Fallback labelled as inferred"),Renderer->GetTelemetryText()[0].Contains(TEXT("INFERRED MOTION")));
    Renderer->Present({},INDEX_NONE,nullptr,true,100);
    TestTrue(TEXT("Expired track hides the articulated body"),Body->bHiddenInGame);
    Renderer->Present({P},INDEX_NONE,nullptr,false,100);
    TestTrue(TEXT("Hidden density hides the whole renderer"),Renderer->IsHidden());
    return true;
}

class FArticulatedCaptureCommand final:public IAutomationLatentCommand
{
    FAutomationTestBase* Test;
    TUniquePtr<FScopedWallhackRuntimeWorld> F;
    AWallhackPeopleRenderer* Renderer=nullptr;
    USceneCaptureComponent2D* Capture=nullptr;
    UTextureRenderTarget2D* RT=nullptr;
    FWallhackPersonPose P;
    int32 Frame=0,Pose=0;
public:
    explicit FArticulatedCaptureCommand(FAutomationTestBase* InTest):Test(InTest){}
    bool Update() override
    {
        if(!F)
        {
            F=MakeUnique<FScopedWallhackRuntimeWorld>(100,false);
            F->World->BeginPlay();
            Renderer=F->World->SpawnActor<AWallhackPeopleRenderer>();
            FWallhackSensorPeopleStream Stream;
            if(!Test->TestTrue(TEXT("Pose fixture parses"),Stream.Ingest(PoseFixture(),1)))return true;
            const auto Track=Stream.GetFrame(1).Tracks[0];
            P.Id=41;P.Height=1.65;P.bArticulated=true;P.bCameraPose=true;
            P.Feet={3,0,0};P.Facing=Track.Facing;
            for(const auto& Joint:Track.Joints)P.Joints.Add(FRotator(0,Track.PoseYaw-P.Facing,0).RotateVector(Joint));
            P.JointQuality=Track.JointQuality;
            P.SourceLabel=TEXT("CAMERA POSE");P.Tint={.95,.1,.1,1};
            Renderer->Present({P},INDEX_NONE,nullptr,true,100,{0,0,.9},FQuat::Identity,0,1);
            FAssetCompilingManager::Get().FinishAllCompilation();
            auto* Camera=F->World->SpawnActor<ASceneCapture2D>();Capture=Camera->GetCaptureComponent2D();
            Capture->bCaptureEveryFrame=false;Capture->bCaptureOnMovement=false;
            Capture->bAlwaysPersistRenderingState=true;Capture->CaptureSource=SCS_SceneColorHDR;
            Capture->PrimitiveRenderMode=ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
            Capture->ShowOnlyComponent(Renderer->GetArticulatedComponents()[0]);Capture->FOVAngle=60;
            RT=NewObject<UTextureRenderTarget2D>(Camera);RT->RenderTargetFormat=RTF_RGBA16f;
            RT->ClearColor=FLinearColor::Black;RT->InitAutoFormat(768,768);RT->UpdateResourceImmediate(true);Capture->TextureTarget=RT;
        }
        P.bCameraPose=Pose==0;P.Speed=Pose==0?0:1;P.LocalVelocity={1,0,0};
        Renderer->Present({P},INDEX_NONE,nullptr,true,100,{0,0,.9},FQuat::Identity,0,Pose+2+Frame/72.);
        F->World->Tick(LEVELTICK_All,1.f/72);
        F->World->SendAllEndOfFrameUpdates();
        Capture->SetWorldLocationAndRotation({0,-3.2,90},FRotator::ZeroRotator);
        Capture->CaptureScene();FlushRenderingCommands();
        // Actual engine frames allow the deferred skeletal updater and PSOs to settle.
        if(++Frame<8)return false;
        auto* Skin=Renderer->GetArticulatedComponents()[0].Get();
        for(const auto& Section:Skin->GetSkeletalMeshRenderData()->LODRenderData[0].RenderSections)
            Test->TestTrue(TEXT("Every skin section uses the identity material"),Skin->GetMaterial(Section.MaterialIndex)==Skin->GetMaterial(0));
        Test->TestTrue(TEXT("Repeated presentation retains uploaded skeletal data"),
            Skin->GetMeshObject()&&Skin->GetMeshObject()->bHasBeenUpdatedAtLeastOnce);
        int32 Ink[2]={};
        for(int32 Eye=0;Eye<2;++Eye)
        {
            Capture->SetWorldLocationAndRotation({0,Eye?3.2:-3.2,90},FRotator::ZeroRotator);
            F->World->SendAllEndOfFrameUpdates();
            for(int I=0;I<3;++I){Capture->CaptureScene();FlushRenderingCommands();}
            TArray<FColor> Pixels;RT->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
            for(const auto C:Pixels)Ink[Eye]+=C.R>C.G*1.5&&C.R>20;
            Test->AddInfo(FString::Printf(TEXT("Skin pixels pose=%d eye=%d count=%d"),Pose,Eye,Ink[Eye]));
            TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(768,768,Pixels,PNG);
            const FString Path=FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("PersonPose"),
                FString::Printf(TEXT("articulated-%s-%s.png"),Pose?TEXT("inferred"):TEXT("camera"),Eye?TEXT("right"):TEXT("left")));
            FFileHelper::SaveArrayToFile(PNG,*Path);
            Test->TestTrue(TEXT("Articulated skin alone renders in each eye"),Ink[Eye]>800);
        }
        Test->TestTrue(TEXT("Both eye views retain the figure"),FMath::Min(Ink[0],Ink[1])>FMath::Max(Ink[0],Ink[1])*.7);
        Frame=0;
        return ++Pose>=2;
    }
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FArticulatedHumanRenderTest,"Wallhack.SensorPeople.ArticulatedPairedEyeRender",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FArticulatedHumanRenderTest::RunTest(const FString&)
{
    ADD_LATENT_AUTOMATION_COMMAND(FArticulatedCaptureCommand(this));
    return true;
}
#endif
