#include "WallhackSensorPeopleActor.h"
#include "WallhackAnchorLifetime.h"
#include "WallhackPeopleRenderer.h"
#include "WallhackTelemetrySubsystem.h"
#include "WallhackVRPawn.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "WallhackPeopleStyle.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "IXRTrackingSystem.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/CoreDelegates.h"
#include "Misc/ScopeExit.h"
#include "UObject/ConstructorHelpers.h"
#include "ProceduralMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "Math/RotationMatrix.h"

AWallhackSensorPeopleActor::AWallhackSensorPeopleActor()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickGroup = TG_PostUpdateWork;
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("SensorReference")));
    AimMarker = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("FloorCursor"));
    AimMarker->SetupAttachment(GetRootComponent());
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
    AimMarker->SetStaticMesh(Sphere.Object);
    AimMarker->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    AimMarker->SetCastShadow(false);
    AimMarker->SetHiddenInGame(true);
    CalibrationGuides=CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("SensorOriginGuides"));
    CalibrationGuides->SetupAttachment(GetRootComponent());
    CalibrationGuides->SetAbsolute(true,true,true);
    CalibrationGuides->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    CalibrationGuides->SetCastShadow(false);CalibrationGuides->SetVisibility(false);
    static ConstructorHelpers::FObjectFinder<UMaterialInterface> GuideMaterial(TEXT("/Game/Materials/M_WallhackTrail.M_WallhackTrail"));
    if(GuideMaterial.Succeeded())
    {CalibrationGuides->SetMaterial(0,GuideMaterial.Object);AimMarker->SetMaterial(0,GuideMaterial.Object);}
}

void AWallhackSensorPeopleActor::BeginPlay()
{
    Super::BeginPlay();
#if !PLATFORM_ANDROID
    bPreview = FParse::Param(FCommandLine::Get(), TEXT("WallhackSensorPeoplePreview"));
#endif
    Renderer = GetWorld()->SpawnActor<AWallhackPeopleRenderer>();
    BackgroundHandle = FCoreDelegates::ApplicationWillEnterBackgroundDelegate.AddUObject(this, &AWallhackSensorPeopleActor::Suspend);
    ForegroundHandle = FCoreDelegates::ApplicationHasEnteredForegroundDelegate.AddUObject(this, &AWallhackSensorPeopleActor::Resume);
    RecenterHandle = FCoreDelegates::VRHeadsetRecenter.AddUObject(this, &AWallhackSensorPeopleActor::ResetPlacement);
}

bool AWallhackSensorPeopleActor::ViewerPose(FVector& Position, FQuat& Orientation) const
{
    if (bSuspended || bEnding) return false;
    if (!bPreview)
    {
        if (!GEngine || !GEngine->XRSystem.IsValid()) return false;
        const auto XR = GEngine->XRSystem;
        if (!XR->IsHeadTrackingAllowedForWorld(*GetWorld()) || !XR->IsTracking(IXRTrackingSystem::HMDDeviceId)
            || !XR->HasValidTrackingPosition()) return false;
    }
    const auto* PC = GetWorld()->GetFirstPlayerController();
    const APlayerCameraManager* Camera = PC ? PC->PlayerCameraManager.Get() : nullptr;
    if (!Camera) return false;
    Position = Camera->GetCameraLocation(); Orientation = Camera->GetCameraRotation().Quaternion();
    return !Position.ContainsNaN() && !Orientation.ContainsNaN();
}

bool AWallhackSensorPeopleActor::AimFloor(FVector& Point) const
{
    FVector Viewer; FQuat Orientation;
    if (!ViewerPose(Viewer, Orientation)) return false;
    const auto* PC = GetWorld()->GetFirstPlayerController();
    const auto* Pawn = PC ? Cast<AWallhackVRPawn>(PC->GetPawn()) : nullptr;
    FVector RayOrigin, Direction;
    if (!Pawn || !Pawn->GetNavigationAim(RayOrigin, Direction)) return false;
    float FloorZ = 0;
    if (!bPreview && GEngine && GEngine->XRSystem.IsValid()) FloorZ = GEngine->XRSystem->GetTrackingToWorldTransform().GetLocation().Z;
    const float Scale = GetWorld()->GetWorldSettings()->WorldToMeters;
    return Scale > 0 && WallhackSensorPeopleMath::FloorAim(RayOrigin, Direction, FloorZ, Point)
        && FVector::Distance(RayOrigin, Point) <= 20 * Scale;
}

