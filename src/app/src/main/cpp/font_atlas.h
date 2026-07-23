#pragma once
#include <GLES3/gl3.h>

// Font atlas: loads a TTF from assets, rasterizes ASCII 32-126 into a
// single-channel GL texture, and provides glyph metrics for text layout.
// The first texel (0,0) is solid white so non-text quads sampling at UV
// (0,0) pass their per-vertex colour through unchanged.

struct GlyphInfo {
    float ax;       // advance width in pixels
    float ay;       // advance height (line gap) in pixels
    float bw, bh;   // bitmap width/height in pixels
    float bl, bt;   // bitmap left/top offset in pixels
    float tx, ty;   // UV origin in atlas (top-left of glyph)
    float tw, th;   // UV size of glyph in atlas
};

class FontAtlas {
public:
    static constexpr int kFirstChar = 32;
    static constexpr int kNumChars = 95;   // 32..126
    static constexpr int kAtlasW = 2048;
    static constexpr int kAtlasH = 64;
    static constexpr int kPixelHeight = 40;  // rasterization size
    // The old 5x7 bitmap font was 7px tall. px values throughout the codebase
    // are tuned for that scale. This factor converts "metres per old-bitmap-pixel"
    // to "metres per font-pixel" so existing px values keep their physical size.
    static constexpr float kBitmapFontHeight = 7.0f;
    static float pxScale() { return (float)kBitmapFontHeight / (float)kPixelHeight; }

    FontAtlas() = default;
    ~FontAtlas();

    // Load TTF from the app's assets directory and build the atlas texture.
    // Returns false on failure.
    bool init(const char *assetPath);

    // Get glyph info for a codepoint (must be in [kFirstChar, kFirstChar+kNumChars-1]).
    const GlyphInfo &glyph(int codepoint) const { return glyphs[codepoint - kFirstChar]; }

    // The GL texture name for the atlas. Bind this before drawing text.
    GLuint texture() const { return tex; }

    // Line height in pixels (ascent + descent).
    float lineHeight() const { return lineH; }

    // Width of a string in pixels at the rasterized size.
    float textWidth(const char *s) const;

private:
    GlyphInfo glyphs[kNumChars] = {};
    GLuint tex = 0;
    float lineH = 0;
};

extern FontAtlas gFont;
