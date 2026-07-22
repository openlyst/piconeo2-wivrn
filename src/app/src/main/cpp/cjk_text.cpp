#include "cjk_text.h"
#include "log.h"
#include <GLES3/gl3.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <cstring>
#include <cmath>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

// Asset manager handle set from JNI at startup.
extern AAssetManager * gAssetManager;

bool CjkText::init(float pxHeight) {
    if (!gAssetManager) {
        LOGE("cjk_text: no asset manager");
        return false;
    }
    AAsset *asset = AAssetManager_open(gAssetManager, "fonts/test_cjk.otf", AASSET_MODE_BUFFER);
    if (!asset) {
        LOGE("cjk_text: font asset not found");
        return false;
    }
    size_t fontLen = AAsset_getLength(asset);
    std::vector<uint8_t> fontBuf(fontLen);
    AAsset_read(asset, fontBuf.data(), fontLen);
    AAsset_close(asset);

    stbtt_fontinfo font;
    if (!stbtt_InitFont(&font, fontBuf.data(), stbtt_GetFontOffsetForIndex(fontBuf.data(), 0))) {
        LOGE("cjk_text: stbtt_InitFont failed");
        return false;
    }
    fontScale_ = stbtt_ScaleForPixelHeight(&font, pxHeight);

    // Build the glyph list: ASCII printable + the 4 CJK chars we need.
    // For a real i18n pass you'd bake the GB2312 set; this is a proof of concept.
    struct Range { int first; int last; };
    static const Range ranges[] = {
        {0x20, 0x7E},           // ASCII printable
        {0x4F60, 0x4F60},       // 你
        {0x597D, 0x597D},       // 好
        {0x4E16, 0x4E16},       // 世
        {0x754C, 0x754C},       // 界
    };

    // First pass: count glyphs + measure atlas.
    int glyphCount = 0;
    for (const auto &r : ranges)
        glyphCount += (r.last - r.first + 1);

    // Atlas: pack glyphs into a square. 256x256 is plenty for ~100 glyphs at 32px.
    atlasW_ = 256;
    atlasH_ = 256;
    std::vector<uint8_t> atlas(atlasW_ * atlasH_, 0);

    stbtt_pack_context pack;
    if (!stbtt_PackBegin(&pack, atlas.data(), atlasW_, atlasH_, 0, 1, nullptr)) {
        LOGE("cjk_text: stbtt_PackBegin failed");
        return false;
    }
    stbtt_PackSetOversampling(&pack, 1, 1);

    // Pack each range; store glyph metrics.
    stbtt_packedchar packed[256];
    glyphs_.clear();
    memset(cpToIdx_, -1, sizeof(cpToIdx_));

    for (const auto &r : ranges) {
        int n = r.last - r.first + 1;
        stbtt_PackFontRange(&pack, fontBuf.data(), 0, pxHeight, r.first, n, packed);
        for (int i = 0; i < n; i++) {
            int cp = r.first + i;
            if (cp >= kMaxCp) continue;
            const stbtt_packedchar &pc = packed[i];
            CjkGlyph g;
            g.x0 = pc.x0 / (float)atlasW_;
            g.y0 = pc.y0 / (float)atlasH_;
            g.x1 = pc.x1 / (float)atlasW_;
            g.y1 = pc.y1 / (float)atlasH_;
            g.xoff = pc.xoff;
            g.yoff = pc.yoff;
            g.advance = pc.xadvance;
            g.pw = (float)(pc.x1 - pc.x0);
            g.ph = (float)(pc.y1 - pc.y0);
            cpToIdx_[cp] = (int16_t)glyphs_.size();
            glyphs_.push_back(g);
        }
    }
    stbtt_PackEnd(&pack);

    // Upload atlas as GL_ALPHA8 -> use as luminance in shader.
    glGenTextures(1, &tex_);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, atlasW_, atlasH_, 0, GL_RED, GL_UNSIGNED_BYTE, atlas.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    LOGI("cjk_text: atlas %dx%d, %zu glyphs, tex=%u", atlasW_, atlasH_, glyphs_.size(), tex_);
    return true;
}

// Decode one UTF-8 codepoint. Returns bytes consumed or 0 on error.
static int utf8Decode(const char *s, int len, int *cp) {
    if (len <= 0) return 0;
    uint8_t c = (uint8_t)s[0];
    if (c < 0x80) { *cp = c; return 1; }
    if ((c & 0xE0) == 0xC0 && len >= 2) { *cp = ((c & 0x1F) << 6) | ((uint8_t)s[1] & 0x3F); return 2; }
    if ((c & 0xF0) == 0xE0 && len >= 3) { *cp = ((c & 0x0F) << 12) | (((uint8_t)s[1] & 0x3F) << 6) | ((uint8_t)s[2] & 0x3F); return 3; }
    if ((c & 0xF8) == 0xF0 && len >= 4) { *cp = ((c & 0x07) << 18) | (((uint8_t)s[1] & 0x3F) << 12) | (((uint8_t)s[2] & 0x3F) << 6) | ((uint8_t)s[3] & 0x3F); return 4; }
    return 0;
}

int CjkText::emitQuads(std::vector<float> &v, const char *utf8, float x, float y, float px,
                       float r, float g, float b) const {
    if (!tex_) return 0;
    int len = (int)strlen(utf8);
    int i = 0, vertCount = 0;
    float cursorX = x;
    while (i < len) {
        int cp = 0;
        int adv = utf8Decode(utf8 + i, len - i, &cp);
        if (adv == 0) { i++; continue; }
        i += adv;
        if (cp >= kMaxCp || cpToIdx_[cp] < 0) continue;
        const CjkGlyph &gl = glyphs_[cpToIdx_[cp]];

        // Atlas pixel coords -> metres via px (metres per atlas pixel).
        float gx0 = cursorX + gl.xoff * px;
        float gy0 = y + gl.yoff * px;
        float gx1 = gx0 + gl.pw * px;
        float gy1 = gy0 + gl.ph * px;

        float u0 = gl.x0, v0 = gl.y0, u1 = gl.x1, v1 = gl.y1;
        float verts[6][8] = {
            {gx0, gy0, 0, u0, v0, r, g, b},
            {gx1, gy0, 0, u1, v0, r, g, b},
            {gx1, gy1, 0, u1, v1, r, g, b},
            {gx0, gy0, 0, u0, v0, r, g, b},
            {gx1, gy1, 0, u1, v1, r, g, b},
            {gx0, gy1, 0, u0, v1, r, g, b},
        };
        for (int k = 0; k < 6; k++)
            v.insert(v.end(), verts[k], verts[k] + 8);
        vertCount += 6;
        cursorX += gl.advance * px;
    }
    return vertCount;
}
