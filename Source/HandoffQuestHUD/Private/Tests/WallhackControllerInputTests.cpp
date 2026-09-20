#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "EnhancedInputDeveloperSettings.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputCoreTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FControllerStartupInputsTest,"Wallhack.Input.OpenXRStartupBindings",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FControllerStartupInputsTest::RunTest(const FString&)
{
    const auto* Settings = GetDefault<UEnhancedInputDeveloperSettings>();
    TestTrue(TEXT("OpenXR can enumerate default contexts before BeginPlay"), bool(Settings->bEnableDefaultMappingContexts));
    TSet<const UInputMappingContext*> Registered;
    for (const auto& Setting : Settings->DefaultMappingContexts)
    {
        const auto* Context = Setting.InputMappingContext.LoadSynchronous();
        if (!TestNotNull(TEXT("Startup input asset loads"), Context)) continue;
        Registered.Add(Context);
        TestFalse(TEXT("Mutually exclusive modes do not activate together"), Setting.bAddImmediately);
        TestFalse(TEXT("OpenXR action-set description exists"), Context->ContextDescription.IsEmpty());
    }
    for (const TCHAR* Mode : {TEXT("Common"),TEXT("Sensor"),TEXT("Navigation"),TEXT("Demo")})
    {
        const auto* Context = LoadObject<UInputMappingContext>(nullptr,
            *FString::Printf(TEXT("/Game/Input/IMC_Wallhack%s.IMC_Wallhack%s"),Mode,Mode));
        if (!TestNotNull(Mode, Context)) continue;
        TestTrue(TEXT("HUD mode uses the exact startup object"), Registered.Contains(Context));
        TestTrue(TEXT("Context contains bindings in UE5.7 DefaultKeyMappings"), Context->GetMappings().Num()>0);
        TSet<FKey> Keys;
        for (const auto& Mapping : Context->GetMappings())
        {
            TestTrue(TEXT("Controller/keyboard key is registered"), Mapping.Key.IsValid());
            TestFalse(TEXT("No ambiguous same-mode key binding"), Keys.Contains(Mapping.Key));
            Keys.Add(Mapping.Key);
            if (TestNotNull(TEXT("Action asset loads"), Mapping.Action.Get()))
                TestFalse(TEXT("OpenXR action has a localized description"), Mapping.Action->ActionDescription.IsEmpty());
        }
        if (FCString::Strcmp(Mode,TEXT("Common"))==0)
            TestTrue(TEXT("Left Touch interaction is explicitly bound"), Keys.Contains(EKeys::OculusTouch_Left_X_Click));
        if (FCString::Strcmp(Mode,TEXT("Sensor"))==0)
        {
            const auto* Confirm = LoadObject<UInputAction>(nullptr,TEXT("/Game/Input/IA_SensorConfirm.IA_SensorConfirm"));
            bool bCorrectTrigger=false;
            for (const auto& Mapping : Context->GetMappings())
                bCorrectTrigger |= Mapping.Key==EKeys::OculusTouch_Right_Trigger_Click && Mapping.Action==Confirm;
            TestTrue(TEXT("Index trigger targets the same action object used by calibration"), bCorrectTrigger);
        }
    }
    return true;
}
#endif
