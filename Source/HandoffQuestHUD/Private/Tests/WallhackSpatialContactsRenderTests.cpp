#include "WallhackRuntimeTestWorld.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR && !PLATFORM_ANDROID

#include "AssetCompilingManager.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/Canvas.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EnhancedInputComponent.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "InputAction.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RenderingThread.h"
#include "WallhackHUD.h"
#include "WallhackHUDProjection.h"
#include "WallhackSpatialMath.h"

namespace
{
    bool IsContactGreen(const FColor& Pixel, bool bRequireAlpha = true)
    {
        return Pixel.G > 130 && Pixel.G > Pixel.R * 1.2f && Pixel.G > Pixel.B * 1.3f
            && (!bRequireAlpha || Pixel.A > 100);
    }

    int32 CountContactGreen(const TArray<FColor>& Pixels, int32 Width, int32 Height,
        int32 Left, int32 Top, int32 Right, int32 Bottom, bool bRequireAlpha = true)
    {
        int32 Count = 0;
        for (int32 Y = FMath::Max(Top, 0); Y <= FMath::Min(Bottom, Height - 1); ++Y)
            for (int32 X = FMath::Max(Left, 0); X <= FMath::Min(Right, Width - 1); ++X)
                if (IsContactGreen(Pixels[Y * Width + X], bRequireAlpha)) ++Count;
        return Count;
    }

    bool SaveContactImage(const TArray<FColor>& Pixels, int32 Width, int32 Height, const TCHAR* Name)
    {
        const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("TrackingVerification/LocalRender"));
        IFileManager::Get().MakeDirectory(*Directory, true);
        TArray64<uint8> PNG;
        FImageUtils::PNGCompressImageArray(Width, Height, Pixels, PNG);
        return FFileHelper::SaveArrayToFile(PNG, *FPaths::Combine(Directory, FString(Name) + TEXT(".png")));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWallhackMultipleContactsRenderTest,
    "Wallhack.Spatial.Render.MultipleContactsStaleAndRecovery",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FWallhackMultipleContactsRenderTest::RunTest(const FString& Parameters)
{
    FScopedWallhackRuntimeWorld Fixture;
    if (!TestTrue(TEXT("Actual world and viewer initialized"), Fixture.HasViewer())
        || !TestNotNull(TEXT("Actual contact actor"), Fixture.Contact)) return false;
    Fixture.Contact->SetMultipleContactsEnabled(true);
    Fixture.AdvanceContact(1.2f);
    Fixture.Contact->SetSimulationPaused(true);
    AWallhackVRHUDActor* HUD = Fixture.SpawnHUD();
    if (!TestNotNull(TEXT("Actual multi-contact HUD"), HUD)) return false;
    HUD->SelectNextContact();
    TestEqual(TEXT("Visual evidence selects person 02"), HUD->GetSelectedContactId(), 2);
    FAssetCompilingManager::Get().FinishAllCompilation();

    FVector Viewer;
    FQuat Orientation;
    if (!TestTrue(TEXT("Actual viewer pose available"), Fixture.Contact->GetViewerWorldPose(Viewer, Orientation))) return false;
    TArray<FWallhackSpatialContact> Contacts;
    Fixture.Contact->GetSpatialContacts(Contacts);
    if (!TestEqual(TEXT("Three contact identities are rendered"), Contacts.Num(), 3)) return false;
    TArray<FVector2D> MarkerPositions;
    for (const FWallhackSpatialContact& Contact : Contacts)
    {
        WallhackSpatialMath::FContactView View;
        if (!TestTrue(TEXT("Fresh source contact projects"), Contact.bPositionValid && !Contact.bStale
            && WallhackSpatialMath::ProjectContact(Contact.WorldPosition, Viewer, Orientation.Rotator().Yaw, 100.f, View))) return false;
        MarkerPositions.Add(FVector2D(320., 790.) + WallhackSpatialMath::MapOffset(View, 149.f, HUD->GetMapRangeMeters()));
    }

    const auto ReadHUD = [HUD](TArray<FColor>& Pixels)
    {
        HUD->Tick(0.1f);
        FlushRenderingCommands();
        UTextureRenderTarget2D* Target = HUD->GetHUDRenderTarget();
        return Target && Target->GameThread_GetRenderTargetResource()
            && Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels)
            && Pixels.Num() == 2048 * 1152;
    };
    const auto CountMarkers = [&MarkerPositions](const TArray<FColor>& Pixels)
    {
        TArray<int32> Counts;
        for (const FVector2D& Position : MarkerPositions)
        {
            const int32 X = FMath::RoundToInt(Position.X), Y = FMath::RoundToInt(Position.Y);
            Counts.Add(CountContactGreen(Pixels, 2048, 1152, X - 8, Y - 8, X + 8, Y + 8));
        }
        return Counts;
    };