void AWallhackSensorPeopleActor::ConfirmPlacement()
{
    if (bPending || bHidden || RegistrationKey.IsEmpty() || bSuspended) return;
    if(bReady){if(bControllerRig)ShowGuidesUntil=FPlatformTime::Seconds()+6;return;}
    if(bControllerRig)
    {
        FTransform Left,Right;FVector Viewer;FQuat View;
        if(!ViewerPose(Viewer,View)){Status=TEXT("HEAD TRACKING UNAVAILABLE");return;}
        if(!GripPose(true,Left)){Status=TEXT("ALIGNMENT / LEFT CONTROLLER POSITION UNAVAILABLE");return;}
        FString Error;
        if(CalibrationCapture.IsReady())
        {
            if(!RigAlignment.SetMount(CalibrationCapture.GetMount(),Error)){Status=Error;return;}
            bMountEstimated=CalibrationCapture.IsEstimated();
            bReady=true;CalibrationCapture.Reset();ShowGuidesUntil=FPlatformTime::Seconds()+6;
            RigHistory.Reset();WorldTracks.Reset();Positions.Reset();
            Status=TEXT("SENSOR ORIGIN SET / COLLECTING FRESH SAMPLES");return;
        }
        if(CalibrationCapture.IsActive())return;
        if(!GripPose(false,Right)){Status=TEXT("ALIGNMENT / RIGHT CONTROLLER POSITION UNAVAILABLE");return;}
        FWallhackRigAlignment Check;
        if(!Check.Align(Right.GetLocation(),Left,Error)){Status=Error;return;}
        CalibrationCapture.Start();
        Status=TEXT("CAPTURING ORIGIN / HOLD STILL");
        return;
    }
    FVector Point;
    if (!AimFloor(Point)) { Status = TEXT("POINT AT THE FLOOR"); return; }
    if (PlacementStep == 0)
    {
        Origin = Point; PlacementStep = 1;
        Status = TEXT("MARK FORWARD / AT LEAST 0.5 M FROM ORIGIN");
        return;
    }
    FVector Direction = Point - Origin; Direction.Z = 0;
    if (Direction.Size() < .5 * GetWorld()->GetWorldSettings()->WorldToMeters)
    { Status = TEXT("FORWARD POINT MUST BE AT LEAST 0.5 M AWAY"); return; }
    Forward = Direction.GetSafeNormal();
    CreateReference();
}

void AWallhackSensorPeopleActor::CreateReference()
{
    ReleaseReference();
    ++Generation;
    SetActorLocationAndRotation(Origin, Forward.Rotation());
    if (bPreview) { bReady = true; Status = TEXT("DESKTOP REGISTRATION / NOT HEADSET VALIDATION"); return; }
#if PLATFORM_ANDROID
    bPending = true; RequestedAt = FPlatformTime::Seconds(); Status = TEXT("CREATING SENSOR ANCHOR");
    const uint32 Expected = Generation;
    const TWeakObjectPtr<AWallhackSensorPeopleActor> WeakThis(this);
    auto Callback = FOculusXRSpatialAnchorCreateDelegate::CreateLambda(
        [WeakThis, Expected](EOculusXRAnchorResult::Type Result, UOculusXRAnchorComponent* Created)
        {
            auto* Self = WeakThis.Get();
            if (!Self || Self->bEnding || Expected != Self->Generation) { DestroyWallhackAnchor(Created); return; }
            Self->bPending = false;
            if (!UOculusXRAnchorBPFunctionLibrary::IsAnchorResultSuccess(Result) || !IsValid(Created) || !Created->HasValidHandle())
            { DestroyWallhackAnchor(Created); Self->Status = TEXT("ANCHOR FAILED / A TO RETRY"); return; }
            Self->Anchor = Created;
            Self->AddTickPrerequisiteComponent(Created);
            Self->bReady = true;
            Self->Status = TEXT("LOCALIZING SENSOR ANCHOR");
        });
    EOculusXRAnchorResult::Type Result = EOculusXRAnchorResult::Failure;
    if (!OculusXRAnchors::FOculusXRAnchors::CreateSpatialAnchor(GetActorTransform(), this, Callback, Result))
    { bPending = false; Status = TEXT("ANCHOR FAILED / A TO RETRY"); }
#else
    Status = TEXT("ANCHOR REQUIRES QUEST / USE EXPLICIT DESKTOP PREVIEW");
#endif
}

