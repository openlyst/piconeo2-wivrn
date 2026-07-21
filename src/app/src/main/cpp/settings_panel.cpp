#include "settings_panel.h"
#include "app_state.h"
#include "eq_panel.h"
#include "menu_model.h"
#include "passthrough.h"
#include "eye_tracking.h"   // gEyeSupported (disable toggle on non-EYE hw)
#include "server_list.h"    // wiVRn server list tab
#include "log.h"        // nowNs()
#include "streaming/streaming_client.h"
#include <cstdio>
#include <cstring>
#include <cmath>

// session state
bool gSettingsOpen = true;   // always open: unified lobby panel
int  gSettingsCat  = 0;
std::vector<float> gSettingsScroll;

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ===========================================================================
// AUDIO category: 16-band EQ as a MK_CUSTOM item. MenuHover.part encodes the
// sub-region: 0..15 = band track, 100..115 = band knob, 200 = RESET,
// 201 = preset header, 300+i = dropdown item i.
// ===========================================================================
static void eqBuild(std::vector<float> &v, const MenuHover &h) {
    int band = (h.part >= 0 && h.part < 16)     ? h.part
             : (h.part >= 100 && h.part < 116)  ? h.part - 100 : -1;
    bool reset  = (h.part == 200);
    bool header = (h.part == 201);
    int  item   = (h.part >= 300) ? h.part - 300 : -1;
    buildEqVerts(v, band, reset, header, item);
}
static void eqHit(float cx, float cy, MenuHover &h) {
    if (cx >= kEqResetX0 && cx <= kEqResetX1 && cy <= kEqResetYTop && cy >= kEqResetYBot) {
        h.item = 0; h.part = 200; h.grab = true; return;
    }
    if (cx >= kEqPresetX0 && cx <= kEqPresetX1 && cy <= kEqPresetYTop && cy >= kEqPresetYBot) {
        h.item = 0; h.part = 201; h.grab = true; return;
    }
    if (gEqPresetOpen && cx >= kEqPresetX0 && cx <= kEqPresetX1 &&
        cy < kEqPresetYBot && cy >= kEqPresetYBot - kEqNumPresets*kEqItemH) {
        int it = (int)((kEqPresetYBot - cy) / kEqItemH);
        if (it >= 0 && it < kEqNumPresets) { h.item = 0; h.part = 300 + it; h.grab = true; }
        return;
    }
    if (cx >= kEqX0 && cx <= kEqX1 && cy <= kEqYTrackTop+0.03f && cy >= kEqYTrackBot-0.03f) {
        float colW = (kEqX1 - kEqX0) / kEqBands;
        int b = (int)((cx - kEqX0) / colW);
        if (b < 0) b = 0; else if (b >= kEqBands) b = kEqBands-1;
        // Drag-draw only arms on the knob; the rest of the column scrolls.
        float lky = eqGainToY(gEqGains[b]);
        bool onKnob = (fabsf(cx - eqColCenterX(b)) <= colW*0.40f && fabsf(cy - lky) <= 0.045f);
        h.item = 0; h.part = onKnob ? (100 + b) : b; h.grab = onKnob;
    }
}
static void eqAct(const MenuHover &h, bool click, bool grab, float cx, float cy) {
    static bool sArmed = false;
    // current column under the pointer (drag can sweep across columns)
    int band = -1;
    if (cx >= kEqX0 && cx <= kEqX1) {
        float colW = (kEqX1 - kEqX0) / kEqBands;
        band = (int)((cx - kEqX0) / colW);
        if (band < 0) band = 0; else if (band >= kEqBands) band = kEqBands-1;
    }
    if (click && h.part >= 100 && h.part < 116) sArmed = true;
    if (sArmed && grab && band >= 0) {
        if (!gEqGrabbing) { gEqGrabbing = true; gEqActiveBand = band; }
        float ng = eqYToGain(cy);
        int to = band, from = (gEqActiveBand >= 0) ? gEqActiveBand : to;
        float fromG = gEqGains[from];
        if (from == to) gEqGains[to] = ng;
        else {
            int step = (to > from) ? 1 : -1;
            int n = (to > from) ? (to - from) : (from - to);
            for (int bb = from; ; bb += step) {
                int d = (bb > from) ? (bb - from) : (from - bb);
                gEqGains[bb] = fromG + (ng - fromG) * ((float)d / (float)n);
                if (bb == to) break;
            }
        }
        gEqActiveBand = to;
        for (int i = 0; i < kEqBands; i++) gEqCustoms[gEqPresetIdx][i] = gEqGains[i];
        gEqDirty = true; gEqChangeNs = nowNs();
        pushEqGains();
    }
    if (!grab) { sArmed = false; if (gEqGrabbing) { gEqGrabbing = false; gEqActiveBand = -1; } }
    if (click && h.part >= 200) {
        if (h.part == 200) {           // RESET
            for (int i = 0; i < kEqBands; i++) { gEqGains[i] = 0.0f; gEqCustoms[gEqPresetIdx][i] = 0.0f; }
            pushEqGains(); saveEqProfile();
            LOGI("EQ: reset %s to flat", kEqPresetNames[gEqPresetIdx]);
        } else if (h.part == 201) {    // preset header
            gEqPresetOpen = !gEqPresetOpen;
        } else if (h.part >= 300) {    // dropdown item
            applyEqPreset(h.part - 300); gEqPresetOpen = false;
        }
    }
}