    // The default must preserve the real view: no map/card backplate and no
    // screen-center ink. Measure alpha, not just the foreground theme color.
    TestTrue(TEXT("Startup uses Minimal density"), HUD->GetHUDDensity() == EWallhackHUDDensity::Minimal);
    HUD->Tick(9.f); // Let the temporary control hint expire.
    TArray<FColor> MinimalPixels;
    if (!TestTrue(TEXT("Read startup minimal HUD"), ReadHUD(MinimalPixels))) return false;
    const auto CountAlpha = [](const TArray<FColor>& Pixels, int32 Left, int32 Top, int32 Right, int32 Bottom)
    {
        int32 Count = 0;
        for (int32 Y = Top; Y < Bottom; ++Y) for (int32 X = Left; X < Right; ++X)
            Count += Pixels[Y * 2048 + X].A > 10 ? 1 : 0;
        return Count;
    };
    TestEqual(TEXT("Minimal map region is fully transparent"), CountAlpha(MinimalPixels, 150, 580, 490, 950), 0);
    TestEqual(TEXT("Central view is fully transparent"), CountAlpha(MinimalPixels, 500, 260, 1500, 860), 0);
    TestEqual(TEXT("Control hint disappears after startup"), CountAlpha(MinimalPixels, 600, 1040, 1450, 1100), 0);
    const int32 MinimalCoverage = CountAlpha(MinimalPixels, 0, 0, 2048, 1152);
    TestTrue(TEXT("Minimal ink covers less than one percent of the visor texture"), MinimalCoverage < 2048 * 1152 / 100);
    TestTrue(TEXT("Save default minimal HUD"), SaveContactImage(MinimalPixels, 2048, 1152, TEXT("16-default-minimal")));
    if (!TestTrue(TEXT("Hide HUD through actual input"), Fixture.SetHUDDensity(EWallhackHUDDensity::Hidden))) return false;
    TArray<FColor> HiddenPixels;
    if (!TestTrue(TEXT("Read hidden HUD"), ReadHUD(HiddenPixels))) return false;
    TestEqual(TEXT("Hidden HUD has no residual text or panel"), CountAlpha(HiddenPixels, 0, 0, 2048, 1152), 0);
    if (!TestTrue(TEXT("Expand HUD through actual input"), Fixture.SetHUDDensity(EWallhackHUDDensity::Full))) return false;

    TArray<FColor> FreshPixels;
    if (!TestTrue(TEXT("Read actual fresh multi-contact HUD"), ReadHUD(FreshPixels))) return false;
    const TArray<int32> FreshCounts = CountMarkers(FreshPixels);
    TestTrue(TEXT("Expanded view contains more visible information than Minimal"),
        CountAlpha(FreshPixels, 0, 0, 2048, 1152) > MinimalCoverage);
    for (int32 Index = 0; Index < Contacts.Num(); ++Index)
    {
        TestTrue(FString::Printf(TEXT("Person %02d has a visible map marker"), Contacts[Index].Id), FreshCounts[Index] > 40);
    }
    TestTrue(TEXT("Save fresh multi-contact HUD"), SaveContactImage(FreshPixels, 2048, 1152, TEXT("11-multiple-map-fresh")));