bool AWallhackSensorPeopleActor::ReferencePose(FTransform& Out,bool* bEstimated) const
{
    if(bEstimated)*bEstimated=false;
    if (!bReady) return false;
    if(bControllerRig)
    {
        FTransform Controller;
        if(!GripPose(true,Controller,bEstimated))return false;
        Out=RigAlignment.Resolve(Controller);
        Out.SetLocation(Out.GetLocation()*GetWorld()->GetWorldSettings()->WorldToMeters);
        return true;
    }
    if (bPreview) { Out = GetActorTransform(); return true; }
#if PLATFORM_ANDROID
    auto* Component = Cast<UOculusXRAnchorComponent>(Anchor.Get());
    if (!IsValid(Component) || !Component->HasValidHandle()) return false;
    FOculusXRAnchorLocationFlags Flags;
    return UOculusXRAnchorBPFunctionLibrary::TryGetAnchorTransformByHandle(Component->GetHandle(), Out, Flags, EOculusXRAnchorSpace::World)
        && Flags.IsValid() && !Out.ContainsNaN();
#else
    return false;
#endif
}

bool AWallhackSensorPeopleActor::GripPose(bool bLeft,FTransform& Out,bool* bEstimated) const
{
    if(bEstimated)*bEstimated=false;
    const auto* PC=GetWorld()->GetFirstPlayerController();
    const auto* Pawn=PC?Cast<AWallhackVRPawn>(PC->GetPawn()):nullptr;
    const float Scale=GetWorld()->GetWorldSettings()->WorldToMeters;
    if(!Pawn||!FMath::IsFinite(Scale)||Scale<=0)return false;
    if(bLeft?!Pawn->GetSensorRigAim(Out,bEstimated):!Pawn->GetSensorCalibrationProbe(Out))return false;
    Out.SetLocation(Out.GetLocation()/Scale);Out.SetScale3D(FVector::OneVector);
    return true;
}

FString AWallhackSensorPeopleActor::AlignmentPrompt() const
{
    if(!bControllerRig)return PlacementStep==0?TEXT("MARK FLOOR BELOW RADAR / RIGHT TRIGGER")
        :TEXT("MARK FORWARD / AT LEAST 0.5 M FROM ORIGIN");
    if(CalibrationCapture.IsReady())return TEXT("CHECK ORIGIN AND FORWARD / TRIGGER TO ACCEPT");
    if(CalibrationCapture.IsActive())return TEXT("CAPTURING ORIGIN / HOLD STILL");
    return TEXT("PLACE WHITE PROBE ON RADAR / TRIGGER TO CAPTURE");
}

FWallhackRigCalibrationView AWallhackSensorPeopleActor::GetCalibrationView() const
{
    FWallhackRigCalibrationView View;
    View.bControllerRig=bControllerRig;View.bTracked=bRigTracked;
    View.bEstimated=bRigEstimated||bMountEstimated||CalibrationCapture.IsEstimated();
    View.Step=bReady?4:CalibrationCapture.IsReady()?3:CalibrationCapture.IsActive()?2:1;
    View.Progress=CalibrationCapture.Progress();
    if(bReady)View.OffsetCm=RigAlignment.GetMount().GetLocation()*100;
    else if(CalibrationCapture.IsReady())View.OffsetCm=CalibrationCapture.GetMount().GetLocation()*100;
    return View;
}

