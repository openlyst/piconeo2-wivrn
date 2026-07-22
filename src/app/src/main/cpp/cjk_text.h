#pragma once
// Minimal CJK text renderer using stb_truetype. Bakes a glyph atlas into a
// GL texture at init time; emitTextQuads produces pos+uv vertices for a string.
#include <string>
#include <vector>
#include <cstdint>

struct CjkGlyph {
    float x0, y0, x1, y1;   // atlas UVs
    float xoff, yoff;       // bearing in atlas pixels
    float advance;          // x advance in atlas pixels
    float pw, ph;           // glyph pixel w/h in atlas
};

class CjkText {
public:
    // Load font from assets/fonts/test_cjk.otf and bake the atlas. Returns false on failure.
    bool init(float pxHeight = 32.0f);

    // Build vertex data for a UTF-8 string. Each vertex: pos.xyz + uv.xy (5 floats).
    // px = metres per font pixel. Returns vertex count (triangles * 3).
    int emitQuads(std::vector<float> &v, const char *utf8, float x, float y, float px,
                  float r, float g, float b) const;

    // Texture atlas handle (0 if not initialised).
    unsigned int texture() const { return tex_; }
    bool ready() const { return tex_ != 0; }

private:
    unsigned int tex_ = 0;
    int atlasW_ = 0, atlasH_ = 0;
    std::vector<CjkGlyph> glyphs_;   // indexed by codepoint hash
    // Simple codepoint -> glyph map. For the test we only have ~20 chars so a
    // flat array keyed by codepoint works (codepoints are small for our subset).
    static constexpr int kMaxCp = 0x10000;
    int16_t cpToIdx_[kMaxCp] = {};
    float fontScale_ = 1.0f;
};

// Global instance, defined in render_thread.cpp.
extern CjkText gCjkText;

// Emit a flat-coloured quad (two triangles) with 8-float verts: pos.xyz +
// uv.xy(=0,0) + color.rgb. Uses the white pixel at UV (0,0) so the textured
// shader renders it as solid colour.
void appendQuad8(std::vector<float> &v, float xL, float yTop, float xR, float yBot,
                 float r, float g, float b);
