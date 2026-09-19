#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "WallhackRuntimeTestWorld.h"
#include "WallhackWorldContact.h"
#include "WallhackVRHUDActor.h"
#include "WallhackHUD.h"
#include "AssetCompilingManager.h"
#include "Engine/Canvas.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialRenderProxy.h"
#include "MaterialShared.h"
#include "ImageUtils.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RenderingThread.h"

namespace
{
    struct FDotImage
    {
        int32 GreenPixels = 0;
        double CenterX = 0;
        double CenterY = 0;
        double MeanInverseAlpha = 0;
        float BackgroundInverseAlpha = 0;
        int32 OpaquePixels = 0;
    };

    bool CaptureDot(USceneCaptureComponent2D* Capture, const FVector& Position,
        const FRotator& Orientation, const TCHAR* Name, FDotImage& Out)
    {
        Capture->SetWorldLocationAndRotation(Position, Orientation);
        Capture->CaptureScene();
        FlushRenderingCommands();
        UTextureRenderTarget2D* Target = Capture->TextureTarget;
        TArray<FFloat16Color> Pixels;
        if (!Target->GameThread_GetRenderTargetResource()->ReadFloat16Pixels(Pixels)
            || Pixels.Num() != Target->SizeX * Target->SizeY) return false;

        TArray<FColor> Image;
        Image.Reserve(Pixels.Num());
        Out.BackgroundInverseAlpha = Pixels[0].A.GetFloat();
        for (int32 Index = 0; Index < Pixels.Num(); ++Index)
        {
            const FFloat16Color& Pixel = Pixels[Index];
            const FLinearColor Linear(Pixel.R.GetFloat(), Pixel.G.GetFloat(), Pixel.B.GetFloat(), 1.f);
            if (Pixel.A.GetFloat() < 0.5f) ++Out.OpaquePixels;
            if (Linear.G > 0.4f && Linear.G > Linear.R * 2.f && Linear.G > Linear.B * 3.f)
            {
                ++Out.GreenPixels;
                Out.CenterX += Index % Target->SizeX;
                Out.CenterY += Index / Target->SizeX;
                Out.MeanInverseAlpha += Pixel.A.GetFloat();
            }
            Image.Add(Linear.ToFColor(true));
        }
        if (Out.GreenPixels > 0)
        {
            Out.CenterX /= Out.GreenPixels;
            Out.CenterY /= Out.GreenPixels;
            Out.MeanInverseAlpha /= Out.GreenPixels;
        }

        const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("TrackingVerification/LocalRender"));
        IFileManager::Get().MakeDirectory(*Directory, true);
        TArray64<uint8> PNG;
        FImageUtils::PNGCompressImageArray(Target->SizeX, Target->SizeY, Image, PNG);
        return FFileHelper::SaveArrayToFile(PNG, *FPaths::Combine(Directory, FString(Name) + TEXT(".png")));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWallhackSpatialRenderTest,
    "Wallhack.Spatial.Render.DotVisibilityAndParallax",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FWallhackSpatialRenderTest::RunTest(const FString& Parameters)
{
    FScopedWallhackRuntimeWorld Fixture;
    if (!TestTrue(TEXT("Actual world and viewer initialized"), Fixture.HasViewer())) return false;
    Fixture.SetViewerPose(FVector(0., 0., 160.), FRotator::ZeroRotator);
    AWallhackWorldContact* Contact = Fixture.Contact;
    if (!TestNotNull(TEXT("Actual contact actor"), Contact)) return false;
    Contact->PlaceInFrontOfViewer();
    Contact->Tick(0.016f);
    FAssetCompilingManager::Get().FinishAllCompilation();

    ASceneCapture2D* Camera = Fixture.World->SpawnActor<ASceneCapture2D>();
    USceneCaptureComponent2D* Capture = Camera->GetCaptureComponent2D();
    Capture->bCaptureEveryFrame = false;
    Capture->bCaptureOnMovement = false;
    Capture->CaptureSource = SCS_SceneColorHDR; // Inverse opacity is explicit in this capture format.
    Capture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
    Capture->ShowOnlyActorComponents(Contact);
    Capture->FOVAngle = 90.f;
    Capture->PostProcessSettings.bOverride_AutoExposureMethod = true;
    Capture->PostProcessSettings.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
    Capture->PostProcessSettings.bOverride_AutoExposureBias = true;
    Capture->PostProcessSettings.AutoExposureBias = 0.f;
    UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(Camera);
    Target->RenderTargetFormat = RTF_RGBA16f;
    Target->ClearColor = FLinearColor(0.f, 0.f, 0.f, 1.f);
    Target->InitAutoFormat(1024, 576);
    Target->UpdateResourceImmediate(true);
    Capture->TextureTarget = Target;

    Fixture.World->Tick(LEVELTICK_All, 0.016f);
    Fixture.World->SendAllEndOfFrameUpdates();
    FlushRenderingCommands();
    const UStaticMeshComponent* Mesh = FindObject<UStaticMeshComponent>(Contact, TEXT("ContactDot"));
    if (!TestNotNull(TEXT("Actual person 01 mesh"), Mesh)) return false;
    FString UsedMaterial;
    bool bUsedFallback = true;
    const FMaterialRenderProxy* Proxy = Mesh->GetMaterial(0)->GetRenderProxy();
    const ERHIFeatureLevel::Type FeatureLevel = Fixture.World->GetFeatureLevel();
    // First draws and fallback queries can request missing material shaders.
    // Finish those jobs and verify the real material before measuring pixels.
    Capture->SetWorldLocationAndRotation(FVector(0., 0., 160.), FRotator::ZeroRotator);
    for (int32 Warmup = 0; Warmup < 3 && bUsedFallback; ++Warmup)
    {
        Capture->CaptureScene();
        FlushRenderingCommands();
        FAssetCompilingManager::Get().FinishAllCompilation();
        ENQUEUE_RENDER_COMMAND(WallhackCheckRenderedMaterial)([Proxy, FeatureLevel, &UsedMaterial, &bUsedFallback](FRHICommandListImmediate&)
        {
            const FMaterialRenderProxy* Fallback = nullptr;
            UsedMaterial = Proxy->GetMaterialWithFallback(FeatureLevel, Fallback).GetFriendlyName();
            bUsedFallback = Fallback != nullptr;
        });
        FlushRenderingCommands();
    }
    AddInfo(FString::Printf(TEXT("Rendered material=%s fallback=%d"), *UsedMaterial, bUsedFallback));
    TestFalse(TEXT("Render uses real contact material after warmup"), bUsedFallback);

    FDotImage Ahead;
    if (!TestTrue(TEXT("Read actual rendered dot"), CaptureDot(Capture, FVector(0., 0., 160.),
        FRotator::ZeroRotator, TEXT("01-ahead-3m"), Ahead))) return false;
    TestTrue(TEXT("Green material is visible at three metres"), Ahead.GreenPixels > 20);
    TestTrue(TEXT("Dot is horizontally centered"), FMath::Abs(Ahead.CenterX - 511.5) < 3.);
    TestTrue(TEXT("Dot is at eye height"), FMath::Abs(Ahead.CenterY - 287.5) < 3.);
    TestTrue(TEXT("Opaque dot has near-zero inverse alpha"), Ahead.MeanInverseAlpha < 0.15);
    TestTrue(TEXT("Empty scene has transparent inverse alpha"), Ahead.BackgroundInverseAlpha > 0.99f);

    // Exercise the actual desktop AHUD overlay on top of the rendered scene.
    // Separate successful scene/HUD exports cannot detect an opaque overlay
    // accidentally covering the dot, which was the original visibility bug.
    AWallhackVRHUDActor* SpatialHUD = Fixture.SpawnHUD();
    if (!TestTrue(TEXT("Expand map through real HUD input"), Fixture.SetHUDDensity(EWallhackHUDDensity::Full))) return false;
    SpatialHUD->Tick(2.f);
    AWallhackHUD* DesktopHUD = Fixture.World->SpawnActor<AWallhackHUD>();
    UTextureRenderTarget2D* Combined = NewObject<UTextureRenderTarget2D>(DesktopHUD);
    Combined->RenderTargetFormat = RTF_RGBA8;
    Combined->InitAutoFormat(1024, 576);
    Combined->UpdateResourceImmediate(true);
    UCanvas* Canvas = nullptr;
    FVector2D CanvasSize;
    FDrawToRenderTargetContext CanvasContext;
    UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(DesktopHUD, Combined, Canvas, CanvasSize, CanvasContext);
    if (TestNotNull(TEXT("Combined scene and HUD canvas"), Canvas))
    {
        Canvas->K2_DrawTexture(Target, FVector2D::ZeroVector, CanvasSize,
            FVector2D::ZeroVector, FVector2D(1., 1.), FLinearColor::White, BLEND_Opaque);
        DesktopHUD->SetCanvas(Canvas, Canvas);
        DesktopHUD->DrawHUD();
        DesktopHUD->SetCanvas(nullptr, nullptr);
    }
    UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(DesktopHUD, CanvasContext);
    FlushRenderingCommands();
    TArray<FColor> CombinedPixels;
    if (TestTrue(TEXT("Read combined scene and actual desktop HUD"),
        Combined->GameThread_GetRenderTargetResource()->ReadPixels(CombinedPixels)))
    {
        int32 VisibleDotPixels = 0;
        for (int32 Y = 280; Y < 296; ++Y) for (int32 X = 504; X < 520; ++X)
        {
            const FColor& Pixel = CombinedPixels[Y * 1024 + X];
            if (Pixel.G > 160 && Pixel.G > Pixel.R * 1.3f && Pixel.B < 130) ++VisibleDotPixels;
        }
        TestTrue(TEXT("Actual desktop HUD preserves the visible world dot"), VisibleDotPixels > 20);
        int32 VisibleMapLabelPixels = 0;
        for (int32 Y = 305; Y < 320; ++Y) for (int32 X = 80; X < 240; ++X)
        {
            const FColor& Pixel = CombinedPixels[Y * 1024 + X];
            if (Pixel.R > 80 && Pixel.G > 80 && Pixel.B > 80) ++VisibleMapLabelPixels;
        }
        TestTrue(TEXT("Local map label survives real HUD compositing"), VisibleMapLabelPixels > 70);
        int32 VisibleNativeLabelPixels = 0;
        for (int32 Y = 433; Y < 461; ++Y) for (int32 X = 779; X < 935; ++X)
        {
            const FColor& Pixel = CombinedPixels[Y * 1024 + X];
            if (Pixel.R > 80 && Pixel.G > 80 && Pixel.B > 80) ++VisibleNativeLabelPixels;
        }
        TestTrue(TEXT("Contact viewer font label survives real HUD compositing"), VisibleNativeLabelPixels > 20);
        TArray64<uint8> PNG;
        FImageUtils::PNGCompressImageArray(1024, 576, CombinedPixels, PNG);
        TestTrue(TEXT("Save combined desktop appearance"), FFileHelper::SaveArrayToFile(PNG,
            *FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("TrackingVerification/LocalRender/09-combined-desktop.png"))));
    }

