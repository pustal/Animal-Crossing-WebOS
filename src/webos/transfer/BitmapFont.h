/**
 * @file BitmapFont.h
 * @brief A tiny embedded 8x8 font and a renderer that draws it with SDL.
 *
 * The port must be able to show text before any game data exists (the setup
 * screen), so it cannot rely on the game's own fonts. The glyphs are uploaded
 * once into a small texture atlas and drawn scaled with nearest filtering.
 */
#ifndef ACWEBOS_BITMAP_FONT_H
#define ACWEBOS_BITMAP_FONT_H

#include <SDL.h>

#include <array>
#include <cstdint>
#include <string>

namespace acwebos
{

/** Raw glyph data for printable ASCII. */
struct BitmapFont {
    static constexpr char kFirstChar       = ' ';
    static constexpr int kGlyphCount       = 95; // ' ' (0x20) .. '~' (0x7E)
    static constexpr int kGlyphSize        = 8;  // pixels, square
    static const std::array<std::array<std::uint8_t, kGlyphSize>, kGlyphCount> kGlyphs;
};

/** An RGBA colour. */
struct Color {
    std::uint8_t r, g, b, a;
};

/** Draws text and simple panels on an SDL renderer. */
class TextRenderer
{
public:
    TextRenderer() = default;
    ~TextRenderer();

    TextRenderer(const TextRenderer &)            = delete;
    TextRenderer &operator=(const TextRenderer &) = delete;

    /**
     * Binds to @p renderer, rebuilding the glyph atlas when the renderer changes.
     * @return False if the atlas texture could not be created.
     */
    bool Attach(SDL_Renderer *renderer);

    /** Releases the atlas texture (call before the renderer is destroyed). */
    void Detach();

    /**
     * Draws @p text with its top-left corner at (@p x, @p y).
     * Characters outside printable ASCII are drawn as '?'.
     *
     * @param scale Integer pixel size multiplier (1 = 8 px glyphs).
     */
    void DrawText(int x, int y, const std::string &text, int scale, Color color) const;

    /** Draws @p text horizontally centred on @p centerX. */
    void DrawTextCentered(int centerX, int y, const std::string &text, int scale, Color color) const;

    /** Width in pixels @p text would take at @p scale. */
    static int MeasureText(const std::string &text, int scale);

    /** Fills a rectangle, blending with what is already drawn. */
    void FillRect(int x, int y, int w, int h, Color color) const;

private:
    SDL_Renderer *renderer_ = nullptr;
    SDL_Texture *atlas_     = nullptr;
};

} // namespace acwebos

#endif // ACWEBOS_BITMAP_FONT_H
