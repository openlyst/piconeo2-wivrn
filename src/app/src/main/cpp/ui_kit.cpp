#include "ui_kit.h"
#include "font_atlas.h"
#include <cstring>
#include <cstdio>

// Vertex format: pos.xyz + uv.xy + rgb = 8 floats per vertex.
// Text quads sample from the font atlas texture; non-text quads use
// UV (0,0) which maps to a solid white texel so colour passes through.

// Append a quad (two triangles) with the given UVs and colour.
static inline void emitQuad(std::vector<float> &v,
                            float x0, float y0, float x1, float y1,
                            float u0, float v0, float u1, float v1,
                            float r, float g, float b)
{
    float verts[6][8] = {
        {x0,y0,0, u0,v0, r,g,b},
        {x0,y1,0, u0,v1, r,g,b},
        {x1,y1,0, u1,v1, r,g,b},
        {x0,y0,0, u0,v0, r,g,b},
        {x1,y1,0, u1,v1, r,g,b},
        {x1,y0,0, u1,v0, r,g,b},
    };
    v.insert(v.end(), &verts[0][0], &verts[0][0] + 48);
}

void appendTextLine(std::vector<float> &v, const char *s, float yTop,
                    float px, float r, float g, float b)
{
    // Calculate total width for centering. px is metres per font pixel;
    // the font was rasterized at kPixelHeight, so we scale glyph metrics
    // by px to get panel-local metres.
    float scale = px;
    float totalW = gFont.textWidth(s) * scale;
    float x = -totalW * 0.5f;

    for (const char *p = s; *p; p++) {
        int c = (unsigned char)*p;
        if (c < FontAtlas::kFirstChar || c >= FontAtlas::kFirstChar + FontAtlas::kNumChars)
            c = '?';

        const GlyphInfo &gi = gFont.glyph(c);

        // Glyph bitmap position: bl/bt are in font pixels, positive bt = above baseline.
        // We treat yTop as the top of the text line (ascent line).
        float gx = x + gi.bl * scale;
        float gy = yTop - gi.bt * scale;           // top of glyph bitmap
        float gw = gi.bw * scale;
        float gh = gi.bh * scale;

        if (gi.bw > 0 && gi.bh > 0) {
            emitQuad(v, gx, gy, gx + gw, gy - gh,
                     gi.tx, gi.ty, gi.tx + gi.tw, gi.ty + gi.th,
                     r, g, b);
        }

        x += gi.ax * scale;
    }
}

// Filled rectangle (two triangles, z=0), top-left (xL,yTop) to bottom-right (xR,yBot).
// UVs are (0,0) -> (0,0) so the quad samples the solid white texel.
void appendQuad(std::vector<float> &v, float xL, float yTop, float xR, float yBot,
                float r, float g, float b)
{
    emitQuad(v, xL, yTop, xR, yBot, 0, 0, 0, 0, r, g, b);
}

// Immediate-mode lobby UI kit. Each widget emits pos.xyz+uv.xy+rgb triangles
// into a vertex vector (panel-local metres). WiVRn dark theme, blue accent.
const float kUiText      = 0.0045f;            // standard 1x text size
const float kUiBg[3]     = {0.13f, 0.13f, 0.13f};   // widget / panel surface
const float kUiBgHot[3]  = {0.26f, 0.34f, 0.46f};   // hovered widget
const float kUiTrack[3]  = {0.08f, 0.08f, 0.08f};   // slider track / disabled fill
float       kUiFill[3]   = {0.24f, 0.52f, 0.88f};   // accent
const float kUiOn[3]     = {0.24f, 0.52f, 0.88f};   // toggle on = accent
const float kUiOff[3]    = {0.18f, 0.18f, 0.18f};
const float kUiWhite[3]  = {1.0f, 1.0f, 1.0f};
float       kUiTitle[3]  = {0.90f, 0.90f, 0.92f};   // labels / titles

