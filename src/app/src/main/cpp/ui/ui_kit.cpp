#include "ui_kit.h"
#include <cstdint>
#include <cstring>
#include <cstdio>

// 5x7 bitmap font, row-major: 7 bytes/glyph, low 5 bits, bit4 = leftmost column.
// Covers space, 0-9, '.', ':', '-', '/', '%', '(', ')', A-Z, a-z, and a few symbols.
static const uint8_t kFont[][7] = {
    {0,0,0,0,0,0,0},                                   // ' '
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},              // 0
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},              // 1
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},              // 2
    {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},              // 3
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},              // 4
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},              // 5
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},              // 6
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},              // 7
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},              // 8
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},              // 9
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C},              // '.'
    {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00},              // ':'
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},              // '-'
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},              // A
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},              // B
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},              // C
    {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C},              // D
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},              // E
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},              // F
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},              // G
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},              // H
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},              // I
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},              // J
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11},              // K
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},              // L
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},              // M
    {0x11,0x11,0x19,0x15,0x13,0x11,0x11},              // N
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},              // O
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},              // P
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},              // Q
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},              // R
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},              // S
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},              // T
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},              // U
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},              // V
    {0x11,0x11,0x11,0x15,0x15,0x1B,0x11},              // W
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},              // X
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},              // Y
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},              // Z
    {0x00,0x04,0x04,0x0E,0x0E,0x1F,0x00},              // '^' = UP triangle   (index 40)
    {0x00,0x1F,0x0E,0x0E,0x04,0x04,0x00},              // '~' = DOWN triangle (index 41)
    {0x00,0x04,0x04,0x1F,0x04,0x04,0x00},              // '+' = plus          (index 42)
    {0x01,0x02,0x02,0x04,0x08,0x08,0x10},              // '/' = slash         (index 43)
    {0x11,0x12,0x04,0x08,0x04,0x09,0x11},              // '%' = percent       (index 44)
    {0x04,0x08,0x10,0x10,0x10,0x08,0x04},              // '(' = left paren    (index 45)
    {0x04,0x02,0x01,0x01,0x01,0x02,0x04},              // ')' = right paren   (index 46)
    {0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F},              // a (index 47)
    {0x10,0x10,0x1E,0x11,0x11,0x11,0x1E},              // b
    {0x00,0x00,0x0E,0x10,0x10,0x10,0x0E},              // c
    {0x01,0x01,0x0F,0x11,0x11,0x11,0x0F},              // d
    {0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E},              // e
    {0x06,0x08,0x1E,0x08,0x08,0x08,0x08},              // f
    {0x00,0x00,0x0F,0x11,0x11,0x0F,0x06},              // g (descender)
    {0x10,0x10,0x1E,0x11,0x11,0x11,0x11},              // h
    {0x04,0x00,0x0C,0x04,0x04,0x04,0x0E},              // i
    {0x02,0x00,0x06,0x02,0x02,0x12,0x0C},              // j (descender)
    {0x10,0x10,0x12,0x14,0x18,0x14,0x12},              // k
    {0x0C,0x04,0x04,0x04,0x04,0x04,0x0E},              // l
    {0x00,0x00,0x1A,0x15,0x15,0x15,0x15},              // m
    {0x00,0x00,0x1E,0x11,0x11,0x11,0x11},              // n
    {0x00,0x00,0x0E,0x11,0x11,0x11,0x0E},              // o
    {0x00,0x00,0x1E,0x11,0x11,0x1E,0x10},              // p (descender)
    {0x00,0x00,0x0F,0x11,0x11,0x0F,0x01},              // q (descender)
    {0x00,0x00,0x1E,0x11,0x10,0x10,0x10},              // r
    {0x00,0x00,0x0F,0x10,0x0E,0x01,0x1E},              // s
    {0x08,0x08,0x1E,0x08,0x08,0x08,0x06},              // t
    {0x00,0x00,0x11,0x11,0x11,0x11,0x0F},              // u
    {0x00,0x00,0x11,0x11,0x11,0x0A,0x04},              // v
    {0x00,0x00,0x11,0x11,0x15,0x15,0x0A},              // w
    {0x00,0x00,0x11,0x0A,0x04,0x0A,0x11},              // x
    {0x00,0x00,0x11,0x11,0x11,0x0F,0x06},              // y (descender)
    {0x00,0x00,0x1F,0x02,0x04,0x08,0x1F},              // z (index 72)
};
static int glyphIndex(char c) {
    if (c == ' ') return 0;
    if (c >= '0' && c <= '9') return 1 + (c - '0');
    if (c == '.') return 11;
    if (c == ':') return 12;
    if (c == '-') return 13;
    if (c == '^') return 40;
    if (c == '~') return 41;
    if (c == '+') return 42;
    if (c == '/') return 43;
    if (c == '%') return 44;
    if (c == '(') return 45;
    if (c == ')') return 46;
    if (c >= 'a' && c <= 'z') return 47 + (c - 'a');
    if (c >= 'A' && c <= 'Z') return 14 + (c - 'A');
    return 0;
}

