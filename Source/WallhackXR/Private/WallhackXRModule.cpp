#include "WallhackMountedController.h"
#include "Modules/ModuleManager.h"

#if PLATFORM_ANDROID
#include "Engine/Engine.h"
#include "IOpenXRExtensionPlugin.h"
#include "IOpenXRHMD.h"
#include "IXRTrackingSystem.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "OpenXRCore.h"
#include "khronos/openxr/meta_openxr_preview/meta_simultaneous_hands_and_controllers.h"

DEFINE_LOG_CATEGORY_STATIC(LogWallhackXR, Log, All);

class FWallhackXRModule final : public IModuleInterface, public IOpenXRExtensionPlugin
{
public:
    virtual void StartupModule() override
    {
        // Register before OpenXR creates its instance and builds input bindings.
        bRigMode = FParse::Param(FCommandLine::Get(), TEXT("WallhackSensorPeople"));
        if (bRigMode) RegisterOpenXRExtensionModularFeature();
    }

    virtual void ShutdownModule() override
    {
        if (bRigMode) UnregisterOpenXRExtensionModularFeature();
        // The runtime owns instance destruction. Session resources are released
        // in OnDestroySession, while their handles are still usable.
    }

    virtual FString GetDisplayName() override { return TEXT("Wallhack mounted physical controller"); }

    virtual bool GetRequiredExtensions(TArray<const ANSICHAR*>& Extensions) override
    {
        Extensions.Add("XR_META_detached_controllers");
        Extensions.Add(XR_META_SIMULTANEOUS_HANDS_AND_CONTROLLERS_EXTENSION_NAME);
        return true;
    }

    virtual void PostCreateInstance(XrInstance InInstance) override
    {
        Instance = InInstance;
        Check(xrGetInstanceProcAddr(Instance, "xrResumeSimultaneousHandsAndControllersTrackingMETA",
            reinterpret_cast<PFN_xrVoidFunction*>(&ResumeTracking)), TEXT("load simultaneous tracking"));
        LeftHeld = Path("/user/hand/left");
        LeftDetached = Path("/user/detached_controller_meta/left");
        HeldAim = Path("/user/hand/left/input/aim/pose");
        DetachedAim = Path("/user/detached_controller_meta/left/input/aim/pose");
        TouchProfiles = {Path("/interaction_profiles/oculus/touch_controller"),
            Path("/interaction_profiles/facebook/touch_controller_pro"),
            Path("/interaction_profiles/meta/touch_controller_plus")};
    }

    virtual void PostGetSystem(XrInstance InInstance, XrSystemId System) override
    {
        XrSystemSimultaneousHandsAndControllersPropertiesMETA Support{
            XR_TYPE_SYSTEM_SIMULTANEOUS_HANDS_AND_CONTROLLERS_PROPERTIES_META};
        XrSystemProperties Properties{XR_TYPE_SYSTEM_PROPERTIES, &Support};
        bSupported = Check(xrGetSystemProperties(InInstance, System, &Properties), TEXT("query support"))
            && Support.supportsSimultaneousHandsAndControllers == XR_TRUE;
    }

    virtual void PostCreateSession(XrSession InSession) override
    {
        Session = InSession;
        bResumed = false;
        if (!bSupported || !ResumeTracking || !LeftHeld || !LeftDetached) return;

        XrActionSetCreateInfo SetInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
        FCStringAnsi::Strcpy(SetInfo.actionSetName, "wallhack_mount");
        FCStringAnsi::Strcpy(SetInfo.localizedActionSetName, "Mounted controller tracking");
        if (!Check(xrCreateActionSet(Instance, &SetInfo, &ActionSet), TEXT("create action set"))) return;

        // The physical aim action is shared, but its two spaces explicitly
        // identify the subaction. Do not rely on runtime selection of an
        // unspecified subaction: Quest returned no pose for that space.
        const XrPath Subactions[] = {LeftHeld, LeftDetached};
        XrActionCreateInfo ActionInfo{XR_TYPE_ACTION_CREATE_INFO};
        ActionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
        ActionInfo.countSubactionPaths = UE_ARRAY_COUNT(Subactions);
        ActionInfo.subactionPaths = Subactions;
        FCStringAnsi::Strcpy(ActionInfo.actionName, "left_mount_aim_explicit");
        FCStringAnsi::Strcpy(ActionInfo.localizedActionName, "Left physical controller aim");
        if (!Check(xrCreateAction(ActionSet, &ActionInfo, &AimAction), TEXT("create aim action"))) return;

        // Spaces are created at OnBeginSession, after UE has suggested the
        // profile bindings and attached this action set to the session.
    }

    virtual bool GetSuggestedBindings(XrPath Profile, TArray<XrActionSuggestedBinding>& Bindings) override
    {
        if (!AimAction || !TouchProfiles.Contains(Profile)) return false;
        // Append to UE's profile bindings; never replace its trigger/buttons.
        // In particular do not bind this action to any hand-interaction profile.
        Bindings.Add({AimAction, HeldAim});
        Bindings.Add({AimAction, DetachedAim});
        UE_LOG(LogWallhackXR, Display, TEXT("Mounted controller: added held + detached aim bindings"));
        return true;
    }

