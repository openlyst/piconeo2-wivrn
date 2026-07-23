#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include "font_atlas.h"
#include "log.h"
#include <cstdlib>
#include <cstring>
#include <cstdio>

FontAtlas gFont;

FontAtlas::~FontAtlas()
{
    if (tex) glDeleteTextures(1, &tex);
}

bool FontAtlas::init(const char *path)
{
    // Read the TTF file from the filesystem (extracted by Java to $HOME).
    FILE *f = fopen(path, "rb");
    if (!f) {
        LOGE("font_atlas: cannot open %s", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *ttfBuf = (unsigned char *)malloc(size);
    if (!ttfBuf) { fclose(f); return false; }
    fread(ttfBuf, 1, size, f);
    fclose(f);

    stbtt_fontinfo font;
    if (!stbtt_InitFont(&font, ttfBuf, stbtt_GetFontOffsetForIndex(ttfBuf, 0))) {
        LOGE("font_atlas: stbtt_InitFont failed");
        free(ttfBuf);
        return false;
    }

    float scale = stbtt_ScaleForPixelHeight(&font, kPixelHeight);

    int fontAscent, fontDescent, fontGap;
    stbtt_GetFontVMetrics(&font, &fontAscent, &fontDescent, &fontGap);
    lineH = (fontAscent - fontDescent) * scale;

    // Allocate atlas buffer (single channel, init to 0).
    // Texel (0,0) is set to 255 so non-text quads sampling at UV (0,0)
    // get full opacity and their per-vertex colour passes through.
    unsigned char *atlas = (unsigned char *)calloc(kAtlasW * kAtlasH, 1);
    atlas[0] = 255;

    // Pack glyphs left-to-right starting at x=1 (leave column 0 for the
    // solid white texel used by non-text quads).
    int x = 1;
    for (int i = 0; i < kNumChars; i++) {
        int codepoint = kFirstChar + i;
        int bw, bh, bl, bt;
        unsigned char *bmp = stbtt_GetCodepointBitmap(&font, 0, scale, codepoint,
                                                       &bw, &bh, &bl, &bt);
        // Skip if it doesn't fit on this row; wrap to next row.
        if (x + bw > kAtlasW) {
            LOGE("font_atlas: atlas too small for all glyphs at x=%d (glyph %d wide=%d)",
                 x, codepoint, bw);
            // Just skip remaining glyphs rather than crashing.
            free(bmp);
            break;
        }

        // Copy bitmap into atlas. bt is the top-side bearing from stbtt
        // (positive = above baseline), so we place it accordingly.
        int yOff = 0;
        for (int row = 0; row < bh; row++) {
            memcpy(atlas + (yOff + row) * kAtlasW + x,
                   bmp + row * bw, bw);
        }

        GlyphInfo &g = glyphs[i];
        g.bw = (float)bw;
        g.bh = (float)bh;
        g.bl = (float)bl;
        g.bt = (float)bt;
        g.tx = (float)x / kAtlasW;
        // Flip V: stb bitmap row 0 is top, but GL texture row 0 is bottom.
        // So the top of the glyph in the atlas buffer is at V = (yOff+bh)/kAtlasH,
        // and we use a negative th so the quad maps top->top, bottom->bottom.
        g.ty = (float)(yOff + bh) / kAtlasH;
        g.tw = (float)bw / kAtlasW;
        g.th = -(float)bh / kAtlasH;

        int adv, lsb;
        stbtt_GetCodepointHMetrics(&font, codepoint, &adv, &lsb);
        g.ax = adv * scale;
        g.ay = 0;

        stbtt_FreeBitmap(bmp, nullptr);
        x += bw + 1;  // 1px padding between glyphs
    }

    LOGI("font_atlas: packed %d glyphs, last x=%d, atlas %dx%d",
         kNumChars, x, kAtlasW, kAtlasH);

    // Upload to GL as a single-channel texture.
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, kAtlasW, kAtlasH, 0,
                 GL_RED, GL_UNSIGNED_BYTE, atlas);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);

    free(atlas);
    free(ttfBuf);
    return true;
}

float FontAtlas::textWidth(const char *s) const
{
    float w = 0;
    for (const char *p = s; *p; p++) {
        int c = (unsigned char)*p;
        if (c >= kFirstChar && c < kFirstChar + kNumChars)
            w += glyphs[c - kFirstChar].ax;
    }
    return w;
}