// Toggle that's interactive.
static inline MenuItem wivrnToggle(const char *label, std::atomic<bool> &val) {
    MenuItem it; it.kind = MK_TOGGLE; it.label = label;
    it.get = [&]{ return val.load() ? 1.0f : 0.0f; };
    it.set = [&](float v){ val.store(v > 0.5f); };
    return it;
}

// ---- model assembly --------------------------------------------------------
// wiVRn-style tab layout:
//   Top:    SERVERS, SETTINGS
//   Bottom: ABOUT, LICENSES, EXIT
static void buildCoreModel(MenuModel &m) {
    // SERVERS (wiVRn-style server list, first tab) --------------------------
    MenuCategory servers; servers.name = "SERVERS"; servers.custom = true;
    {
        MenuItem srv; srv.kind = MK_CUSTOM;
        srv.customH = 0.8f;
        srv.cBuild = [](std::vector<float> &v, const MenuHover &h) {
            int hoverItem = (h.item >= 0) ? h.item : -1;
            int connectHot = (h.item >= 0 && h.part == 1) ? h.item : -1;
            float scrollY = settingsScroll();
            buildServerContent(v, scrollY, hoverItem, connectHot);
        };
        srv.cHit = [](float cx, float cy, MenuHover &h) {
            float scrollY = settingsScroll();
            SrvHover sh = hitServerContent(cx, cy, scrollY);
            h.item = sh.item; h.part = sh.part; h.grab = sh.grab;
        };
        srv.cAct = [](const MenuHover &h, bool click, bool grab, float cx, float cy) {
            SrvHover sh; sh.item = h.item; sh.part = h.part; sh.grab = h.grab;
            applyServerClick(sh, click);
        };
        servers.items.push_back(srv);
    }
    m.push_back(servers);

    // SETTINGS (only options we actually support) ---------------------------
    MenuCategory settings; settings.name = "SETTINGS";
    {
        MenuItem ipd; ipd.kind = MK_STEPPER; ipd.label = "SOFTWARE IPD";
        ipd.vmin = kIpdMin; ipd.vmax = kIpdMax; ipd.vstep = kIpdStep;
        ipd.get = []{ return gSoftIpdMm.load(); };
        ipd.set = [](float v){ gSoftIpdMm.store(v); gIpdChangeNs.store(nowNs()); gIpdDirty.store(true); };
        ipd.valueText = [](char *b,int n){ snprintf(b,n,"%.1f MM", gSoftIpdMm.load()); };
        settings.items.push_back(ipd);

        MenuItem br; br.kind = MK_FADER; br.label = "BRIGHTNESS";
        br.vmin = 0.0f; br.vmax = 1.0f;
        br.get = []{ return gBrightnessFrac.load(); };
        br.set = [](float f){ gBrightnessFrac.store(f); gBrightnessSaved.store(true); gBrightnessApply.store(true); };
        br.valueText = [](char *b,int n){ snprintf(b,n,"%d PERCENT",(int)(gBrightnessFrac.load()*100.0f+0.5f)); };
        br.onCommit = []{ saveBrightness(); };
        settings.items.push_back(br);

        MenuItem fov; fov.kind = MK_FADER; fov.label = "FIELD OF VIEW";
        fov.vmin = kFovMin; fov.vmax = kFovMax;
        fov.get = []{ return gStreamFovDeg.load(); };
        fov.set = [](float v){ gStreamFovDeg.store(roundf(v)); };
        fov.valueText = [](char *b,int n){ snprintf(b,n,"%.0f DEG", gStreamFovDeg.load()); };
        fov.onCommit = []{ gFovDirty.store(true); };
        settings.items.push_back(fov);

        MenuItem res; res.kind = MK_FADER; res.label = "RESOLUTION SCALE";
        res.vmin = 0.5f; res.vmax = 2.0f;
        res.get = []{ return gWivrnResolutionScale.load(); };
        res.set = [](float v){ gWivrnResolutionScale.store(v); };
        res.valueText = [](char *b,int n){ snprintf(b,n,"%.2f X", gWivrnResolutionScale.load()); };
        res.onCommit = []{
            if (g_stream && g_stream->session) {
                constexpr int base_w = 1664;
                constexpr int base_h = 1756;
                float s = gWivrnResolutionScale.load();
                int rw = (int)(base_w * s);
                int rh = (int)(base_h * s);
                rw = (rw / 2) * 2;
                rh = (rh / 2) * 2;
                g_stream->eye_width.store(rw);
                g_stream->eye_height.store(rh);
                g_stream->stream_eye_width.store(rw);
                g_stream->stream_eye_height.store(rh);
                g_stream->blit_pipeline.set_resolution(rw, rh);
                g_stream->resolution_dirty.store(true);
                g_stream->send_headset_info();
                LOGI("Resolution scale changed to %.2f (%dx%d), sent headset_info", s, rw, rh);
            }
            saveAllConfig();
        };
        settings.items.push_back(res);

        MenuItem bit; bit.kind = MK_FADER; bit.label = "BITRATE";
        bit.vmin = 5.0f; bit.vmax = 200.0f;
        bit.get = []{ return gWivrnBitrateMbps.load(); };
        bit.set = [](float v){ gWivrnBitrateMbps.store(v); };
        bit.valueText = [](char *b,int n){ snprintf(b,n,"%.0f MBPS", gWivrnBitrateMbps.load()); };
        bit.onCommit = []{
            int mbps = (int)gWivrnBitrateMbps.load();
            if (g_stream) {
                g_stream->bitrate_mbps.store(mbps);
                g_stream->send_bitrate_change(mbps);
            }
            saveAllConfig();
        };
        settings.items.push_back(bit);

        MenuItem eyeFov = wivrnToggle("EYE-TRACKED FOVEATION", gWivrnEyeFoveation);
        eyeFov.disabled = !gEyeSupported.load();
        eyeFov.onChange = []{ saveAllConfig(); gEyeFoveationDirty.store(true); };
        settings.items.push_back(eyeFov);

        MenuItem mic = wivrnToggle("MICROPHONE", gWivrnMicrophone);
        mic.onChange = []{
            bool on = gWivrnMicrophone.load();
            if (g_stream) {
                g_stream->microphone_enabled.store(on);
                if (g_stream->audio_handle)
                    g_stream->audio_handle->set_mic_state(on);
                g_stream->send_headset_info();
            }
            saveAllConfig();
        };
        settings.items.push_back(mic);

        MenuItem vib; vib.kind = MK_FADER; vib.label = "CONTROLLER VIBRATION";
        vib.vmin = 0.0f; vib.vmax = 1.0f;
        vib.get = []{ return gWivrnCtrlVibration.load(); };
        vib.set = [](float v){ gWivrnCtrlVibration.store(v); };
        vib.valueText = [](char *b,int n){ snprintf(b,n,"%d PCT", (int)(gWivrnCtrlVibration.load()*100.0f+0.5f)); };
        vib.onCommit = []{ saveAllConfig(); };
        settings.items.push_back(vib);

        MenuItem pt = wivrnToggle("PASSTHROUGH", gWivrnPassthrough);
        pt.onChange = []{
            extern pico_passthrough * gPassthrough;
            if (gWivrnPassthrough.load()) {
                if (gPassthrough && !gPassthrough->is_camera_on()) gPassthrough->start();
            } else {
                if (gPassthrough && gPassthrough->is_camera_on()) gPassthrough->stop();
            }
            saveAllConfig();
        };
        settings.items.push_back(pt);

        MenuItem rec; rec.kind = MK_BUTTON; rec.label = "RECENTER";
        rec.onClick = []{ gWivrnRecenterReq.store(true); };
        settings.items.push_back(rec);

        MenuItem eye; eye.kind = MK_TOGGLE; eye.label = "EYE DEBUG";
        eye.get = []{ return gEyeDebugOn.load()?1.0f:0.0f; };
        eye.set = [](float v){ gEyeDebugOn.store(v>0.5f); };
        eye.onChange = []{ saveEyeDebug(); gEyeTrackReapply.store(true); };
        settings.items.push_back(eye);

        MenuItem diag; diag.kind = MK_DROPDOWN; diag.label = "DIAGNOSTICS HUD";
        diag.options = { "OFF", "PIPELINE", "SYSTEM" };
        diag.get = []{ return (float)gDiagHudMode.load(); };
        diag.set = [](float v){ gDiagHudMode.store((int)(v+0.5f)); };
        diag.onChange = []{ saveDiagHud(); };
        settings.items.push_back(diag);
    }
    m.push_back(settings);

    // ABOUT (bottom) --------------------------------------------------------
    MenuCategory about; about.name = "ABOUT";
    {
        MenuItem info; info.kind = MK_BUTTON; info.label = "WIVRN FOR PICO NEO 2";
        info.disabled = true;
        about.items.push_back(info);

        MenuItem ver; ver.kind = MK_BUTTON; ver.label = "VERSION: POC";
        ver.disabled = true;
        about.items.push_back(ver);

        MenuItem url; url.kind = MK_BUTTON; url.label = "GITHUB.COM/WIVRN/WIVRN";
        url.disabled = true;
        about.items.push_back(url);
    }
    m.push_back(about);

    // LICENSES (bottom) -----------------------------------------------------
    MenuCategory licenses; licenses.name = "LICENSES";
    {
        MenuItem info; info.kind = MK_BUTTON; info.label = "GPL V3 OR LATER";
        info.disabled = true;
        licenses.items.push_back(info);

        MenuItem note; note.kind = MK_BUTTON; note.label = "SEE WIVRN REPO FOR DETAILS";
        note.disabled = true;
        licenses.items.push_back(note);
    }
    m.push_back(licenses);

    // EXIT (bottom) ---------------------------------------------------------
    MenuCategory exit; exit.name = "EXIT";
    {
        MenuItem quit; quit.kind = MK_BUTTON; quit.label = "QUIT WIVRN";
        quit.onClick = []{
            extern std::function<void()> gOnExit;
            if (gOnExit) gOnExit();
        };
        exit.items.push_back(quit);
    }
    m.push_back(exit);
}