    virtual void AttachActionSets(TSet<XrActionSet>& Sets) override
    {
        if (AimAction) Sets.Add(ActionSet);
    }

    virtual void GetActiveActionSetsForSync(TArray<XrActiveActionSet>& Sets) override
    {
        if (AimAction) Sets.Add({ActionSet, XR_NULL_PATH});
    }

    virtual const void* OnBeginSession(XrSession InSession, const void* Next) override
    {
        if (AimAction)
        {
            XrActionSpaceCreateInfo SpaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            SpaceInfo.action = AimAction;
            SpaceInfo.poseInActionSpace.orientation.w = 1.f;
            SpaceInfo.subactionPath = LeftHeld;
            if (!HeldSpace) Check(xrCreateActionSpace(InSession, &SpaceInfo, &HeldSpace), TEXT("create held aim space"));
            SpaceInfo.subactionPath = LeftDetached;
            if (!DetachedSpace) Check(xrCreateActionSpace(InSession, &SpaceInfo, &DetachedSpace), TEXT("create detached aim space"));
        }
        // Meta's plugin pauses multimodal tracking in PostCreateSession. Resume
        // here, after all plugins have finished their session initialization.
        if (DetachedSpace && ResumeTracking)
        {
            XrSimultaneousHandsAndControllersTrackingResumeInfoMETA Info{
                XR_TYPE_SIMULTANEOUS_HANDS_AND_CONTROLLERS_TRACKING_RESUME_INFO_META};
            bResumed = Check(ResumeTracking(InSession, &Info), TEXT("resume detached tracking"));
            UE_LOG(LogWallhackXR, Display, TEXT("Mounted controller: simultaneous tracking resumed=%d"), bResumed);
        }
        return Next;
    }

    virtual void OnDestroySession(XrSession InSession) override
    {
        if (HeldSpace) xrDestroySpace(HeldSpace);
        if (DetachedSpace) xrDestroySpace(DetachedSpace);
        if (ActionSet) xrDestroyActionSet(ActionSet); // also destroys AimAction
        HeldSpace = DetachedSpace = XR_NULL_HANDLE;
        ActionSet = XR_NULL_HANDLE;
        AimAction = XR_NULL_HANDLE;
        Session = XR_NULL_HANDLE;
        bResumed = false;
    }

    bool GetAim(FTransform& Out)
    {
        const bool bValid = ReadAim(Out);
#if !UE_BUILD_SHIPPING
        // Record classification/quality changes without continuous pose logging.
        // This preserves the release transition even before a console query.
        const uint64 Signature = (LastFlags << 4) | (bLastHeld ? 1 : 0)
            | (bLastDetached ? 2 : 0) | (bValid ? 4 : 0) | (bLocateCalled ? 8 : 0);
        if (Signature != LastLoggedSignature || LastLocateResult != LastLoggedResult)
        {
            UE_LOG(LogWallhackXR, Display, TEXT("Mounted pose change: held=%d detached=%d source=%s flags=%llu valid=%d estimated=%d locate_called=%d locate_result=%d world=%s"),
                bLastHeld, bLastDetached, LastSource, static_cast<unsigned long long>(LastFlags),
                bValid, bLastEstimated, bLocateCalled, int32(LastLocateResult), *Out.GetLocation().ToString());
            LastLoggedSignature = Signature; LastLoggedResult = LastLocateResult;
        }
#endif
        return bValid;
    }