    Fixture.Contact->SetContactUpdatesEnabled(false);
    Fixture.AdvanceContact(0.9f);
    TArray<FColor> StalePixels;
    if (!TestTrue(TEXT("Read actual stale multi-contact HUD"), ReadHUD(StalePixels))) return false;
    const TArray<int32> StaleCounts = CountMarkers(StalePixels);
    for (int32 Index = 0; Index < Contacts.Num(); ++Index)
    {
        // Comparing the same pixels before/after catches old markers left on
        // screen and does not credit the static map grid as a passing marker.
        TestTrue(FString::Printf(TEXT("Stale person %02d disappears from its former map position"), Contacts[Index].Id),
            FreshCounts[Index] - StaleCounts[Index] > 30 && StaleCounts[Index] < FreshCounts[Index] / 2);
    }
    TestEqual(TEXT("Stale rendering preserves selected identity"), HUD->GetSelectedContactId(), 2);
    TestTrue(TEXT("Save stale multi-contact HUD"), SaveContactImage(StalePixels, 2048, 1152, TEXT("12-multiple-map-stale")));

    Fixture.Contact->SetContactUpdatesEnabled(true);
    Fixture.AdvanceContact(0.1f);
    TArray<FColor> RecoveredPixels;
    if (!TestTrue(TEXT("Read actual recovered multi-contact HUD"), ReadHUD(RecoveredPixels))) return false;
    const TArray<int32> RecoveredCounts = CountMarkers(RecoveredPixels);
    for (int32 Index = 0; Index < Contacts.Num(); ++Index)
    {
        TestTrue(FString::Printf(TEXT("Recovered person %02d returns to the same map location"), Contacts[Index].Id),
            RecoveredCounts[Index] > 40 && RecoveredCounts[Index] - StaleCounts[Index] > 30);
        AddInfo(FString::Printf(TEXT("Person %02d map=(%.2f,%.2f) fresh=%d stale=%d recovered=%d"),
            Contacts[Index].Id, MarkerPositions[Index].X, MarkerPositions[Index].Y,
            FreshCounts[Index], StaleCounts[Index], RecoveredCounts[Index]));
    }
    TestTrue(TEXT("Save recovered multi-contact HUD"), SaveContactImage(RecoveredPixels, 2048, 1152, TEXT("13-multiple-map-recovered")));

    // Render actual 3D spheres AND the instanced world ID labels, then apply
    // the actual desktop AHUD compositing path to produce reviewable evidence.
    ASceneCapture2D* SceneCamera = Fixture.World->SpawnActor<ASceneCapture2D>();
    if (!TestNotNull(TEXT("Multi-contact scene capture actor"), SceneCamera)) return false;
    USceneCaptureComponent2D* Capture = SceneCamera->GetCaptureComponent2D();
    Capture->bCaptureEveryFrame = false;
    Capture->bCaptureOnMovement = false;
    Capture->CaptureSource = SCS_SceneColorHDR;
    Capture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
    Capture->ShowOnlyActorComponents(Fixture.Contact);
    Capture->ShowOnlyActorComponents(HUD);
    Capture->FOVAngle = 90.f;
    Capture->SetWorldLocationAndRotation(Viewer, Orientation.Rotator());
    Capture->PostProcessSettings.bOverride_AutoExposureMethod = true;
    Capture->PostProcessSettings.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
    Capture->PostProcessSettings.bOverride_AutoExposureBias = true;
    Capture->PostProcessSettings.AutoExposureBias = 0.f;
    UTextureRenderTarget2D* SceneTarget = NewObject<UTextureRenderTarget2D>(SceneCamera);
    SceneTarget->RenderTargetFormat = RTF_RGBA16f;
    SceneTarget->ClearColor = FLinearColor(0.f, 0.f, 0.f, 1.f);
    SceneTarget->InitAutoFormat(1024, 576);
    SceneTarget->UpdateResourceImmediate(true);
    Capture->TextureTarget = SceneTarget;
    Fixture.World->Tick(LEVELTICK_All, 0.016f);
    for (int32 Warmup = 0; Warmup < 3; ++Warmup)
    {
        HUD->Tick(0.016f);
        Capture->CaptureScene();
        FlushRenderingCommands();
        FAssetCompilingManager::Get().FinishAllCompilation();
    }
    Capture->CaptureScene();
    FlushRenderingCommands();