void AWallhackSensorPeopleActor::PresentCalibrationGuides(const FTransform& SensorMetres)
{
    const float Scale=GetWorld()->GetWorldSettings()->WorldToMeters;
    if(SensorMetres.ContainsNaN()||!FMath::IsFinite(Scale)||Scale<=0)return;
    if(!CalibrationGuides->GetProcMeshSection(0))
    {
        const auto Geometry=BuildWallhackRigCalibrationGuides(FTransform::Identity);
        CalibrationGuides->CreateMeshSection_LinearColor(0,Geometry.Vertices,Geometry.Indices,{}, {},Geometry.Colors,{},false);
    }
    CalibrationGuides->SetWorldTransform(FTransform(SensorMetres.GetRotation(),SensorMetres.GetLocation()*Scale,FVector(Scale)));
    CalibrationGuides->SetVisibility(true);
}

void AWallhackSensorPeopleActor::HidePeople()
{
    Views.Reset(); RadarViews.Reset();
    if (Renderer) Renderer->SetActorHiddenInGame(true);
}

void AWallhackSensorPeopleActor::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    Views.Reset();RadarViews.Reset();AimMarker->SetHiddenInGame(true);
    CalibrationGuides->SetVisibility(false);bRigTracked=bRigEstimated=false;
    bool bPresented=false;
    // Only hide on a failed/disabled frame. Hiding then showing on every valid
    // tick recreates the skeletal render object and loses its GPU bone upload.
    ON_SCOPE_EXIT {if(!bPresented)HidePeople();};
    if (bEnding || bSuspended) return;
    auto* Telemetry = GetGameInstance() ? GetGameInstance()->GetSubsystem<UWallhackTelemetrySubsystem>() : nullptr;
    bool bTrackedReference=false;
    ON_SCOPE_EXIT
    {
        if(Telemetry&&bControllerRig)
        {
            const bool Estimated=bRigEstimated||bMountEstimated||CalibrationCapture.IsEstimated();
            Telemetry->ReportControllerRig(bReady,bTrackedReference&&!Estimated,
                Estimated?TEXT("ESTIMATED RIG / ")+Status:Status);
        }
    };
    const auto Frame = Telemetry ? Telemetry->GetSensorPeopleFrame() : FWallhackSensorPeopleFrame{};
    bReplay = Frame.bReplay; Unpositioned = Frame.Unpositioned;
    const bool ControllerMode=Frame.RigMotionMode==TEXT("left_controller");
    if(ControllerMode!=bControllerRig){bControllerRig=ControllerMode;ResetPlacement();}
    if (!Frame.RegistrationKey.IsEmpty() && Frame.RegistrationKey != RegistrationKey)
    { RegistrationKey = Frame.RegistrationKey; ResetPlacement(); }
    if (RegistrationKey.IsEmpty()) { Status = TEXT("WAITING FOR GROUND STATION"); return; }
    if(!Frame.bRigPoseValid&&!bControllerRig)
    {
        if(bReady||bPending||PlacementStep>0)ResetPlacement();
        Positions.Reset();Status=TEXT("RIG MOVING / TRACKED RIG POSE REQUIRED");return;
    }
    if (bPending && FPlatformTime::Seconds() - RequestedAt > 20)
    { ResetPlacement(); Status = TEXT("ANCHOR TIMED OUT / A TO RETRY"); return; }
    FVector Viewer; FQuat Orientation;
    if (!ViewerPose(Viewer, Orientation))
    { Positions.Reset();RigHistory.Reset();WorldTracks.Reset();CalibrationCapture.LoseTracking();Status = TEXT("HEAD TRACKING UNAVAILABLE");return; }
    if (bHidden) return;
    const float Scale = GetWorld()->GetWorldSettings()->WorldToMeters;
    if (!FMath::IsFinite(Scale) || Scale <= 0) return;
    if (!bReady)
    {
        if(bControllerRig)
        {
            FTransform Left,Right;
            if(!GripPose(true,Left,&bRigEstimated)){CalibrationCapture.LoseTracking();Status=TEXT("ALIGNMENT / LEFT CONTROLLER POSITION UNAVAILABLE");return;}
            bRigTracked=!bRigEstimated;
            if(CalibrationCapture.IsReady())
            {
                PresentCalibrationGuides(CalibrationCapture.GetMount()*Left);
                Status=AlignmentPrompt();return;
            }
            if(!GripPose(false,Right)){CalibrationCapture.LoseTracking();Status=TEXT("ALIGNMENT / RIGHT CONTROLLER POSITION UNAVAILABLE");return;}
            // Keep the probe visible even when it is too far from the mount to
            // accept. The user needs to see the point they are positioning.
            AimMarker->SetWorldLocation(Right.GetLocation()*Scale);
            AimMarker->SetWorldScale3D(FVector(.012*Scale/100));
            AimMarker->SetHiddenInGame(false);
            if(CalibrationCapture.IsActive())CalibrationCapture.Observe(FPlatformTime::Seconds(),Left,Right.GetLocation(),Status,bRigEstimated);
            else Status=AlignmentPrompt();
            FWallhackRigAlignment Preview;FString Error;
            if(Preview.Align(Right.GetLocation(),Left,Error))
                PresentCalibrationGuides(CalibrationCapture.IsReady()?CalibrationCapture.GetMount()*Left:Preview.Resolve(Left));
            else Status=Error;
            return;
        }
        if (Status == TEXT("HEAD TRACKING UNAVAILABLE"))
            Status = bPending ? TEXT("CREATING SENSOR ANCHOR") : PlacementStep == 0
                ? TEXT("MARK FLOOR BELOW RADAR / RIGHT TRIGGER") : TEXT("MARK FORWARD / AT LEAST 0.5 M FROM ORIGIN");
        FVector Aim;
        if (!bPending && AimFloor(Aim))
        {
            AimMarker->SetWorldLocation(Aim + FVector(0,0,.02 * Scale));
            AimMarker->SetWorldScale3D(FVector(.05 * Scale / 100));
            AimMarker->SetHiddenInGame(false);
        }
        return;
    }
    FTransform Reference;
    if (!ReferencePose(Reference,&bRigEstimated))
    {
        Positions.Reset();RigHistory.Reset();WorldTracks.Reset();
        Status=bControllerRig?TEXT("LEFT CONTROLLER POSITION LOST / FIGURES PAUSED"):TEXT("SENSOR ANCHOR NOT LOCALIZED");return;
    }
    const double Now=FPlatformTime::Seconds();
    bTrackedReference=true;
    if(bControllerRig)
    {
        bRigTracked=!bRigEstimated;
        FTransform Metres=Reference;Metres.SetLocation(Reference.GetLocation()/Scale);
        if(Now<ShowGuidesUntil)PresentCalibrationGuides(Metres);
        RigHistory.Add(Now,Metres,bRigEstimated||bMountEstimated);
        if(bReplay){Status=TEXT("REPLAY HAS NO RECORDED CONTROLLER POSE");return;}
        if(!Frame.bHasTracks){Status=TEXT("TRACKED RIG REQUIRES CURRENT RELAY");return;}
    }
    Status = Frame.RegistrationKey.IsEmpty() ? TEXT("GROUND LINK DISCONNECTED")
        : Frame.People.IsEmpty() && Frame.Radar.IsEmpty() ? TEXT("NO FRESH POSITIONED CONTACTS") : TEXT("SENSOR CONTACTS LIVE");
    TArray<FWallhackPersonPose> Poses;
    Positions.BeginFrame(Now);
    if(Frame.bHasTracks)
    {
        Status=Frame.Tracks.IsEmpty()?TEXT("NO FRESH POSITIONED CONTACTS"):TEXT("SENSOR CONTACTS LIVE");
        for(const auto& Person:Frame.Tracks)
        {
            const FString Key=FString::Printf(TEXT("F/%d"),Person.Id);
            FVector Feet;FTransform SampleReference=Reference;FVector WorldVelocity=FVector::ZeroVector;
            if(bControllerRig)
            {
                auto& Cached=WorldTracks.FindOrAdd(Person.Id);
                if(Cached.Sample!=Person.SampleKey)
                {
                    FTransform AtObservation;
                    bool Estimated=false;
                    if(!RigHistory.Sample(Person.ObservedAt,AtObservation,&Estimated))continue;
                    // A 2D radar return has no measured elevation. The displayed
                    // feet use the session floor; the label preserves that uncertainty.
                    const FVector Point=AtObservation.TransformPosition(FVector(Person.Position.Y,Person.Position.X,0));
                    const double Dt=Person.ObservedAt-Cached.At;
                    FVector Velocity=FVector::ZeroVector;
                    if(Dt>.025&&Dt<.35&&!Cached.Sample.IsEmpty()&&!Estimated&&!Cached.bRigEstimated)
                    {
                        Velocity=(Point-Cached.Position)/Dt;Velocity.Z=0;
                        Velocity=Velocity.Size()<4?FMath::Lerp(Cached.Velocity,Velocity,.35):FVector::ZeroVector;
                    }
                    Cached.Sample=Person.SampleKey;Cached.Position=Point;Cached.Velocity=Velocity;
                    Cached.At=Person.ObservedAt;Cached.Reference=AtObservation;
                    Cached.bRigEstimated=Estimated;
                }
                Cached.LastSeen=Now;WorldVelocity=Cached.Velocity;SampleReference=Cached.Reference;
                const FVector2D XY=Positions.Sample(Key,{Cached.Position.X,Cached.Position.Y});
                const float FloorZ=GEngine&&GEngine->XRSystem.IsValid()
                    ?GEngine->XRSystem->GetTrackingToWorldTransform().GetLocation().Z:0;
                Feet={XY.X*Scale,XY.Y*Scale,FloorZ};
            }
            else Feet=WallhackSensorPeopleMath::ToWorld(Positions.Sample(Key,Person.Position),Reference,Scale);
            FWallhackPersonPose Pose;Pose.Id=Person.Id;Pose.Feet=Feet/Scale;Pose.Height=Person.Height;
            // Sensor facing is clockwise from forward; UE yaw has the same convention.
            const FVector Facing=SampleReference.TransformVectorNoScale(FRotator(0,Person.Facing,0).Vector());
            Pose.Facing=Facing.Rotation().Yaw;
            Pose.bArticulated=true;Pose.bCameraPose=Person.Joints.Num()==33;
            Pose.bHeightEstimated=Person.HeightSource!=TEXT("assumed");
            Pose.Speed=Person.Velocity.Size();Pose.bRadarOnly=Person.Source==TEXT("radar_only");
            Pose.LocalVelocity=FRotator(0,-Person.Facing,0).RotateVector(FVector(Person.Velocity.Y,Person.Velocity.X,0));
            Pose.SourceLabel=Pose.bCameraPose?TEXT("CAMERA POSE"):TEXT("ESTIMATED POSE");
            if(Pose.bRadarOnly)Pose.SourceLabel=TEXT("RADAR / EST. POSE");
            FTransform PoseReference=SampleReference;
            bool bPoseRigEstimated=false;
            if(bControllerRig&&!RigHistory.Sample(Person.PoseObservedAt,PoseReference,&bPoseRigEstimated))Pose.bCameraPose=false;
            if(bControllerRig)
            {
                auto& Cached=WorldTracks[Person.Id];Pose.Speed=WorldVelocity.Size();
                if(Pose.bCameraPose&&Person.FacingSource==TEXT("camera"))
                    Pose.Facing=PoseReference.TransformVectorNoScale(FRotator(0,Person.Facing,0).Vector()).Rotation().Yaw;
                else if(Pose.Speed>.18)Pose.Facing=WorldVelocity.Rotation().Yaw;
                else if(Cached.bHasFacing)Pose.Facing=Cached.Facing;
                Cached.Facing=Pose.Facing;Cached.bHasFacing=true;
                Pose.LocalVelocity=FRotator(0,-Pose.Facing,0).RotateVector(WorldVelocity);
            }
            if(Pose.bCameraPose)for(const auto& Joint:Person.Joints)
                Pose.Joints.Add(bControllerRig?FRotator(0,-Pose.Facing,0).RotateVector(
                    PoseReference.TransformVectorNoScale(FRotator(0,Person.PoseYaw,0).RotateVector(Joint)))
                    :FRotator(0,Person.PoseYaw-Person.Facing,0).RotateVector(Joint));
            if(!Pose.bCameraPose&&bControllerRig)Pose.SourceLabel=Pose.bRadarOnly?TEXT("RADAR / EST. POSE"):TEXT("ESTIMATED POSE");
            if(bControllerRig)
            {
                const bool Estimated=WorldTracks[Person.Id].bRigEstimated||bRigEstimated||bMountEstimated||bPoseRigEstimated;
                Pose.SourceLabel+=Estimated?TEXT(" / EST. RIG + FLOOR"):TEXT(" / EST. FLOOR");
                if(Estimated){Pose.Speed=0;Pose.LocalVelocity=FVector::ZeroVector;}
            }
            Pose.JointQuality=Person.JointQuality;
            FWallhackSensorPersonView View;View.Id=Person.Id;View.bFused=true;
            View.bRadar=Person.Source!=TEXT("camera_estimate");View.bRadarOnly=Pose.bRadarOnly;
            View.Feet=Feet;View.Color=WallhackPeopleStyle::Color(Pose);
            if(WallhackSpatialMath::ProjectContact(Feet,Viewer,Orientation.Rotator().Yaw,Scale,View.View))Views.Add(View);
            Poses.Add(MoveTemp(Pose));
        }
    }
    else for (const auto& Person : Frame.People)
    {
        const FString Key = FString::Printf(TEXT("C/%d/%d"), Person.CameraGeneration, Person.Id);
        const FVector Feet = WallhackSensorPeopleMath::ToWorld(Positions.Sample(Key, Person.Position), Reference, Scale);
        FWallhackSensorPersonView View; View.Id = Person.Id; View.bRadar = Person.bRadar; View.Feet = Feet;
        if (!WallhackSpatialMath::ProjectContact(Feet, Viewer, Orientation.Rotator().Yaw, Scale, View.View)) continue;
        Views.Add(View);
        FWallhackPersonPose Pose; Pose.Id = Person.Id; Pose.Feet = Feet / Scale; Pose.Height = 1.65f;
        Pose.Facing = (Reference.GetLocation() - Feet).Rotation().Yaw;
        Pose.Tint = Person.bRadar ? FLinearColor(.25f,.9f,.35f,1) : FLinearColor(1,.62f,.08f,1);
        Pose.SourceLabel = Person.bRadar ? TEXT("RADAR") : TEXT("ESTIMATED");
        Poses.Add(Pose);
    }
    // The relay supplies unmatched radar returns separately. They have their own
    // freshness deadline and do not need a camera detection or alignment match.
    // A generic body is a display assumption, not a camera-confirmed person.
    if(!Frame.bHasTracks)for (const auto& Dot : Frame.Radar)
    {
        const FString Key = FString::Printf(TEXT("R/%d/%d"), Dot.Generation, Dot.Id);
        const FVector Feet = WallhackSensorPeopleMath::ToWorld(Positions.Sample(Key, Dot.Position), Reference, Scale);
        FWallhackSensorPersonView View;
        View.Id = Dot.Id; View.bRadar = true; View.bRadarOnly = true; View.Feet = Feet;
        if (!WallhackSpatialMath::ProjectContact(Feet, Viewer, Orientation.Rotator().Yaw, Scale, View.View)) continue;
        if (Poses.Num() >= UWallhackPeopleSubsystem::MaxPeople)
        {
            RadarViews.Add(View.View); // Keep overflow on the map without exceeding the body budget.
            continue;
        }
        Views.Add(View);
        FWallhackPersonPose Pose; Pose.Id = Dot.Id; Pose.bRadarOnly = true;
        Pose.Feet = Feet / Scale; Pose.Height = 1.65f;
        Pose.Facing = (Reference.GetLocation() - Feet).Rotation().Yaw;
        Pose.Tint = FLinearColor(.35f,.7f,1,1);
        Pose.SourceLabel = TEXT("RADAR ONLY");
        Poses.Add(Pose);
    }
    Positions.EndFrame();
    for(auto It=WorldTracks.CreateIterator();It;++It)if(Now-It.Value().LastSeen>.75)It.RemoveCurrent();
    if(bControllerRig)Status=bRigEstimated||bMountEstimated?TEXT("LEFT RIG AVAILABLE / ESTIMATED POSITION")
        :Poses.IsEmpty()?TEXT("LEFT RIG TRACKED / WAITING FOR FRESH CONTACTS")
        :TEXT("LEFT RIG TRACKED / ESTIMATED FLOOR POSITIONS");
    // Reuse the stereo-tested corner labels with the actual viewer pose.
    // Separate TextRender labels would duplicate telemetry and use a different
    // mobile translucency path from the fixed manual-person renderer.
    if (Renderer) Renderer->Present(Poses, INDEX_NONE, nullptr, true, Scale,
        Viewer / Scale, Orientation, 0, FPlatformTime::Seconds());
    bPresented=Renderer!=nullptr;
}