    bool ReadAim(FTransform& Out)
    {
        Out = FTransform::Identity;
        LastFlags = 0; bLastHeld = bLastDetached = bLocateCalled = bLastEstimated = false;
        LastSource = TEXT("none"); LastLocateResult = XR_SUCCESS;
        const auto XR = GEngine ? GEngine->XRSystem : nullptr;
        IOpenXRHMD* HMD = XR ? XR->GetIOpenXRHMD() : nullptr;
        if (!IsInGameThread() || !HMD || !HMD->IsRunning() || !HMD->IsFocused()
            || !bResumed || !AimAction || HMD->GetSession() != Session) return false;
        const XrSpace TrackingSpace = HMD->GetTrackingSpace();
        const XrTime Time = HMD->GetDisplayTime();
        if (!TrackingSpace || Time <= 0) return false;

        XrActionStateGetInfo StateInfo{XR_TYPE_ACTION_STATE_GET_INFO};
        StateInfo.action = AimAction;
        XrActionStatePose HeldState{XR_TYPE_ACTION_STATE_POSE}, DetachedState{XR_TYPE_ACTION_STATE_POSE};
        StateInfo.subactionPath = LeftHeld;
        bLastHeld = XR_SUCCEEDED(xrGetActionStatePose(Session, &StateInfo, &HeldState)) && HeldState.isActive;
        StateInfo.subactionPath = LeftDetached;
        bLastDetached = XR_SUCCEEDED(xrGetActionStatePose(Session, &StateInfo, &DetachedState)) && DetachedState.isActive;
        const XrSpace ActiveSpace = bLastDetached ? DetachedSpace : bLastHeld ? HeldSpace : XR_NULL_HANDLE;
        if (!ActiveSpace) return false;
        LastSource = bLastDetached ? TEXT("detached") : TEXT("held");
        XrSpaceLocation Location{XR_TYPE_SPACE_LOCATION};
        bLocateCalled = true;
        LastLocateResult = xrLocateSpace(ActiveSpace, TrackingSpace, Time, &Location);
        if (XR_FAILED(LastLocateResult)) return false;
        LastFlags = Location.locationFlags;
        WallhackMountedController::FAimSample Sample;
        Sample.bFocused = true;
        Sample.bActive = true;
        Sample.bPositionValid = (LastFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
        Sample.bOrientationValid = (LastFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
        Sample.bPositionTracked = (LastFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) != 0;
        Sample.bOrientationTracked = (LastFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) != 0;
        Sample.TrackingPose = ToFTransform(Location.pose, XR->GetWorldToMetersScale());
        return WallhackMountedController::ResolveAvailable(Sample, XR->GetTrackingToWorldTransform(), Out,bLastEstimated);
    }

    FString GetDiagnostics()
    {
        FTransform Pose;
        const bool bValid = GetAim(Pose);
        XrInteractionProfileState DetachedProfile{XR_TYPE_INTERACTION_PROFILE_STATE};
        if (Session && AimAction)
        {
            xrGetCurrentInteractionProfile(Session, LeftDetached, &DetachedProfile);
        }
        const FString Profile = DetachedProfile.interactionProfile
            ? FOpenXRPath(DetachedProfile.interactionProfile).ToString() : TEXT("None");
        return FString::Printf(TEXT("supported=%d resumed=%d held=%d detached=%d source=%s flags=%llu valid=%d estimated=%d locate_called=%d locate_result=%d profile=%s world=%s"),
            bSupported, bResumed, bLastHeld, bLastDetached, LastSource,
            static_cast<unsigned long long>(LastFlags), bValid, bLastEstimated, bLocateCalled, int32(LastLocateResult), *Profile, *Pose.GetLocation().ToString());
    }

    bool IsEstimated() const {return bLastEstimated;}

private:
    bool Check(XrResult Result, const TCHAR* Operation)
    {
        if (XR_SUCCEEDED(Result)) return true;
        UE_LOG(LogWallhackXR, Warning, TEXT("Mounted controller: %s failed result=%d"), Operation, int32(Result));
        return false;
    }

    XrPath Path(const ANSICHAR* Name)
    {
        XrPath Result = XR_NULL_PATH;
        Check(xrStringToPath(Instance, Name, &Result), TEXT("resolve path"));
        return Result;
    }

    XrInstance Instance = XR_NULL_HANDLE;
    XrSession Session = XR_NULL_HANDLE;
    XrActionSet ActionSet = XR_NULL_HANDLE;
    XrAction AimAction = XR_NULL_HANDLE;
    XrSpace HeldSpace = XR_NULL_HANDLE, DetachedSpace = XR_NULL_HANDLE;
    XrPath LeftHeld = XR_NULL_PATH, LeftDetached = XR_NULL_PATH;
    XrPath HeldAim = XR_NULL_PATH, DetachedAim = XR_NULL_PATH;
    TArray<XrPath> TouchProfiles;
    PFN_xrResumeSimultaneousHandsAndControllersTrackingMETA ResumeTracking = nullptr;
    XrSpaceLocationFlags LastFlags = 0;
    const TCHAR* LastSource = TEXT("none");
    XrResult LastLocateResult = XR_SUCCESS, LastLoggedResult = XR_SUCCESS;
    uint64 LastLoggedSignature = MAX_uint64;
    bool bRigMode = false, bSupported = false, bResumed = false;
    bool bLastHeld = false, bLastDetached = false, bLocateCalled = false;
    bool bLastEstimated=false;
};
#else
class FWallhackXRModule final : public IModuleInterface {};
#endif

IMPLEMENT_MODULE(FWallhackXRModule, WallhackXR)

bool WallhackMountedController::GetAim(FTransform& WorldPose,bool& bEstimated)
{
    bEstimated=false;
#if PLATFORM_ANDROID
    if (auto* Module = FModuleManager::GetModulePtr<FWallhackXRModule>(TEXT("WallhackXR")))
    {const bool bValid=Module->GetAim(WorldPose);bEstimated=Module->IsEstimated();return bValid;}
#endif
    WorldPose = FTransform::Identity;
    return false;
}

FString WallhackMountedController::GetDiagnostics()
{
#if PLATFORM_ANDROID
    if (auto* Module = FModuleManager::GetModulePtr<FWallhackXRModule>(TEXT("WallhackXR")))
        return Module->GetDiagnostics();
#endif
    return TEXT("mounted controller extension inactive");
}
