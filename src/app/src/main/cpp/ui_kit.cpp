#include "ui_kit.h"
#include "cjk_text.h"    // gCjkText, appendQuad8
#include "i18n.h"        // tr()
#include <cstdint>
#include <cstring>
#include <cstdio>

// All UI geometry now uses 8-float verts: pos.xyz + uv.xy + color.rgb.
// Text uses the stb_truetype atlas; flat quads use UV (0,0) which maps to
// the white pixel in the atlas, rendering as solid vertex colour.

// Bake height of the atlas font. Used to convert between metres and atlas px.
static constexpr float kBakeHeight = 48.0f;

// Baseline offset for vertically centering text. For a 48px bake the visual
// center sits ~14px above the baseline (ascent ~38, descent ~10).
float baselineOffset(float px) {
    return kBakeHeight * 0.30f * px;
}

void appendTextLine(std::vector<float> &v, const char *s, float yTop,
                    float px, float r, float g, float b) {
    if (!gCjkText.ready()) return;
    float w = gCjkText.textWidth(s) * px;
    gCjkText.emitQuads(v, s, -w * 0.5f, yTop, px, r, g, b);
}

void appendQuad(std::vector<float> &v, float xL, float yTop, float xR, float yBot,
                float r, float g, float b) {
    appendQuad8(v, xL, yTop, xR, yBot, r, g, b);
}

// ---- widget kit ----------------------------------------------------------
const float kUiText      = 0.000656f;         // metres per atlas pixel (~31.5mm text)
const float kUiBg[3]     = {0.13f, 0.13f, 0.13f};
const float kUiBgHot[3]  = {0.26f, 0.34f, 0.46f};
const float kUiTrack[3]  = {0.08f, 0.08f, 0.08f};
float       kUiFill[3]   = {0.24f, 0.52f, 0.88f};
const float kUiOn[3]     = {0.24f, 0.52f, 0.88f};
const float kUiOff[3]    = {0.18f, 0.18f, 0.18f};
const float kUiWhite[3]  = {1.0f, 1.0f, 1.0f};
float       kUiTitle[3]  = {0.90f, 0.90f, 0.92f};

void uiBox(std::vector<float> &v, const UiRect &r, const float c[3]) {
    appendQuad(v, r.cx-r.w*0.5f, r.cy+r.h*0.5f, r.cx+r.w*0.5f, r.cy-r.h*0.5f, c[0],c[1],c[2]);
}
void uiTextC(std::vector<float> &v, const char *s, float cx, float yTop, float px,
             float r, float g, float b) {
    if (!gCjkText.ready()) return;
    float w = gCjkText.textWidth(s) * px;
    gCjkText.emitQuads(v, s, cx - w * 0.5f, yTop, px, r, g, b);
}
void uiTextL(std::vector<float> &v, const char *s, float xLeft, float yTop, float px,
             float r, float g, float b) {
    gCjkText.emitQuads(v, s, xLeft, yTop, px, r, g, b);
}
void uiLabel(std::vector<float> &v, const char *s, float cx, float cy, float px,
             const float col[3]) {
    uiTextC(v, s, cx, cy - baselineOffset(px), px, col[0], col[1], col[2]);
}
void uiButton(std::vector<float> &v, const UiRect &r, const char *label, bool hot, bool disabled) {
    const float *bg = disabled ? kUiTrack : (hot ? kUiBgHot : kUiBg);
    uiBox(v, r, bg);
    float txt[3] = { disabled ? 0.50f : 1.0f, disabled ? 0.52f : 1.0f, disabled ? 0.56f : 1.0f };
    uiTextC(v, label, r.cx, r.cy - baselineOffset(kUiText), kUiText, txt[0], txt[1], txt[2]);
}
void uiToggle(std::vector<float> &v, const UiRect &r, const char *label, bool on, bool hot, float textScale, bool disabled) {
    uiBox(v, r, disabled ? kUiTrack : (hot ? kUiBgHot : kUiBg));
    float sw = 0.11f, sh = r.h * 0.60f;
    float tpx = kUiText * textScale;
    if (label[0]) {
        float labelLeft = r.cx - r.w*0.5f + 0.012f;
        float pillLeft  = r.cx + r.w*0.5f - sw - 0.012f;
        float avail = pillLeft - labelLeft - 0.010f;
        if (avail > 0.0f && gCjkText.ready()) {
            float natW = gCjkText.textWidth(label) * tpx;
            if (natW > avail) tpx *= avail / natW;
        }
    }
    float txt[3] = { disabled ? 0.45f : 0.85f, disabled ? 0.48f : 0.88f, disabled ? 0.52f : 0.92f };
    uiTextL(v, label, r.cx - r.w*0.5f + 0.012f, r.cy - baselineOffset(tpx), tpx, txt[0], txt[1], txt[2]);
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
    char buf[64]; snprintf(buf, sizeof(buf), "%s %s", label, open ? "^" : "~");
    float txt[3] = { disabled ? 0.50f : 1.0f, disabled ? 0.52f : 1.0f, disabled ? 0.56f : 1.0f };
    uiTextC(v, buf, r.cx, r.cy - baselineOffset(kUiText), kUiText, txt[0], txt[1], txt[2]);
}
void uiDropdownItem(std::vector<float> &v, const UiRect &r, const char *label, bool hot, bool disabled) {
    uiBox(v, r, disabled ? kUiTrack : (hot ? kUiBgHot : kUiOff));
    float txt[3] = { disabled ? 0.50f : 1.0f, disabled ? 0.52f : 1.0f, disabled ? 0.56f : 1.0f };
    uiTextC(v, label, r.cx, r.cy - baselineOffset(kUiText), kUiText, txt[0], txt[1], txt[2]);
}
