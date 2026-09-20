#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR && !PLATFORM_ANDROID
#include "WallhackRuntimeTestWorld.h"
#include "WallhackSensorPeopleActor.h"
#include "WallhackTelemetrySubsystem.h"
#include "Engine/GameInstance.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"

// Run by Build/verify_sensor_setup.py against real loopback Pi/relay sockets.
// Separate namespace: the normal Wallhack suite needs no external server.
class FWallhackSensorRelayCommand final : public IAutomationLatentCommand
{
    FAutomationTestBase* Test;
    TUniquePtr<FScopedWallhackRuntimeWorld> Fixture;
    UGameInstance* Instance = nullptr;
    UWallhackTelemetrySubsystem* Telemetry = nullptr;
    AWallhackSensorPeopleActor* People = nullptr;
    FString Control;
    int32 Stage = 0, PersonId = INDEX_NONE;
    double Deadline = 0;

    void Mode(const TCHAR* Value)
    {
        Test->TestTrue(TEXT("Fixture control written"), FFileHelper::SaveStringToFile(Value, *Control,
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
        Deadline = FPlatformTime::Seconds() + 15;
    }
    void Aim(float FloorX)
    {
        Fixture->SetViewerPose({0,0,170}, FRotator(-FMath::RadiansToDegrees(FMath::Atan2(170.f, FloorX)),0,0));
        People->ConfirmPlacement();
    }
public:
    FWallhackSensorRelayCommand(FAutomationTestBase* InTest, const FString& Url, const FString& InControl)
        : Test(InTest), Control(InControl)
    {
        Fixture = MakeUnique<FScopedWallhackRuntimeWorld>();
        if (!Fixture->HasViewer()) return;
        FCommandLine::Append(TEXT(" -WallhackSensorPeople -WallhackSensorPeoplePreview"));
        Instance = NewObject<UGameInstance>(GEngine);
        Instance->AddToRoot();
        Instance->InitializeStandalone();
        UWorld* Dummy = Instance->GetWorld();
        GEngine->DestroyWorldContext(Fixture->World);
        Instance->GetWorldContext()->SetCurrentWorld(Fixture->World);
        Fixture->World->SetGameInstance(Instance);
        Dummy->DestroyWorld(false);
        Telemetry = Instance->GetSubsystem<UWallhackTelemetrySubsystem>();
        Telemetry->SetBridgeUrl(Url);
        People = Fixture->World->SpawnActor<AWallhackSensorPeopleActor>();
        People->DispatchBeginPlay();
        Mode(TEXT("live"));
    }
    ~FWallhackSensorRelayCommand()
    {
        if (People && People->HasActorBegunPlay()) People->RouteEndPlay(EEndPlayReason::EndPlayInEditor);
        if (Instance)
        {
            Instance->Shutdown();
            Fixture->World->SetGameInstance(nullptr);
            Instance->RemoveFromRoot();
        }
        Fixture.Reset();
    }
    virtual bool Update() override
    {
        if (!People || !Telemetry) { Test->AddError(TEXT("Native relay fixture initialization failed")); return true; }
        if (FPlatformTime::Seconds() > Deadline)
        {
            Test->AddError(FString::Printf(TEXT("Relay stage %d timed out: %s"),Stage,*People->GetStatus()));
            return true;
        }
        Telemetry->Tick(.016f);
        People->Tick(.016f);
        const auto& Views = People->GetPeopleViews();
        if (Stage == 0)
        {
            const auto Frame = Telemetry->GetSensorPeopleFrame();
            if (Frame.People.IsEmpty() || !Frame.People[0].bRadar) return false;
            Test->TestTrue(TEXT("Loopback input is explicitly marked replay"),Frame.bReplay);
            Aim(100);
            Aim(120);
            Test->TestTrue(TEXT("Registration rejects forward point under 0.5 m"),People->GetStatus().Contains(TEXT("AT LEAST 0.5 M")));
            Aim(200);
            Fixture->SetViewerPose({0,0,170},FRotator::ZeroRotator);
            People->Tick(.016f);
            if (!Test->TestEqual(TEXT("Two floor confirmations render one real relay contact"),Views.Num(),1)) return true;
            PersonId=Views[0].Id;
            Test->TestTrue(TEXT("Registered origin 1 m + sensor range 3 m"),Views[0].Feet.Equals({400,0,0},.1));
            Fixture->SetViewerPose({100,0,170},FRotator(0,90,0));
            People->Tick(.016f);
            Test->TestTrue(TEXT("Walk and turn use current wearer pose"),FMath::IsNearlyEqual(Views[0].View.RightMeters,-3.f,.001f));
            People->SetPresentationHidden(true);
            Test->TestEqual(TEXT("Hidden clears contacts immediately"),Views.Num(),0);
            People->SetPresentationHidden(false);
            People->Tick(.016f);
            Test->TestEqual(TEXT("Visible recovers fresh contact"),Views.Num(),1);
            Stage=1; Mode(TEXT("radar_off"));
        }
        else if (Stage == 1 && Views.Num()==1 && !Views[0].bRadar)
        {
            Test->TestEqual(TEXT("Radar outage preserves camera identity"),Views[0].Id,PersonId);
            Test->TestTrue(TEXT("Radar outage uses camera estimate in the same frame"),Views[0].Feet.X>250 && Views[0].Feet.X<400);
            Stage=2; Mode(TEXT("camera_off"));
        }
        else if (Stage == 2 && Views.Num()==1 && Views[0].bRadarOnly)
        {
            Test->TestTrue(TEXT("Camera outage keeps an independent radar silhouette at measured range"),Views[0].Feet.Equals({400,0,0},.1));
            Test->TestTrue(TEXT("Radar-only contact remains live without camera confirmation"),People->GetStatus().Contains(TEXT("LIVE")));
            Test->TestEqual(TEXT("Radar silhouette is not also drawn as a map-only dot"),People->GetRadarViews().Num(),0);
            Stage=3; Mode(TEXT("live"));
        }
        else if (Stage == 3 && Views.Num()==1 && Views[0].bRadar && !Views[0].bRadarOnly)
        {
            Test->TestTrue(TEXT("Fresh packets recover against the same registration"),Views[0].Feet.Equals({400,0,0},.1));
            Stage=4; Mode(TEXT("frozen"));
        }
        else if (Stage == 4 && Views.IsEmpty() && People->GetStatus().Contains(TEXT("NO FRESH")))
        {
            Test->TestEqual(TEXT("Both sources expire even while relay packets continue"),People->GetRadarViews().Num(),0);
            Stage=5; Mode(TEXT("live"));
        }
        else if (Stage == 5 && Views.Num()==1 && Views[0].bRadar && !Views[0].bRadarOnly)
        {
            Stage=6; Mode(TEXT("restart"));
        }
        else if (Stage == 6 && Views.IsEmpty() && People->GetStatus().Contains(TEXT("MARK FLOOR")))
        {
            Test->AddInfo(TEXT("Actual Pi bridge -> laptop relay -> Unreal socket -> registration -> rendering passed, including outages and source restart."));
            return true;
        }
        return false;
    }
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWallhackSensorRelayTest,
    "SensorSetup.NativeRelay",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FWallhackSensorRelayTest::RunTest(const FString&)
{
    FString Url,Control;
    if (!FParse::Value(FCommandLine::Get(),TEXT("WallhackSensorTestUrl="),Url)
        || !FParse::Value(FCommandLine::Get(),TEXT("WallhackSensorTestControl="),Control))
    { AddError(TEXT("Run this test through Build/verify_sensor_setup.py --unreal")); return false; }
    ADD_LATENT_AUTOMATION_COMMAND(FWallhackSensorRelayCommand(this,Url,Control));
    return true;
}
#endif