    AWallhackHUD* DesktopHUD = Fixture.World->SpawnActor<AWallhackHUD>();
    if (!TestNotNull(TEXT("Actual desktop compositing HUD"), DesktopHUD)) return false;
    UTextureRenderTarget2D* Combined = NewObject<UTextureRenderTarget2D>(DesktopHUD);
    Combined->RenderTargetFormat = RTF_RGBA8;
    Combined->InitAutoFormat(1024, 576);
    Combined->UpdateResourceImmediate(true);
    UCanvas* Canvas = nullptr;
    FVector2D CanvasSize;
    FDrawToRenderTargetContext Context;
    UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(DesktopHUD, Combined, Canvas, CanvasSize, Context);
    if (TestNotNull(TEXT("Multi-contact composite canvas"), Canvas))
    {
        Canvas->K2_DrawTexture(SceneTarget, FVector2D::ZeroVector, CanvasSize,
            FVector2D::ZeroVector, FVector2D(1., 1.), FLinearColor::White, BLEND_Opaque);
        DesktopHUD->SetCanvas(Canvas, Canvas);
        DesktopHUD->DrawHUD();
        DesktopHUD->SetCanvas(nullptr, nullptr);
    }
    UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(DesktopHUD, Context);
    FlushRenderingCommands();
    TArray<FColor> CombinedPixels;
    if (!TestTrue(TEXT("Read multi-contact scene with actual green HUD"),
        Combined->GameThread_GetRenderTargetResource()->ReadPixels(CombinedPixels) && CombinedPixels.Num() == 1024 * 576)) return false;
    for (const FWallhackSpatialContact& Contact : Contacts)
    {
        const FVector Local = Orientation.UnrotateVector(Contact.WorldPosition - Viewer);
        const int32 X = FMath::RoundToInt(511.5 + Local.Y / Local.X * 512.);
        const int32 Y = FMath::RoundToInt(287.5 - Local.Z / Local.X * 512.);
        const int32 DotPixels = CountContactGreen(CombinedPixels, 1024, 576, X - 8, Y - 8, X + 8, Y + 8, false);
        const int32 LabelPixels = CountContactGreen(CombinedPixels, 1024, 576, X - 16, Y - 35, X + 16, Y - 8, false);
        TestTrue(FString::Printf(TEXT("Person %02d world dot survives HUD compositing"), Contact.Id), DotPixels > 20);
        TestTrue(FString::Printf(TEXT("Person %02d has a visible world ID label"), Contact.Id), LabelPixels > 10);
        AddInfo(FString::Printf(TEXT("Person %02d scene=(%d,%d) dot_pixels=%d world_label_pixels=%d"), Contact.Id, X, Y, DotPixels, LabelPixels));
    }
    TestTrue(TEXT("Save multi-contact scene/HUD visual evidence"), SaveContactImage(CombinedPixels, 1024, 576, TEXT("10-multiple-contacts")));

    // Look away from every still-fresh stationary contact. Capture the scene
    // and real HUD again so drawn edge guidance is verified beyond pure math.
    Fixture.SetViewerPose(Viewer, FRotator(0., 180., 0.));
    Fixture.AdvanceContact(0.1f);
    FVector ReversedViewer;
    FQuat ReversedOrientation;
    if (!TestTrue(TEXT("Backward viewer pose available"), Fixture.Contact->GetViewerWorldPose(ReversedViewer, ReversedOrientation))) return false;
    const FWallhackSpatialContact* Selected = Contacts.FindByPredicate([HUD](const FWallhackSpatialContact& Contact)
    {
        return Contact.Id == HUD->GetSelectedContactId();
    });
    if (!TestNotNull(TEXT("Selected offscreen person has a source identity"), Selected)) return false;
    const WallhackHUDProjection::FEdgeIndicator Edge = WallhackHUDProjection::ProjectOffscreen(
        Selected->WorldPosition, ReversedViewer, ReversedOrientation);
    if (!TestTrue(TEXT("Selected person is outside the reversed view"), Edge.bOutside)) return false;

