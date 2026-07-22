#include "settings_panel.h"
#include "app_state.h"
#include "eq_panel.h"
#include "menu_model.h"
#include "passthrough.h"
#include "eye_tracking.h"   // gEyeSupported (disable toggle on non-EYE hw)
#include "server_list.h"    // wiVRn server list tab
#include "stream_panel.h"   // streaming-only categories (Stats, Apps, Launch)
#include "log.h"        // nowNs()
#include "streaming/streaming_client.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include "i18n.h"

// session state
bool gSettingsOpen = true;   // always open: unified lobby panel
int  gSettingsCat  = 0;
bool gStreamingMode = false;
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
    MenuCategory servers; servers.name = tr(Str::Servers); servers.custom = true; servers.hideWhileStreaming = true;
    {
        MenuItem srv; srv.kind = MK_CUSTOM;
        srv.customH = 0.8f;
        srv.cBuild = [](std::vector<float> &v, const MenuHover &h) {
            int hoverItem = (h.item >= 0) ? h.item : -1;
            int connectHot = (h.item >= 0 && h.part == 1) ? h.item : -1;
            if (h.item >= 0 && h.part == 3) connectHot = -2;  // X button hover
            if (h.part == 4) connectHot = -3;                 // Refresh button hover
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
    MenuCategory settings; settings.name = tr(Str::Settings);
    {
        MenuItem ipd; ipd.kind = MK_STEPPER; ipd.label = tr(Str::SoftwareIPD);
        ipd.vmin = kIpdMin; ipd.vmax = kIpdMax; ipd.vstep = kIpdStep;
        ipd.get = []{ return gSoftIpdMm.load(); };
        ipd.set = [](float v){ gSoftIpdMm.store(v); gIpdChangeNs.store(nowNs()); gIpdDirty.store(true); };
        ipd.valueText = [](char *b,int n){ snprintf(b,n,"%.1f mm", gSoftIpdMm.load()); };
        settings.items.push_back(ipd);

        MenuItem br; br.kind = MK_FADER; br.label = tr(Str::Brightness);
        br.vmin = 0.0f; br.vmax = 1.0f;
        br.get = []{ return gBrightnessFrac.load(); };
        br.set = [](float f){ gBrightnessFrac.store(f); gBrightnessSaved.store(true); gBrightnessApply.store(true); };
        br.valueText = [](char *b,int n){ snprintf(b,n,"%d%%",(int)(gBrightnessFrac.load()*100.0f+0.5f)); };
        br.onCommit = []{ saveBrightness(); };
        settings.items.push_back(br);

        MenuItem fov; fov.kind = MK_FADER; fov.label = tr(Str::FieldOfView);
        fov.vmin = kFovMin; fov.vmax = kFovMax;
        fov.get = []{ return gStreamFovDeg.load(); };
        fov.set = [](float v){ gStreamFovDeg.store(roundf(v)); };
        fov.valueText = [](char *b,int n){ snprintf(b,n,"%.0f deg", gStreamFovDeg.load()); };
        fov.onCommit = []{ gFovDirty.store(true); };
        settings.items.push_back(fov);

        MenuItem res; res.kind = MK_FADER; res.label = tr(Str::ResolutionScale);
        res.vmin = 0.5f; res.vmax = 2.0f;
        res.get = []{ return gWivrnResolutionScale.load(); };
        res.set = [](float v){ gWivrnResolutionScale.store(v); };
        res.valueText = [](char *b,int n){
            float s = gWivrnResolutionScale.load();
            int rw = (int)((1664 * s) / 2) * 2;
            int rh = (int)((1756 * s) / 2) * 2;
            snprintf(b,n,"%d%% - %dx%d", (int)(s * 100), rw, rh);
        };
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

        MenuItem bit; bit.kind = MK_FADER; bit.label = tr(Str::Bitrate);
        bit.vmin = 5.0f; bit.vmax = 200.0f;
        bit.get = []{ return gWivrnBitrateMbps.load(); };
        bit.set = [](float v){ gWivrnBitrateMbps.store(v); };
        bit.valueText = [](char *b,int n){ snprintf(b,n,"%.0f Mbps", gWivrnBitrateMbps.load()); };
        bit.onCommit = []{
            int mbps = (int)gWivrnBitrateMbps.load();
            if (g_stream) {
                g_stream->bitrate_mbps.store(mbps);
                g_stream->send_bitrate_change(mbps);
            }
            saveAllConfig();
        };
        settings.items.push_back(bit);

        MenuItem eyeFov = wivrnToggle(tr(Str::EyeTrackedFoveation), gWivrnEyeFoveation);
        eyeFov.disabled = !gEyeSupported.load();
        eyeFov.onChange = []{ saveAllConfig(); gEyeFoveationDirty.store(true); };
        settings.items.push_back(eyeFov);

        MenuItem mic = wivrnToggle(tr(Str::Microphone), gWivrnMicrophone);
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

        MenuItem vib; vib.kind = MK_FADER; vib.label = tr(Str::ControllerVibration);
        vib.vmin = 0.0f; vib.vmax = 1.0f;
        vib.get = []{ return gWivrnCtrlVibration.load(); };
        vib.set = [](float v){ gWivrnCtrlVibration.store(v); };
        vib.valueText = [](char *b,int n){ snprintf(b,n,"%d%%", (int)(gWivrnCtrlVibration.load()*100.0f+0.5f)); };
        vib.onCommit = []{ saveAllConfig(); };
        settings.items.push_back(vib);

        MenuItem pt = wivrnToggle(tr(Str::Passthrough), gWivrnPassthrough);
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

        MenuItem rec; rec.kind = MK_BUTTON; rec.label = tr(Str::Recenter);
        rec.onClick = []{ gWivrnRecenterReq.store(true); };
        settings.items.push_back(rec);

        MenuItem eye; eye.kind = MK_TOGGLE; eye.label = tr(Str::EyeDebug);
        eye.get = []{ return gEyeDebugOn.load()?1.0f:0.0f; };
        eye.set = [](float v){ gEyeDebugOn.store(v>0.5f); };
        eye.onChange = []{ saveEyeDebug(); gEyeTrackReapply.store(true); };
        settings.items.push_back(eye);

        MenuItem diag; diag.kind = MK_DROPDOWN; diag.label = tr(Str::DiagnosticsHUD);
        diag.options = { tr(Str::Off), tr(Str::Pipeline), tr(Str::System_) };
        diag.get = []{ return (float)gDiagHudMode.load(); };
        diag.set = [](float v){ gDiagHudMode.store((int)(v+0.5f)); };
        diag.onChange = []{ saveDiagHud(); };
        settings.items.push_back(diag);

        MenuItem lang; lang.kind = MK_DROPDOWN; lang.label = tr(Str::Language);
        lang.options = { tr(Str::English_), tr(Str::Chinese_), tr(Str::SystemLang) };
        lang.get = []{ return (float)gLangSetting.load(); };
        lang.set = [](float v){
            int lv = (int)(v + 0.5f);
            gLangSetting.store(lv);
            setLanguage((Lang)lv);
            saveLangSetting();
        };
        settings.items.push_back(lang);
    }
    m.push_back(settings);

    // STREAMING-ONLY categories: Stats, Apps, Launch (hidden unless streaming)
    buildStreamCategories(m);

    // ABOUT (bottom) --------------------------------------------------------
    MenuCategory about; about.name = tr(Str::About); about.custom = true;
    {
        MenuItem ab; ab.kind = MK_CUSTOM;
        ab.customH = 1.6f;
        ab.cBuild = [](std::vector<float> &v, const MenuHover &h) {
            (void)h;
            float y = kMenuTopY;
            float cx = 0.0f;
            float halfW = (kCtX1 - kCtX0) * 0.5f - 0.02f;

            // Header banner
            UiRect banner = { cx, y - 0.06f, halfW * 2.0f, 0.12f };
            uiBox(v, banner, kUiFill);
            uiTextC(v, tr(Str::AppName), cx, y - 0.09f, 0.007f, 1.0f, 1.0f, 1.0f);
            y -= 0.20f;

            // Divider
            appendQuad(v, -halfW, y, halfW, y - 0.005f,
                       kUiTrack[0], kUiTrack[1], kUiTrack[2]);
            y -= 0.04f;

            // Description
            const char *desc[] = {
                tr(Str::AboutDesc1),
                tr(Str::AboutDesc2),
                tr(Str::AboutDesc3),
            };
            for (const char *line : desc) {
                uiTextC(v, line, cx, y, 0.0045f,
                        kUiTitle[0], kUiTitle[1], kUiTitle[2]);
                y -= 0.06f;
            }
            y -= 0.03f;

            // Links section
            uiTextL(v, tr(Str::Upstream), -halfW + 0.02f, y, 0.004f,
                    kUiFill[0], kUiFill[1], kUiFill[2]);
            y -= 0.06f;
            uiTextL(v, "github.com/wivrn/wivrn", -halfW + 0.02f, y, 0.004f,
                    kUiTitle[0], kUiTitle[1], kUiTitle[2]);
            y -= 0.08f;

            uiTextL(v, tr(Str::ThisClient), -halfW + 0.02f, y, 0.004f,
                    kUiFill[0], kUiFill[1], kUiFill[2]);
            y -= 0.06f;
            uiTextL(v, "gitlab.com/httpanimations/", -halfW + 0.02f, y, 0.004f,
                    kUiTitle[0], kUiTitle[1], kUiTitle[2]);
            y -= 0.05f;
            uiTextL(v, "piconeo2-wivrn", -halfW + 0.02f, y, 0.004f,
                    kUiTitle[0], kUiTitle[1], kUiTitle[2]);
            y -= 0.08f;

            // License
            appendQuad(v, -halfW, y, halfW, y - 0.005f,
                       kUiTrack[0], kUiTrack[1], kUiTrack[2]);
            y -= 0.04f;
            uiTextC(v, tr(Str::LicensedAGPL), cx, y, 0.004f,
                    kUiTitle[0], kUiTitle[1], kUiTitle[2]);
            y -= 0.06f;
            uiTextC(v, tr(Str::SeeLicensesTab), cx, y, 0.0035f,
                    kUiTrack[0], kUiTrack[1], kUiTrack[2]);
        };
        about.items.push_back(ab);
    }
    m.push_back(about);

    // LICENSES (bottom) -----------------------------------------------------
    MenuCategory licenses; licenses.name = tr(Str::Licenses); licenses.custom = true;
    {
        MenuItem lic; lic.kind = MK_CUSTOM;
        lic.customH = 0.9f;
        lic.cBuild = [](std::vector<float> &v, const MenuHover &h) {
            (void)h;
            float y = kMenuTopY;
            float cx = 0.0f;
            float halfW = (kCtX1 - kCtX0) * 0.5f - 0.02f;

            UiRect banner = { cx, y - 0.06f, halfW * 2.0f, 0.12f };
            uiBox(v, banner, kUiFill);
            uiTextC(v, tr(Str::AGPLv3), cx, y - 0.09f, 0.007f, 1.0f, 1.0f, 1.0f);
            y -= 0.20f;

            uiTextC(v, tr(Str::LicenseText1), cx, y, 0.0045f,
                    kUiTitle[0], kUiTitle[1], kUiTitle[2]);
            y -= 0.06f;
            uiTextC(v, tr(Str::LicenseText2), cx, y, 0.0045f,
                    kUiTitle[0], kUiTitle[1], kUiTitle[2]);
            y -= 0.06f;
            uiTextC(v, tr(Str::LicenseText3), cx, y, 0.0045f,
                    kUiTitle[0], kUiTitle[1], kUiTitle[2]);
            y -= 0.10f;

            uiTextC(v, tr(Str::FullLicenseText), cx, y, 0.004f,
                    kUiFill[0], kUiFill[1], kUiFill[2]);
            y -= 0.07f;

            UiRect urlBtn = { cx, y - 0.035f, halfW * 2.0f, 0.07f };
            uiButton(v, urlBtn, "gitlab.com/httpanimations/", false, false);
            y -= 0.06f;
            uiTextC(v, "piconeo2-wivrn/-/raw/main/LICENSE", cx, y, 0.0035f,
                    kUiTitle[0], kUiTitle[1], kUiTitle[2]);
            y -= 0.12f;

            // Third-party notices
            uiTextC(v, tr(Str::ThirdPartyComponents), cx, y, 0.005f,
                    kUiTitle[0], kUiTitle[1], kUiTitle[2]);
            y -= 0.07f;

            uiTextC(v, tr(Str::ALVRLicense), cx, y, 0.0038f,
                    kUiFill[0], kUiFill[1], kUiFill[2]);
            y -= 0.05f;
            uiTextC(v, "github.com/alvr-org/alvr", cx, y, 0.0032f,
                    kUiTitle[0], kUiTitle[1], kUiTitle[2]);
            y -= 0.08f;

            uiTextC(v, tr(Str::ControllerModels), cx, y, 0.0038f,
                    kUiFill[0], kUiFill[1], kUiFill[2]);
            y -= 0.05f;
            uiTextC(v, tr(Str::OwnedByPico), cx, y, 0.0032f,
                    kUiTitle[0], kUiTitle[1], kUiTitle[2]);
        };
        licenses.items.push_back(lic);
    }
    m.push_back(licenses);

    // EXIT (bottom) ---------------------------------------------------------
    MenuCategory exit; exit.name = tr(Str::Exit);
    {
        MenuItem quit; quit.kind = MK_BUTTON; quit.label = tr(Str::QuitWiVRn);
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

// wiVRn-style sidebar: top group (SERVERS, SETTINGS, [STATS, APPS, LAUNCH])
// then gap, then bottom group (ABOUT, LICENSES, EXIT) anchored to bottom.
// Streaming-only tabs are skipped when gStreamingMode is false.
static bool tabVisible(int i) {
    if (i < 0 || i >= (int)settingsModel().size()) return false;
    if (settingsModel()[i].streamingOnly && !gStreamingMode) return false;
    if (settingsModel()[i].hideWhileStreaming && gStreamingMode) return false;
    return true;
}

// Map a visible-tab index to the actual model index (skipping hidden tabs).
static int visibleToModel(int visIdx) {
    int vis = 0;
    for (int i = 0; i < (int)settingsModel().size(); i++) {
        if (!tabVisible(i)) continue;
        if (vis == visIdx) return i;
        vis++;
    }
    return -1;
}

// Count visible tabs in the top group (non-streamingOnly before the bottom group).
static int countTopTabs() {
    int n = 0;
    for (int i = 0; i < (int)settingsModel().size(); i++) {
        if (settingsModel()[i].streamingOnly) break;
        if (i < 2) n++;  // Servers, Settings are always top
        else break;
    }
    // Add streaming tabs if visible
    if (gStreamingMode) {
        for (int i = 2; i < (int)settingsModel().size(); i++) {
            if (settingsModel()[i].streamingOnly) n++;
            else break;
        }
    }
    return n;
}

// Count visible tabs in the bottom group (About, Licenses, Exit).
static int countBottomTabs() {
    int n = 0;
    for (int i = (int)settingsModel().size() - 1; i >= 0; i--) {
        if (settingsModel()[i].streamingOnly) continue;
        n++;
        if (n >= 3) break;
    }
    return n;
}

UiRect settingsTabRect(int i) {
    if (!tabVisible(i)) return {0, 0, 0, 0};

    float tabH = 0.09f;
    float tabGap = 0.02f;
    float sidebarL = kSetPanelL + 0.02f;
    float sidebarR = kSidebarR - 0.02f;
    float tabW = sidebarR - sidebarL;
    float sidebarX = (sidebarL + sidebarR) * 0.5f;

    // Determine if this tab is in the top group or bottom group.
    // Top: Servers, Settings, [Stats, Apps, Launch]
    // Bottom: About, Licenses, Exit
    bool isBottom = false;
    int bottomCount = countBottomTabs();
    // The bottom group is the last `bottomCount` visible tabs.
    int visIdx = 0;
    for (int j = 0; j < i; j++) if (tabVisible(j)) visIdx++;
    int totalVisible = 0;
    for (int j = 0; j < (int)settingsModel().size(); j++) if (tabVisible(j)) totalVisible++;
    isBottom = (visIdx >= totalVisible - bottomCount);

    if (!isBottom) {
        // Top group: stack from the top
        int topIdx = 0;
        for (int j = 0; j < i; j++) if (tabVisible(j) && j < i) {
            // Check j is also top group
            int vj = 0;
            for (int k = 0; k < j; k++) if (tabVisible(k)) vj++;
            if (vj < totalVisible - bottomCount) topIdx++;
        }
        float yTop = kSetPanelTop - 0.02f;
        return { sidebarX, yTop - tabH*0.5f - topIdx*(tabH + tabGap), tabW, tabH };
    } else {
        // Bottom group: stack from the bottom
        int botIdx = 0;
        for (int j = i + 1; j < (int)settingsModel().size(); j++) {
            if (tabVisible(j)) {
                int vj = 0;
                for (int k = 0; k < j; k++) if (tabVisible(k)) vj++;
                if (vj >= totalVisible - bottomCount) botIdx++;
            }
        }
        float yBot = kSetPanelBot + 0.02f;
        return { sidebarX, yBot + tabH*0.5f + botIdx*(tabH + tabGap), tabW, tabH };
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
    // Custom categories have dynamic content (app lists, running apps) that
    // changes without the menu structure changing, so always re-measure them.
    bool cacheValid = !cat.custom && sExtCat == gSettingsCat && sExtSig == sig;
    float maxY;
    if (cacheValid) {
        maxY = sExtMaxY; contentH = sExtContentH;
    } else {
        static std::vector<float> tmp; tmp.clear();
        MenuHover none;
        menuBuild(tmp, cat, none);
        float minY = 1e9f, mxY = -1e9f;
        for (size_t i = 1; i < tmp.size(); i += 8) { float y = tmp[i]; if (y<minY) minY=y; if (y>mxY) mxY=y; }
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

    // Update the Exit tab: "Disconnect" while streaming, "Quit WiVRn" otherwise.
    for (auto &c : m) {
        if (c.name == std::string(tr(Str::Exit)) && !c.items.empty()) {
            if (gStreamingMode) {
                c.items[0].label = tr(Str::Disconnect);
                c.items[0].onClick = []{
                    if (g_stream) {
                        g_stream->shutdown = true;
                        g_stream->auto_reconnect.store(false);
                        LOGI("disconnect requested from streaming UI");
                    }
                };
            } else {
                c.items[0].label = tr(Str::QuitWiVRn);
                c.items[0].onClick = []{
                    extern std::function<void()> gOnExit;
                    if (gOnExit) gOnExit();
                };
            }
        }
    }
    // wiVRn-style: pure black sidebar, grey semi-transparent content area.
    // Sidebar: pure black (0,0,0)
    appendQuad(v, kSetPanelL, kSetPanelTop, kSidebarR, kSetPanelBot, 0.0f, 0.0f, 0.0f);
    // Content area: dark grey semi-transparent (wiVRn uses 8,8,8,224)
    appendQuad(v, kSidebarR, kSetPanelTop, kSetPanelR, kSetPanelBot, 0.03f, 0.03f, 0.03f);

    // Header removed - wiVRn doesn't show a title in the content area.

    // sidebar tabs: blue buttons (wiVRn style)
    for (int i = 0; i < (int)m.size(); i++) {
        if (m[i].streamingOnly && !gStreamingMode) continue;
        if (m[i].hideWhileStreaming && gStreamingMode) continue;
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
        float tpx = kUiText;
        float textW = r.w - 0.02f;  // padding
        // measureTextTTF is internal to ui_kit; use a simple fit
        uiTextC(v, m[i].name, r.cx, r.cy + baselineOffset(tpx), tpx, tc[0], tc[1], tc[2]);
    }

    // content: build builder-local, then translate + triangle-clip to the viewport
    static std::vector<float> c; c.clear();
    menuBuild(c, m[cat], content);
    for (size_t t = 0; t + 24 <= c.size(); t += 24) {
        float y0 = c[t+1]+offY, y1 = c[t+9]+offY, y2 = c[t+17]+offY;
        if ((y0>kCtTop && y1>kCtTop && y2>kCtTop) || (y0<kCtBot && y1<kCtBot && y2<kCtBot)) continue;
        for (int k = 0; k < 3; k++) {
            size_t b = t + k*8;
            float yy = c[b+1]+offY;
            if (yy>kCtTop) yy=kCtTop; else if (yy<kCtBot) yy=kCtBot;
            v.push_back(c[b+0]+offX); v.push_back(yy); v.push_back(c[b+2]);
            v.push_back(c[b+3]); v.push_back(c[b+4]);
            v.push_back(c[b+5]); v.push_back(c[b+6]); v.push_back(c[b+7]);
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