MenuModel &settingsModel() {
    static MenuModel m;
    if (m.empty()) {
        buildCoreModel(m);
        gSettingsScroll.assign(m.size(), 0.0f);
    }
    return m;
}
int  settingsNumCats() { return (int)settingsModel().size(); }
void settingsClampCat() { int n = settingsNumCats(); if (gSettingsCat < 0) gSettingsCat = 0; else if (gSettingsCat >= n) gSettingsCat = n-1; }
float &settingsScroll() { settingsModel(); settingsClampCat(); return gSettingsScroll[gSettingsCat]; }

// wiVRn-style sidebar: top group (SERVERS, SETTINGS)
// then gap, then bottom group (ABOUT, LICENSES, EXIT) anchored to bottom.
UiRect settingsTabRect(int i) {
    int nTop = 2;       // top group
    float tabH = 0.09f;
    float tabGap = 0.02f;
    float sidebarL = kSetPanelL + 0.02f;
    float sidebarR = kSidebarR - 0.02f;
    float tabW = sidebarR - sidebarL;
    float sidebarX = (sidebarL + sidebarR) * 0.5f;
    if (i < nTop) {
        float yTop = kSetPanelTop - 0.02f;
        return { sidebarX, yTop - tabH*0.5f - i*(tabH + tabGap), tabW, tabH };
    } else {
        int bi = i - nTop;  // 0=ABOUT, 1=LICENSES, 2=EXIT
        float yBot = kSetPanelBot + 0.02f;
        int nBot = 3;
        return { sidebarX, yBot + tabH*0.5f + (nBot-1-bi)*(tabH + tabGap), tabW, tabH };
    }
}