    const auto CaptureComposite = [this, HUD, Capture, SceneTarget, DesktopHUD, Combined,
        &Fixture](const TCHAR* Name, TArray<FColor>& Pixels)
    {
        FVector CaptureViewer;
        FQuat CaptureOrientation;
        if (!Fixture.Contact->GetViewerWorldPose(CaptureViewer, CaptureOrientation)) return false;
        HUD->Tick(0.1f);
        Capture->SetWorldLocationAndRotation(CaptureViewer, CaptureOrientation.Rotator());
        Capture->CaptureScene();
        FlushRenderingCommands();
        UCanvas* VariantCanvas = nullptr;
        FVector2D VariantSize;
        FDrawToRenderTargetContext VariantContext;
        UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(DesktopHUD, Combined, VariantCanvas, VariantSize, VariantContext);
        if (VariantCanvas)
        {
            VariantCanvas->K2_DrawTexture(SceneTarget, FVector2D::ZeroVector, VariantSize,
                FVector2D::ZeroVector, FVector2D(1., 1.), FLinearColor::White, BLEND_Opaque);
            DesktopHUD->SetCanvas(VariantCanvas, VariantCanvas);
            DesktopHUD->DrawHUD();
            DesktopHUD->SetCanvas(nullptr, nullptr);
        }
        UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(DesktopHUD, VariantContext);
        FlushRenderingCommands();
        return TestNotNull(FString(Name) + TEXT(" composite canvas"), VariantCanvas)
            && Combined->GameThread_GetRenderTargetResource()->ReadPixels(Pixels)
            && Pixels.Num() == 1024 * 576
            && SaveContactImage(Pixels, 1024, 576, Name);
    };
    const auto CheckBackwardImage = [this, &Contacts, Viewer, Orientation, Edge](
        const TCHAR* Name, const TArray<FColor>& Pixels)
    {
        const int32 ArrowX = FMath::RoundToInt(Edge.Position.X * 0.5);
        const int32 ArrowY = FMath::RoundToInt(Edge.Position.Y * 0.5);
        const int32 ArrowPixels = CountContactGreen(Pixels, 1024, 576,
            ArrowX - 12, ArrowY - 12, ArrowX + 12, ArrowY + 12, false);
        TestTrue(FString(Name) + TEXT(" draws selected direction arrow at its projected visor edge"), ArrowPixels > 10);
        for (const FWallhackSpatialContact& Contact : Contacts)
        {
            const FVector Local = Orientation.UnrotateVector(Contact.WorldPosition - Viewer);
            const int32 X = FMath::RoundToInt(511.5 + Local.Y / Local.X * 512.);
            const int32 Y = FMath::RoundToInt(287.5 - Local.Z / Local.X * 512.);
            TestEqual(FString::Printf(TEXT("%s clears the former world-dot region for person %02d"), Name, Contact.Id),
                CountContactGreen(Pixels, 1024, 576, X - 8, Y - 8, X + 8, Y + 8, false), 0);
        }
        AddInfo(FString::Printf(TEXT("%s selected_arrow=(%d,%d) visible_arrow_pixels=%d"), Name, ArrowX, ArrowY, ArrowPixels));
    };

    TArray<FColor> FullOffscreenPixels;
    if (!TestTrue(TEXT("Save full-density backward scene and HUD"),
        CaptureComposite(TEXT("14-offscreen-full"), FullOffscreenPixels))) return false;
    CheckBackwardImage(TEXT("Full offscreen"), FullOffscreenPixels);
    TArray<FFloat16Color> BackwardScene;
    if (TestTrue(TEXT("Read backward 3D scene independently of overlay"),
        SceneTarget->GameThread_GetRenderTargetResource()->ReadFloat16Pixels(BackwardScene)))
    {
        int32 SceneGreenPixels = 0;
        for (const FFloat16Color& Pixel : BackwardScene)
        {
            if (Pixel.G.GetFloat() > 0.4f && Pixel.G.GetFloat() > Pixel.R.GetFloat() * 2.f
                && Pixel.G.GetFloat() > Pixel.B.GetFloat() * 3.f) ++SceneGreenPixels;
        }
        TestEqual(TEXT("Looking 180 degrees away removes all real dots and world labels from scene"), SceneGreenPixels, 0);
    }