    FDotImage Sideways;
    TestTrue(TEXT("Read shifted viewpoint"), CaptureDot(Capture, FVector(0., 100., 160.),
        FRotator::ZeroRotator, TEXT("02-step-right-1m"), Sideways));
    TestTrue(TEXT("World dot remains visible after a sideways step"), Sideways.GreenPixels > 20);
    TestTrue(TEXT("Real render has the expected horizontal parallax"),
        FMath::Abs(Sideways.CenterX - (511.5 - 1024. / 6.)) < 4.);

    FDotImage Near;
    TestTrue(TEXT("Read closer viewpoint"), CaptureDot(Capture, FVector(100., 0., 160.),
        FRotator::ZeroRotator, TEXT("03-forward-1m"), Near));
    TestTrue(TEXT("World dot grows when moving one metre closer"), Near.GreenPixels > Ahead.GreenPixels * 1.6);

    FDotImage Behind;
    TestTrue(TEXT("Read reversed viewpoint"), CaptureDot(Capture, FVector(0., 0., 160.),
        FRotator(0., 180., 0.), TEXT("04-look-back"), Behind));
    TestEqual(TEXT("Dot leaves the view when looking away"), Behind.GreenPixels, 0);

    AddInfo(FString::Printf(TEXT("Actual scene pixels: ahead=%d center=(%.2f,%.2f) inverse_alpha=%.4f background=%.4f; sideways_x=%.2f; near=%d; behind=%d; opaque=%d"),
        Ahead.GreenPixels, Ahead.CenterX, Ahead.CenterY, Ahead.MeanInverseAlpha, Ahead.BackgroundInverseAlpha,
        Sideways.CenterX, Near.GreenPixels, Behind.GreenPixels, Ahead.OpaquePixels));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWallhackSpatialHUDRenderTest,
    "Wallhack.Spatial.Render.MinimapTracksActualViewer",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FWallhackSpatialHUDRenderTest::RunTest(const FString& Parameters)
{
    FScopedWallhackRuntimeWorld Fixture;
    if (!TestTrue(TEXT("Actual viewer initialized"), Fixture.HasViewer())) return false;
    Fixture.AdvanceContact(1.2f);
    AWallhackVRHUDActor* HUD = Fixture.SpawnHUD();
    if (!TestNotNull(TEXT("Actual compositor HUD"), HUD)) return false;
    if (!TestTrue(TEXT("Map tests explicitly expand the default minimal view"), Fixture.SetHUDDensity(EWallhackHUDDensity::Full))) return false;
    FAssetCompilingManager::Get().FinishAllCompilation();

    const auto CheckMap = [this, HUD](const TCHAR* Name, const FVector2D& Expected)
    {
        HUD->Tick(2.f);
        FlushRenderingCommands();
        UTextureRenderTarget2D* Target = HUD->GetHUDRenderTarget();
        TArray<FColor> Pixels;
        if (!TestTrue(FString(Name) + TEXT(" read actual HUD pixels"),
            Target && Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels))) return;
        const auto IsGreen = [](const FColor& Pixel)
        {
            return Pixel.G > 160 && Pixel.G > Pixel.R * 1.3f && Pixel.B < 130 && Pixel.A > 100;
        };
        int32 GreenNearMarker = 0;
        for (int32 Y = FMath::RoundToInt(Expected.Y) - 8; Y <= FMath::RoundToInt(Expected.Y) + 8; ++Y)
            for (int32 X = FMath::RoundToInt(Expected.X) - 8; X <= FMath::RoundToInt(Expected.X) + 8; ++X)
                if (IsGreen(Pixels[Y * Target->SizeX + X])) ++GreenNearMarker;
        TestTrue(FString(Name) + TEXT(" green contact appears at expected map coordinate"), GreenNearMarker > 40);
        int32 OpaqueLabelPixels = 0;
        for (int32 Y = 610; Y < 635; ++Y) for (int32 X = 160; X < 480; ++X)
        {
            const FColor& Pixel = Pixels[Y * Target->SizeX + X];
            if (Pixel.A > 100 && Pixel.R > 80 && Pixel.G > 80 && Pixel.B > 80) ++OpaqueLabelPixels;
        }
        TestTrue(FString(Name) + TEXT(" label has compositor-visible alpha"), OpaqueLabelPixels > 150);
        TestTrue(FString(Name) + TEXT(" scene center remains transparent"), Pixels[576 * Target->SizeX + 1024].A == 0);
        AddInfo(FString::Printf(TEXT("%s expected_map=(%.2f,%.2f) green_marker_pixels=%d"), Name, Expected.X, Expected.Y, GreenNearMarker));
        const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("TrackingVerification/LocalRender"));
        IFileManager::Get().MakeDirectory(*Directory, true);
        TArray64<uint8> PNG;
        FImageUtils::PNGCompressImageArray(Target->SizeX, Target->SizeY, Pixels, PNG);
        TestTrue(FString(Name) + TEXT(" saves visual evidence"), FFileHelper::SaveArrayToFile(PNG,
            *FPaths::Combine(Directory, FString(Name) + TEXT(".png"))));
    };

    CheckMap(TEXT("05-map-ahead-3m"), FVector2D(320., 700.6));
    Fixture.SetViewerPose(FVector(0., 100., 170.), FRotator::ZeroRotator);
    Fixture.AdvanceContact(0.1f);
    CheckMap(TEXT("06-map-step-right-1m"), FVector2D(290.2, 700.6));
    Fixture.SetViewerPose(FVector(100., 0., 170.), FRotator::ZeroRotator);
    Fixture.AdvanceContact(0.1f);
    CheckMap(TEXT("07-map-forward-1m"), FVector2D(320., 730.4));
    Fixture.SetViewerPose(FVector(0., 0., 170.), FRotator(0., 180., 0.));
    Fixture.AdvanceContact(0.1f);
    CheckMap(TEXT("08-map-look-back"), FVector2D(320., 879.4));
    return true;
}

#endif