void AWallhackSensorPeopleActor::ReleaseReference()
{
    bReady = false;
    if (auto* Component = Anchor.Get())
    {
        RemoveTickPrerequisiteComponent(Component);
#if PLATFORM_ANDROID
        DestroyWallhackAnchor(Cast<UOculusXRAnchorComponent>(Component));
#endif
    }
    Anchor.Reset();
}

void AWallhackSensorPeopleActor::ResetPlacement()
{
    ++Generation; bPending = false; PlacementStep = 0;
    ReleaseReference(); HidePeople(); Positions.Reset();
    RigAlignment.Reset();RigHistory.Reset();WorldTracks.Reset();
    CalibrationCapture.Reset();bRigTracked=bRigEstimated=bMountEstimated=false;ShowGuidesUntil=0;
    CalibrationGuides->SetVisibility(false);
    Status = AlignmentPrompt();
}

void AWallhackSensorPeopleActor::SetPresentationHidden(bool Hidden)
{ bHidden = Hidden; if (Hidden) { HidePeople(); Positions.Reset(); CalibrationCapture.LoseTracking(); AimMarker->SetHiddenInGame(true);CalibrationGuides->SetVisibility(false); } }
void AWallhackSensorPeopleActor::Suspend() { bSuspended = true; ResetPlacement(); }
void AWallhackSensorPeopleActor::Resume() { bSuspended = false; ResetPlacement(); }
void AWallhackSensorPeopleActor::EndPlay(const EEndPlayReason::Type Reason)
{
    bEnding = true; ResetPlacement();
    FCoreDelegates::ApplicationWillEnterBackgroundDelegate.Remove(BackgroundHandle);
    FCoreDelegates::ApplicationHasEnteredForegroundDelegate.Remove(ForegroundHandle);
    FCoreDelegates::VRHeadsetRecenter.Remove(RecenterHandle);
    if (Renderer) Renderer->Destroy();
    Super::EndPlay(Reason);
}