    bool bCycledToMinimal = false;
    if (const UEnhancedInputComponent* Input = HUD->FindComponentByClass<UEnhancedInputComponent>())
    {
        for (const TUniquePtr<FEnhancedInputActionEventBinding>& Binding : Input->GetActionEventBindings())
        {
            if (Binding->GetAction() && Binding->GetAction()->GetFName() == FName(TEXT("WallhackCycleHUDAction"))
                && Binding->GetTriggerEvent() == ETriggerEvent::Started)
            {
                Binding->Execute(FInputActionInstance(Binding->GetAction()));
                bCycledToMinimal = true;
                break;
            }
        }
    }
    if (!TestTrue(TEXT("Actual HUD density input switches Full to Minimal"), bCycledToMinimal)) return false;
    TArray<FColor> MinimalOffscreenPixels;
    if (!TestTrue(TEXT("Save minimal-density backward scene and HUD"),
        CaptureComposite(TEXT("15-offscreen-minimal"), MinimalOffscreenPixels))) return false;
    CheckBackwardImage(TEXT("Minimal offscreen"), MinimalOffscreenPixels);
    TestEqual(TEXT("Minimal view removes the local map panel"),
        CountContactGreen(MinimalOffscreenPixels, 1024, 576, 80, 290, 240, 480, false), 0);

    Fixture.SetViewerPose(Viewer, Orientation.Rotator());
    Fixture.AdvanceContact(0.1f);
    HUD->Tick(9.f);
    TArray<FColor> MinimalScenePixels;
    if (!TestTrue(TEXT("Save minimal view with actual world dots and selected ID"),
        CaptureComposite(TEXT("17-minimal-scene"), MinimalScenePixels))) return false;
    for (const FWallhackSpatialContact& Contact : Contacts)
    {
        const FVector Local = Orientation.UnrotateVector(Contact.WorldPosition - Viewer);
        const int32 X = FMath::RoundToInt(511.5 + Local.Y / Local.X * 512.);
        const int32 Y = FMath::RoundToInt(287.5 - Local.Z / Local.X * 512.);
        const int32 Labels = CountContactGreen(MinimalScenePixels, 1024, 576, X - 16, Y - 35, X + 16, Y - 8, false);
        if (Contact.Id == HUD->GetSelectedContactId()) TestTrue(TEXT("Minimal preserves selected world ID"), Labels > 10);
        else TestEqual(TEXT("Minimal suppresses unselected world IDs"), Labels, 0);
    }
    Fixture.Contact->SetContactUpdatesEnabled(false);
    Fixture.AdvanceContact(0.9f);
    TArray<FColor> MinimalStalePixels;
    if (!TestTrue(TEXT("Read minimal stale state"), ReadHUD(MinimalStalePixels))) return false;
    int32 WarningPixels = 0;
    for (int32 Y = 1005; Y < 1035; ++Y) for (int32 X = 160; X < 500; ++X)
    {
        const FColor& Pixel = MinimalStalePixels[Y * 2048 + X];
        WarningPixels += Pixel.A > 100 && Pixel.R > 130 && Pixel.R > Pixel.B * 1.4f ? 1 : 0;
    }
    TestTrue(TEXT("Minimal view keeps an explicit visible stale warning"), WarningPixels > 50);
    TestTrue(TEXT("Save minimal stale warning"), SaveContactImage(MinimalStalePixels, 2048, 1152, TEXT("18-minimal-stale")));
    AddInfo(FString::Printf(TEXT("Minimal visible texture coverage: %d pixels (%.3f percent)"),
        MinimalCoverage, MinimalCoverage * 100.f / (2048 * 1152)));
    return true;
}

#endif
