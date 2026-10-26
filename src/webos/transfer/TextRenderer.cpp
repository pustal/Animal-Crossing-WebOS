/**
 * @file TextRenderer.cpp
 * @brief Implementation of TextRenderer (see BitmapFont.h).
 */
#include "BitmapFont.h"

#include <vector>

namespace acwebos
{
namespace
{

constexpr int kAtlasWidth  = BitmapFont::kGlyphCount * BitmapFont::kGlyphSize;
constexpr int kAtlasHeight = BitmapFont::kGlyphSize;

} // namespace

TextRenderer::~TextRenderer() { Detach(); }

bool TextRenderer::Attach(SDL_Renderer *renderer)
{
    if (renderer == renderer_ && atlas_)
        return true;

    Detach();
    renderer_ = renderer;
    if (!renderer_)
        return false;

    // White glyphs on a transparent background; colour comes from SDL colour modulation.
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(kAtlasWidth * kAtlasHeight), 0u);
    for (int glyph = 0; glyph < BitmapFont::kGlyphCount; ++glyph) {
        for (int row = 0; row < BitmapFont::kGlyphSize; ++row) {
            const std::uint8_t bits = BitmapFont::kGlyphs[static_cast<std::size_t>(glyph)][static_cast<std::size_t>(row)];
            for (int col = 0; col < BitmapFont::kGlyphSize; ++col) {
                if (bits & (1u << col)) {
                    const int x = glyph * BitmapFont::kGlyphSize + col;
                    pixels[static_cast<std::size_t>(row * kAtlasWidth + x)] = 0xFFFFFFFFu;
                }
            }
        }
    }

    atlas_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, kAtlasWidth, kAtlasHeight);
    if (!atlas_)
        return false;

    SDL_UpdateTexture(atlas_, nullptr, pixels.data(), kAtlasWidth * static_cast<int>(sizeof(std::uint32_t)));
    SDL_SetTextureBlendMode(atlas_, SDL_BLENDMODE_BLEND);
#if SDL_VERSION_ATLEAST(2, 0, 12)
    SDL_SetTextureScaleMode(atlas_, SDL_ScaleModeNearest);
#endif
    return true;
}

void TextRenderer::Detach()
{
    if (atlas_)
        SDL_DestroyTexture(atlas_);
    atlas_    = nullptr;
    renderer_ = nullptr;
}

void TextRenderer::DrawText(int x, int y, const std::string &text, int scale, Color color) const
{
    if (!renderer_ || !atlas_)
        return;

    SDL_SetTextureColorMod(atlas_, color.r, color.g, color.b);
    SDL_SetTextureAlphaMod(atlas_, color.a);

    const int size = BitmapFont::kGlyphSize * scale;
    SDL_Rect dst{ x, y, size, size };
    for (const char raw : text) {
        int index = static_cast<unsigned char>(raw) - static_cast<unsigned char>(BitmapFont::kFirstChar);
        if (index < 0 || index >= BitmapFont::kGlyphCount)
            index = '?' - BitmapFont::kFirstChar;

        const SDL_Rect src{ index * BitmapFont::kGlyphSize, 0, BitmapFont::kGlyphSize, BitmapFont::kGlyphSize };
        SDL_RenderCopy(renderer_, atlas_, &src, &dst);
        dst.x += size;
    }
}

void TextRenderer::DrawTextCentered(int centerX, int y, const std::string &text, int scale, Color color) const
{
    DrawText(centerX - MeasureText(text, scale) / 2, y, text, scale, color);
}

int TextRenderer::MeasureText(const std::string &text, int scale)
{
    return static_cast<int>(text.size()) * BitmapFont::kGlyphSize * scale;
}

void TextRenderer::FillRect(int x, int y, int w, int h, Color color) const
{
    if (!renderer_)
        return;

    const SDL_Rect rect{ x, y, w, h };
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, color.r, color.g, color.b, color.a);
    SDL_RenderFillRect(renderer_, &rect);
}

} // namespace acwebos