// Signature of everything that can move the active category's geometry (item set,
// kinds, dropdown-open states, EQ preset dropdown). Hover/scroll don't change
// vertex positions, so the extent can be cached and re-measured only on change.
static unsigned menuStructSig(const MenuCategory &c) {
    unsigned h = 2166136261u;
    auto mix = [&](unsigned x){ h = (h ^ x) * 16777619u; };
    mix((unsigned)c.items.size());
    for (const auto &it : c.items) { mix((unsigned)it.kind); mix(it.dropOpen ? 1u : 0u);
                                     mix((unsigned)it.options.size()); }
    mix(gEqPresetOpen ? 1u : 0u);
    return h;
}

// ---- measure (top-align active content; scan real vertex extent) ------------
// Extent depends only on menu structure (menuStructSig), so it's cached per
// (category,structure). Build-to-measure only on structure change; steady state
// is a cache hit (no per-frame double-build).
void settingsMeasure(float &offX, float &offY, float &contentH) {
    settingsModel(); settingsClampCat();
    MenuCategory &cat = settingsModel()[gSettingsCat];

    static int      sExtCat = -1;
    static unsigned sExtSig = 0;
    static float    sExtMaxY = 0.0f, sExtContentH = 0.0f;

    unsigned sig = menuStructSig(cat);
    float maxY;
    if (sExtCat == gSettingsCat && sExtSig == sig) {
        maxY = sExtMaxY; contentH = sExtContentH;
    } else {
        static std::vector<float> tmp; tmp.clear();
        MenuHover none;
        menuBuild(tmp, cat, none);
        float minY = 1e9f, mxY = -1e9f;
        for (size_t i = 1; i < tmp.size(); i += 6) { float y = tmp[i]; if (y<minY) minY=y; if (y>mxY) mxY=y; }
        if (mxY < minY) { minY = 0; mxY = 0; }
        maxY = mxY; contentH = mxY - minY;
        sExtCat = gSettingsCat; sExtSig = sig; sExtMaxY = maxY; sExtContentH = contentH;
    }
    float maxScroll = contentH - kSetViewportH; if (maxScroll < 0) maxScroll = 0;
    float &s = gSettingsScroll[gSettingsCat];
    s = clampf(s, 0.0f, maxScroll);
    offX = kSetContentOffX;
    offY = (kCtTop - maxY) + s;
}

