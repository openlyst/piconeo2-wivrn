#pragma once
// Immediate-mode lobby UI kit + stb_truetype font renderer. Pure geometry
// emitters: each widget appends pos.xyz+uv.xy+rgb triangles into a vertex
// vector (panel-local metres). Text quads sample from the font atlas
// texture; non-text quads use UV (0,0) which maps to a solid white texel
// so their per-vertex colour passes through unchanged.
#include <vector>

// Append one centred line of text as textured quads. px = metres per font pixel.
void appendTextLine(std::vector<float> &v, const char *s, float yTop,
                    float px, float r, float g, float b);
// Filled rectangle (two triangles, z=0), top-left (xL,yTop) to bottom-right (xR,yBot).
// UVs are (0,0) so the quad samples the solid white texel in the font atlas.
void appendQuad(std::vector<float> &v, float xL, float yTop, float xR, float yBot,
                float r, float g, float b);

// ---- widget kit ----------------------------------------------------------
struct UiRect { float cx, cy, w, h; };   // centre + full size (panel-local metres)
static inline bool uiHit(const UiRect &r, float lx, float ly) {
    return lx >= r.cx - r.w*0.5f && lx <= r.cx + r.w*0.5f &&
           ly >= r.cy - r.h*0.5f && ly <= r.cy + r.h*0.5f;
}

// Shared visual language (defined in ui_kit.cpp).
extern const float kUiText;        // standard 1x text size
extern const float kUiBg[3];
extern const float kUiBgHot[3];
extern const float kUiTrack[3];
extern float       kUiFill[3];     // accent
extern const float kUiOn[3];       // green = on
extern const float kUiOff[3];
extern const float kUiWhite[3];
extern float       kUiTitle[3];    // title/label text

void uiBox(std::vector<float> &v, const UiRect &r, const float c[3]);
void uiTextC(std::vector<float> &v, const char *s, float cx, float yTop, float px,
             float r, float g, float b);
void uiTextL(std::vector<float> &v, const char *s, float xLeft, float yTop, float px,
             float r, float g, float b);
void uiLabel(std::vector<float> &v, const char *s, float cx, float cy, float px,
             const float col[3]);
void uiButton(std::vector<float> &v, const UiRect &r, const char *label, bool hot, bool disabled = false);
void uiToggle(std::vector<float> &v, const UiRect &r, const char *label, bool on, bool hot, float textScale = 1.0f, bool disabled = false);
void uiVFader(std::vector<float> &v, const UiRect &r, float frac, bool hot, bool disabled = false);
void uiHFader(std::vector<float> &v, const UiRect &r, float frac, bool hot, bool disabled = false);
void uiDropdownHeader(std::vector<float> &v, const UiRect &r, const char *label, bool open, bool hot, bool disabled = false);
void uiDropdownItem(std::vector<float> &v, const UiRect &r, const char *label, bool hot, bool disabled = false);
