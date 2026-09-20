#pragma once

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR && !PLATFORM_ANDROID

#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EnhancedInputComponent.h"
#include "InputAction.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/CommandLine.h"
#include "WallhackVRHUDActor.h"
#include "WallhackVRPawn.h"
#include "WallhackWorldContact.h"

/**
 * Actual game actors in an isolated world, without GameMode, sockets, or an HMD.
 * The existing opt-in desktop source supplies the contact; no production hooks
 * or private state injection are used. The camera component and camera manager
 * remain the production path for the viewer pose.
 */
class FScopedWallhackRuntimeWorld
{
private:
    FString SavedCommandLine;

public:
    UWorld* World = nullptr;
    APlayerController* Controller = nullptr;
    AWallhackVRPawn* Pawn = nullptr;
    AWallhackWorldContact* Contact = nullptr;
    AWallhackVRHUDActor* HUD = nullptr;

    explicit FScopedWallhackRuntimeWorld(float WorldToMeters = 100.f, bool bCreateViewer = true)
        : SavedCommandLine(FCommandLine::Get())
    {
        // Preview controls consult this switch during Tick and HUD creation,
        // so keep it active for the fixture's entire synchronous lifetime.
        FCommandLine::Append(TEXT(" -WallhackTrackingPreview"));
        if (!GEngine) return;

        const UWorld::InitializationValues Options = UWorld::InitializationValues()
            .AllowAudioPlayback(false)
            .RequiresHitProxies(false)
            .CreatePhysicsScene(false)
            .CreateNavigation(false)
            .CreateAISystem(false)
            .ShouldSimulatePhysics(false)
            .SetTransactional(false);
        World = UWorld::CreateWorld(EWorldType::Game, false,
            MakeUniqueObjectName(GetTransientPackage(), UWorld::StaticClass(), TEXT("WallhackRuntimeTest")),
            nullptr, true, ERHIFeatureLevel::Num, &Options);
        if (!World) return;

        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        World->GetWorldSettings()->WorldToMeters = WorldToMeters;
        World->InitializeActorsForPlay(FURL());

        FActorSpawnParameters Spawn;
        Spawn.ObjectFlags |= RF_Transient;
        Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        if (bCreateViewer)
        {
            Controller = World->SpawnActor<APlayerController>(Spawn);
            Pawn = World->SpawnActor<AWallhackVRPawn>(Spawn);
            if (Controller && Pawn && Controller->PlayerCameraManager)
            {
                if (UCameraComponent* Camera = Pawn->FindComponentByClass<UCameraComponent>())
                {
                    // An attached editor headset must not override a scripted
                    // test pose. Real Quest builds retain bLockToHmd=true.
                    Camera->bLockToHmd = false;
                    Camera->bUsePawnControlRotation = false;
                }
                Controller->bAutoManageActiveCameraTarget = false;
                Controller->PlayerCameraManager->bUseClientSideCameraUpdates = false;
                Controller->Possess(Pawn);
                Controller->SetViewTarget(Pawn);
                Controller->DispatchBeginPlay();
                Pawn->DispatchBeginPlay();
                SetViewerPose(FVector(0., 0., 170.), FRotator::ZeroRotator);
            }
        }

        Contact = World->SpawnActor<AWallhackWorldContact>(Spawn);
        if (Contact)
        {
            Contact->DispatchBeginPlay();
            // Baseline tests isolate contact 01. Multi-contact suites enable
            // the product's default richer simulation explicitly.
            Contact->SetMultipleContactsEnabled(false);
        }
    }

    ~FScopedWallhackRuntimeWorld()
    {
        if (World)
        {
            // Dispatch EndPlay for actors explicitly begun without a GameMode.
            if (HUD && HUD->HasActorBegunPlay()) HUD->RouteEndPlay(EEndPlayReason::EndPlayInEditor);
            if (Contact && Contact->HasActorBegunPlay()) Contact->RouteEndPlay(EEndPlayReason::EndPlayInEditor);
            if (Pawn && Pawn->HasActorBegunPlay()) Pawn->RouteEndPlay(EEndPlayReason::EndPlayInEditor);
            if (Controller && Controller->HasActorBegunPlay()) Controller->RouteEndPlay(EEndPlayReason::EndPlayInEditor);
            World->DestroyWorld(false);
            GEngine->DestroyWorldContext(World);
        }
        // Restore even if world creation or fixture setup failed early.
        FCommandLine::Set(*SavedCommandLine);
    }

    FScopedWallhackRuntimeWorld(const FScopedWallhackRuntimeWorld&) = delete;
    FScopedWallhackRuntimeWorld& operator=(const FScopedWallhackRuntimeWorld&) = delete;

    bool HasViewer() const
    {
        return World && Controller && Pawn && Controller->PlayerCameraManager;
    }

    /** Reach an explicit density through the actual bound controller action. */
    bool SetHUDDensity(EWallhackHUDDensity Density)
    {
        if (!HUD) return false;
        for (int32 Attempt = 0; Attempt < 3 && HUD->GetHUDDensity() != Density; ++Attempt)
        {
            const UEnhancedInputComponent* Input = HUD->FindComponentByClass<UEnhancedInputComponent>();
            if (!Input) return false;
            bool bInvoked = false;
            for (const auto& Binding : Input->GetActionEventBindings())
            {
                if (Binding->GetAction() && Binding->GetAction()->GetFName() == FName(TEXT("IA_CycleHUD"))
                    && Binding->GetTriggerEvent() == ETriggerEvent::Started)
                {
                    Binding->Execute(FInputActionInstance(Binding->GetAction()));
                    bInvoked = true;
                    break;
                }
            }
            if (!bInvoked) return false;
        }
        return HUD->GetHUDDensity() == Density;
    }

    void SetViewerPose(const FVector& EyePosition, const FRotator& Orientation)
    {
        if (!HasViewer()) return;
        Pawn->SetActorLocationAndRotation(EyePosition, Orientation, false, nullptr, ETeleportType::TeleportPhysics);
        Controller->SetControlRotation(Orientation);
        Controller->SetViewTarget(Pawn);
        Controller->UpdateCameraManager(0.016f);
    }

    /** Advance the actual camera/contact behavior deterministically, no wall-clock wait. */
    void AdvanceContact(float Seconds)
    {
        while (Contact && Seconds > UE_SMALL_NUMBER)
        {
            const float Step = FMath::Min(Seconds, 0.1f);
            if (Controller && Controller->PlayerCameraManager) Controller->UpdateCameraManager(Step);
            Contact->Tick(Step);
            Seconds -= Step;
        }
    }

    AWallhackVRHUDActor* SpawnHUD()
    {
        if (!World || !Contact || HUD) return HUD;
        FActorSpawnParameters Spawn;
        Spawn.ObjectFlags |= RF_Transient;
        Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        HUD = World->SpawnActor<AWallhackVRHUDActor>(Spawn);
        if (HUD)
        {
            HUD->SetWorldContact(Contact);
            HUD->DispatchBeginPlay();
        }
        return HUD;
    }
};

#endif