void buildSettingsPanel(std::vector<float> &v, float offX, float offY, float contentH,
                        const MenuHover &content, int tabHover, bool closeHover) {
    MenuModel &m = settingsModel();
    int cat = gSettingsCat;
    // wiVRn-style: pure black sidebar, grey semi-transparent content area.
    // Sidebar: pure black (0,0,0)
    appendQuad(v, kSetPanelL, kSetPanelTop, kSidebarR, kSetPanelBot, 0.0f, 0.0f, 0.0f);
    // Content area: dark grey semi-transparent (wiVRn uses 8,8,8,224)
    appendQuad(v, kSidebarR, kSetPanelTop, kSetPanelR, kSetPanelBot, 0.03f, 0.03f, 0.03f);

    // Header removed - wiVRn doesn't show a title in the content area.

    // sidebar tabs: blue buttons (wiVRn style)
    for (int i = 0; i < (int)m.size(); i++) {
        bool active = (i == cat);
        UiRect r = settingsTabRect(i);
        float xL = r.cx - r.w*0.5f, xR = r.cx + r.w*0.5f;
        float yT = r.cy + r.h*0.5f, yB = r.cy - r.h*0.5f;
        float c0,c1,c2;
        // Active: bright blue, hover: medium blue, inactive: dark blue
        if (active)           { c0=0.10f; c1=0.35f; c2=0.70f; }
        else if (tabHover==i) { c0=0.08f; c1=0.22f; c2=0.45f; }
        else                  { c0=0.04f; c1=0.10f; c2=0.22f; }
        appendQuad(v, xL, yT, xR, yB, c0, c1, c2);
        const float *tc = active ? kUiWhite : kUiTitle;
        // Shrink text to fit button width
        float tpx = 0.0045f;
        float textW = r.w - 0.02f;  // padding
        // measureTextTTF is internal to ui_kit; use a simple fit
        uiTextC(v, m[i].name, r.cx, r.cy + 3.0f*tpx, tpx, tc[0], tc[1], tc[2]);
    }

    // content: build builder-local, then translate + triangle-clip to the viewport
    static std::vector<float> c; c.clear();
    menuBuild(c, m[cat], content);
    for (size_t t = 0; t + 18 <= c.size(); t += 18) {
        float y0 = c[t+1]+offY, y1 = c[t+7]+offY, y2 = c[t+13]+offY;
        if ((y0>kCtTop && y1>kCtTop && y2>kCtTop) || (y0<kCtBot && y1<kCtBot && y2<kCtBot)) continue;
        for (int k = 0; k < 3; k++) {
            size_t b = t + k*6;
            float yy = c[b+1]+offY;
            if (yy>kCtTop) yy=kCtTop; else if (yy<kCtBot) yy=kCtBot;
            v.push_back(c[b+0]+offX); v.push_back(yy); v.push_back(c[b+2]);
            v.push_back(c[b+3]); v.push_back(c[b+4]); v.push_back(c[b+5]);
        }
    }

    // reference scrollbar
    if (contentH > kSetViewportH + 1e-4f) {
        appendQuad(v, kSbX, kCtTop, kSbX + kSbW, kCtBot, 0.08f, 0.08f, 0.08f);
        float maxScroll = contentH - kSetViewportH;
        float thumbH = kSetViewportH * (kSetViewportH / contentH); if (thumbH < 0.04f) thumbH = 0.04f;
        float frac = (maxScroll > 1e-5f) ? (gSettingsScroll[cat] / maxScroll) : 0.0f;
        float thumbTop = kCtTop - frac * (kSetViewportH - thumbH);
        appendQuad(v, kSbX, thumbTop, kSbX + kSbW, thumbTop - thumbH, kUiFill[0], kUiFill[1], kUiFill[2]);
    }
}
