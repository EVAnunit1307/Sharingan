#pragma once

#include "CanvasItem.h"
#include "Engine/Canvas.h"
#include "Engine/Texture2D.h"

// Small runtime atlas: labels remain visible without an offline UFont asset.
// Each character is one batched tile, independent of the scene/material cook.
namespace WallhackCanvasLabels
{
    inline constexpr uint8 Glyphs[][7] = {
            {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30}, // A B
            {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30},
            {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
            {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
            {14,4,4,4,4,4,14}, {7,2,2,2,18,18,12},
            {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
            {17,27,21,21,17,17,17}, {17,25,25,21,19,19,17},
            {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
            {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
            {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
            {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4},
            {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
            {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31}, // Z
            {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14}, // 0 1
            {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30},
            {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
            {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8},
            {14,17,17,14,17,17,14}, {14,17,17,15,1,1,14},
            {0,0,0,0,0,4,4}, {0,0,0,31,0,0,0}, // . -
            {1,1,2,4,8,16,16}, {0,4,4,0,4,4,0}, // / :
            {0,4,4,31,4,4,0}, {0,0,0,0,0,0,0} // + space
        };
    inline int32 GlyphIndex(TCHAR Character)
    {
        Character=FChar::ToUpper(Character);
        if(Character>='A'&&Character<='Z')return Character-'A';
        if(Character>='0'&&Character<='9')return 26+Character-'0';
        switch(Character){case '.':return 36;case '-':return 37;case '/':return 38;case ':':return 39;case '+':return 40;default:return 41;}
    }
    inline UTexture2D* CreateAtlas()
    {
        UTexture2D* Atlas = UTexture2D::CreateTransient(128, 32, PF_B8G8R8A8, TEXT("SpatialLabels"));
        if (!Atlas) return nullptr;
        Atlas->Filter = TF_Nearest;
        Atlas->SRGB = false;
        FTexture2DMipMap& Mip = Atlas->GetPlatformData()->Mips[0];
        FColor* Pixels = static_cast<FColor*>(Mip.BulkData.Lock(LOCK_READ_WRITE));
        FMemory::Memzero(Pixels, 128 * 32 * sizeof(FColor));
        for (int32 Glyph = 0; Glyph < UE_ARRAY_COUNT(Glyphs); ++Glyph)
            for (int32 Row = 0; Row < 7; ++Row)
                for (int32 Col = 0; Col < 5; ++Col)
                    if (Glyphs[Glyph][Row] & (1 << (4 - Col)))
                        Pixels[((Glyph / 16) * 8 + Row) * 128 + (Glyph % 16) * 8 + Col] = FColor::White;
        Mip.BulkData.Unlock();
        Atlas->UpdateResource();
        return Atlas;
    }

    inline void Draw(UCanvas* Canvas, UTexture2D* Atlas, const FString& Label,
        float X, float Y, float Height, const FLinearColor& Color, bool bCenter = false)
    {
        if (!Canvas || !Atlas || !Atlas->GetResource()) return;
        const float Pixel = Height / 7.f;
        const float Advance = 6.f * Pixel;
        float Cursor = bCenter ? X - (Label.Len() * Advance - Pixel) * 0.5f : X;
        for (const TCHAR Character : Label.ToUpper())
        {
            const int32 Index = GlyphIndex(Character);
            if (Index != 41)
            {
                const FVector2D UV0((Index % 16) * 8.f / 128.f, (Index / 16) * 8.f / 32.f);
                const FVector2D UV1 = UV0 + FVector2D(5.f / 128.f, 7.f / 32.f);
                FCanvasTileItem Tile(FVector2D(Cursor + 1.5f, Y + 1.5f), Atlas->GetResource(),
                    FVector2D(Pixel * 5.f, Height), UV0, UV1, FLinearColor(0.f, 0.f, 0.f, Color.A));
                // Ordinary Canvas translucency writes RGB only. The headset
                // compositor needs glyph coverage in alpha as well.
                Tile.BlendMode = SE_BLEND_AlphaBlend;
                Canvas->DrawItem(Tile);
                Tile.Position = FVector2D(Cursor, Y);
                Tile.SetColor(Color);
                Canvas->DrawItem(Tile);
            }
            Cursor += Advance;
        }
    }
}