void uiBox(std::vector<float> &v, const UiRect &r, const float c[3]) {
    appendQuad(v, r.cx-r.w*0.5f, r.cy+r.h*0.5f, r.cx+r.w*0.5f, r.cy-r.h*0.5f, c[0],c[1],c[2]);
}
// Text centred at cx (appendTextLine centres on x=0, so we shift).
void uiTextC(std::vector<float> &v, const char *s, float cx, float yTop, float px,
             float r, float g, float b) {
    std::vector<float> t; appendTextLine(t, s, yTop, px, r, g, b);
    for (size_t i = 0; i < t.size(); i += 8) t[i] += cx;
    v.insert(v.end(), t.begin(), t.end());
}
// Left-aligned text starting at xLeft.
void uiTextL(std::vector<float> &v, const char *s, float xLeft, float yTop, float px,
             float r, float g, float b) {
    float lineW = gFont.textWidth(s) * px;
    uiTextC(v, s, xLeft + lineW * 0.5f, yTop, px, r, g, b);
}
// Plain label (centred), baseline auto from rect centre.
void uiLabel(std::vector<float> &v, const char *s, float cx, float cy, float px,
             const float col[3]) {
    // The font's ascent is roughly 0.8 * lineHeight; offset so the text
    // is vertically centred in the rect.
    float ascent = gFont.lineHeight() * px * 0.75f;
    uiTextC(v, s, cx, cy + ascent, px, col[0], col[1], col[2]);
}
void uiButton(std::vector<float> &v, const UiRect &r, const char *label, bool hot, bool disabled) {
    const float *bg = disabled ? kUiTrack : (hot ? kUiBgHot : kUiBg);
    uiBox(v, r, bg);
    float txt[3] = { disabled ? 0.50f : 1.0f, disabled ? 0.52f : 1.0f, disabled ? 0.56f : 1.0f };
    float ascent = gFont.lineHeight() * kUiText * 0.5f;
    uiTextC(v, label, r.cx, r.cy + ascent, kUiText, txt[0], txt[1], txt[2]);
}
void uiToggle(std::vector<float> &v, const UiRect &r, const char *label, bool on, bool hot, float textScale, bool disabled) {
    uiBox(v, r, disabled ? kUiTrack : (hot ? kUiBgHot : kUiBg));
    float sw = 0.11f, sh = r.h * 0.60f;
    float tpx = kUiText * textScale;
    int n = (int) strlen(label);
    if (n > 0) {
        float labelLeft = r.cx - r.w*0.5f + 0.012f;
        float pillLeft  = r.cx + r.w*0.5f - sw - 0.012f;
        float avail = pillLeft - labelLeft - 0.010f;
        if (avail > 0.0f) {
            float textW = gFont.textWidth(label) * tpx;
            float fit = avail / textW;
            if (fit < 1.0f) tpx *= fit;
        }
    }
    float txt[3] = { disabled ? 0.45f : 0.85f, disabled ? 0.48f : 0.88f, disabled ? 0.52f : 0.92f };
    float ascent = gFont.lineHeight() * tpx * 0.5f;
    uiTextL(v, label, r.cx - r.w*0.5f + 0.012f, r.cy + ascent, tpx, txt[0], txt[1], txt[2]);
    float scx = r.cx + r.w*0.5f - sw*0.5f - 0.012f;
    UiRect track = { scx, r.cy, sw, sh };
    const float *trackCol = on ? kUiOn : kUiOff;
    float dimTrack[3] = { trackCol[0]*0.55f, trackCol[1]*0.55f, trackCol[2]*0.55f };
    uiBox(v, track, disabled ? dimTrack : trackCol);
    float kx = on ? (scx + sw*0.22f) : (scx - sw*0.22f);
    UiRect knob = { kx, r.cy, sw*0.42f, sh*0.82f };
    float dimWhite[3] = { disabled ? 0.55f : 1.0f, disabled ? 0.55f : 1.0f, disabled ? 0.55f : 1.0f };
    uiBox(v, knob, dimWhite);
}
void uiVFader(std::vector<float> &v, const UiRect &r, float frac, bool hot, bool disabled) {
    if (frac < 0) frac = 0; else if (frac > 1) frac = 1;
    float th = r.w * 0.30f;
    appendQuad(v, r.cx-th, r.cy+r.h*0.5f, r.cx+th, r.cy-r.h*0.5f, hot?0.30f:kUiTrack[0], hot?0.30f:kUiTrack[1], hot?0.36f:kUiTrack[2]);
    float yb = r.cy - r.h*0.5f, ky = yb + frac*r.h;
    float fillDim[3] = { kUiFill[0]*0.45f, kUiFill[1]*0.45f, kUiFill[2]*0.45f };
    appendQuad(v, r.cx-th, ky, r.cx+th, yb, disabled?fillDim[0]:kUiFill[0], disabled?fillDim[1]:kUiFill[1], disabled?fillDim[2]:kUiFill[2]);
    float knob[3] = { disabled ? 0.55f : 1.0f, disabled ? 0.55f : 1.0f, disabled ? 0.55f : 1.0f };
    appendQuad(v, r.cx-r.w*0.5f, ky+0.012f, r.cx+r.w*0.5f, ky-0.012f, knob[0], knob[1], knob[2]);
}
void uiHFader(std::vector<float> &v, const UiRect &r, float frac, bool hot, bool disabled) {
    if (frac < 0) frac = 0; else if (frac > 1) frac = 1;
    float th = r.h * 0.30f;
    appendQuad(v, r.cx-r.w*0.5f, r.cy+th, r.cx+r.w*0.5f, r.cy-th, hot?0.30f:kUiTrack[0], hot?0.30f:kUiTrack[1], hot?0.36f:kUiTrack[2]);
    float xl = r.cx - r.w*0.5f, kx = xl + frac*r.w;
    float fillDim[3] = { kUiFill[0]*0.45f, kUiFill[1]*0.45f, kUiFill[2]*0.45f };
    appendQuad(v, xl, r.cy+th, kx, r.cy-th, disabled?fillDim[0]:kUiFill[0], disabled?fillDim[1]:kUiFill[1], disabled?fillDim[2]:kUiFill[2]);
    float knob[3] = { disabled ? 0.55f : 1.0f, disabled ? 0.55f : 1.0f, disabled ? 0.55f : 1.0f };
    appendQuad(v, kx-0.012f, r.cy+r.h*0.5f, kx+0.012f, r.cy-r.h*0.5f, knob[0], knob[1], knob[2]);
}
void uiDropdownHeader(std::vector<float> &v, const UiRect &r, const char *label, bool open, bool hot, bool disabled) {
    uiBox(v, r, disabled ? kUiTrack : (hot ? kUiBgHot : kUiBg));
    char buf[48]; snprintf(buf, sizeof(buf), "%s %s", label, open ? "^" : "~");
    float txt[3] = { disabled ? 0.50f : 1.0f, disabled ? 0.52f : 1.0f, disabled ? 0.56f : 1.0f };
    float ascent = gFont.lineHeight() * kUiText * 0.5f;
    uiTextC(v, buf, r.cx, r.cy + ascent, kUiText, txt[0], txt[1], txt[2]);
}
void uiDropdownItem(std::vector<float> &v, const UiRect &r, const char *label, bool hot, bool disabled) {
    uiBox(v, r, disabled ? kUiTrack : (hot ? kUiBgHot : kUiOff));
    float txt[3] = { disabled ? 0.50f : 1.0f, disabled ? 0.52f : 1.0f, disabled ? 0.56f : 1.0f };
    float ascent = gFont.lineHeight() * kUiText * 0.5f;
    uiTextC(v, label, r.cx, r.cy + ascent, kUiText, txt[0], txt[1], txt[2]);
}