void appendTextLine(std::vector<float> &v, const char *s, float yTop,
                    float px, float r, float g, float b) {
    int n = (int) strlen(s);
    float lineW = (n * 6 - 1) * px;     // 5 px glyph + 1 px gap
    float x0 = -lineW * 0.5f;
    for (int i = 0; i < n; i++) {
        const uint8_t *gl = kFont[glyphIndex(s[i])];
        float cx = x0 + i * 6 * px;
        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                if (!(gl[row] & (1 << (4 - col)))) continue;
                float x = cx + col * px, y = yTop - row * px;
                float x1 = x + px, y1 = y - px;
                float q[6][6] = {
                    {x,y,0, r,g,b}, {x,y1,0, r,g,b}, {x1,y1,0, r,g,b},
                    {x,y,0, r,g,b}, {x1,y1,0, r,g,b}, {x1,y,0, r,g,b},
                };
                v.insert(v.end(), &q[0][0], &q[0][0] + 36);
            }
        }
    }
}

// Filled rectangle (two triangles, z=0), top-left (xL,yTop) to bottom-right (xR,yBot).
void appendQuad(std::vector<float> &v, float xL, float yTop, float xR, float yBot,
                float r, float g, float b) {
    float q[6][6] = {
        {xL,yTop,0, r,g,b}, {xL,yBot,0, r,g,b}, {xR,yBot,0, r,g,b},
        {xL,yTop,0, r,g,b}, {xR,yBot,0, r,g,b}, {xR,yTop,0, r,g,b},
    };
    v.insert(v.end(), &q[0][0], &q[0][0] + 36);
}

// Immediate-mode lobby UI kit. Each widget emits pos.xyz+rgb triangles into a
// vertex vector (panel-local metres). WiVRn dark theme, blue accent.
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
    for (size_t i = 0; i < t.size(); i += 6) t[i] += cx;
    v.insert(v.end(), t.begin(), t.end());
}
// Left-aligned text starting at xLeft.
void uiTextL(std::vector<float> &v, const char *s, float xLeft, float yTop, float px,
             float r, float g, float b) {
    int n = (int) strlen(s);
    float lineW = (n * 6 - 1) * px;
    uiTextC(v, s, xLeft + lineW * 0.5f, yTop, px, r, g, b);
}
// Plain label (centred), baseline auto from rect centre.
void uiLabel(std::vector<float> &v, const char *s, float cx, float cy, float px,
             const float col[3]) {
    uiTextC(v, s, cx, cy + 3.5f*px, px, col[0], col[1], col[2]);
}
void uiButton(std::vector<float> &v, const UiRect &r, const char *label, bool hot, bool disabled) {
    const float *bg = disabled ? kUiTrack : (hot ? kUiBgHot : kUiBg);
    uiBox(v, r, bg);
    float txt[3] = { disabled ? 0.50f : 1.0f, disabled ? 0.52f : 1.0f, disabled ? 0.56f : 1.0f };
    uiTextC(v, label, r.cx, r.cy + 3.5f*kUiText, kUiText, txt[0], txt[1], txt[2]);
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
        if (avail > 0.0f) { float fit = avail / (n*6 - 1); if (fit < tpx) tpx = fit; }
    }
    float txt[3] = { disabled ? 0.45f : 0.85f, disabled ? 0.48f : 0.88f, disabled ? 0.52f : 0.92f };
    uiTextL(v, label, r.cx - r.w*0.5f + 0.012f, r.cy + 3.5f*tpx, tpx, txt[0], txt[1], txt[2]);
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
    uiTextC(v, buf, r.cx, r.cy + 3.5f*kUiText, kUiText, txt[0], txt[1], txt[2]);
}
void uiDropdownItem(std::vector<float> &v, const UiRect &r, const char *label, bool hot, bool disabled) {
    uiBox(v, r, disabled ? kUiTrack : (hot ? kUiBgHot : kUiOff));
    float txt[3] = { disabled ? 0.50f : 1.0f, disabled ? 0.52f : 1.0f, disabled ? 0.56f : 1.0f };
    uiTextC(v, label, r.cx, r.cy + 3.5f*kUiText, kUiText, txt[0], txt[1], txt[2]);
}
