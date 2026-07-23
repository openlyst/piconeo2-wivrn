#include "imgui_ui.h"
#include "third_party/imgui/imgui.h"
#include "third_party/imgui/imgui_internal.h"
#include "app_state.h"
#include "settings_panel.h"
#include "server_list.h"
#include "eye_tracking.h"
#include "passthrough.h"
#include "streaming/streaming_client.h"
#include "render_thread.h"
#include "log.h"
#include <jni.h>
#include <cstdio>
#include <cstring>
#include <cmath>

// Tab indices: 0=Servers, 1=Settings, 2=Stats, 3=Apps, 4=Launch,
//              5=About, 6=Licenses, 7=Exit
static int sCurrentTab = 0;
static bool sStreamingMode = false;

// ---- exact palette from the main branch ui_kit.h ----
static const ImVec4 kColBg       (0.13f, 0.13f, 0.13f, 1.0f);
static const ImVec4 kColBgHot    (0.26f, 0.34f, 0.46f, 1.0f);
static const ImVec4 kColTrack    (0.08f, 0.08f, 0.08f, 1.0f);
static const ImVec4 kColFill     (0.24f, 0.52f, 0.88f, 1.0f);
static const ImVec4 kColOn       (0.24f, 0.52f, 0.88f, 1.0f);
static const ImVec4 kColOff      (0.18f, 0.18f, 0.18f, 1.0f);
static const ImVec4 kColWhite    (1.0f,  1.0f,  1.0f,  1.0f);
static const ImVec4 kColTitle    (0.90f, 0.90f, 0.92f, 1.0f);
static const ImVec4 kColDim      (0.50f, 0.52f, 0.56f, 1.0f);
static const ImVec4 kColHost     (0.50f, 0.55f, 0.60f, 1.0f);
static const ImVec4 kColSidebar  (0.0f,  0.0f,  0.0f,  1.0f);
static const ImVec4 kColContent  (0.03f, 0.03f, 0.03f, 1.0f);
// tab button colors
static const ImVec4 kColTabActive (0.10f, 0.35f, 0.70f, 1.0f);
static const ImVec4 kColTabHover  (0.08f, 0.22f, 0.45f, 1.0f);
static const ImVec4 kColTabIdle   (0.04f, 0.10f, 0.22f, 1.0f);
// server row card
static const ImVec4 kColRowNormal (0.08f, 0.09f, 0.12f, 1.0f);
static const ImVec4 kColRowHover  (0.14f, 0.18f, 0.24f, 1.0f);
// connect button (green)
static const ImVec4 kColConnect   (0.10f, 0.40f, 0.15f, 1.0f);
static const ImVec4 kColConnectH  (0.20f, 0.80f, 0.30f, 1.0f);
// X / stop button (red)
static const ImVec4 kColXBtn      (0.20f, 0.10f, 0.10f, 1.0f);
static const ImVec4 kColXBtnH     (0.50f, 0.20f, 0.20f, 1.0f);
// refresh button
static const ImVec4 kColRefresh   (0.10f, 0.15f, 0.20f, 1.0f);
static const ImVec4 kColRefreshH  (0.15f, 0.25f, 0.35f, 1.0f);
static const ImVec4 kColRefreshTxt(0.70f, 0.80f, 1.0f,  1.0f);
// error banner
static const ImVec4 kColErrBg     (0.25f, 0.08f, 0.08f, 1.0f);
static const ImVec4 kColErrTxt    (1.0f,  0.70f, 0.70f, 1.0f);
// discovered badge
static const ImVec4 kColBadgeDot  (0.20f, 0.70f, 0.30f, 1.0f);
static const ImVec4 kColBadgeTxt  (0.30f, 0.70f, 0.40f, 1.0f);

void applyImGuiTheme()
{
    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowRounding = 0.0f;
    s.ChildRounding = 4.0f;
    s.FrameRounding = 4.0f;
    s.GrabRounding = 3.0f;
    s.PopupRounding = 4.0f;
    s.ScrollbarRounding = 4.0f;
    s.TabRounding = 4.0f;
    s.WindowBorderSize = 0.0f;
    s.FrameBorderSize = 0.0f;
    s.ChildBorderSize = 0.0f;
    s.PopupBorderSize = 0.0f;
    s.WindowPadding = ImVec2(20, 16);
    s.FramePadding = ImVec2(12, 8);
    s.ItemSpacing = ImVec2(12, 10);
    s.ItemInnerSpacing = ImVec2(8, 6);
    s.ScrollbarSize = 10.0f;
    s.GrabMinSize = 12.0f;

    ImVec4 *c = s.Colors;
    // window / backgrounds
    c[ImGuiCol_WindowBg]        = kColContent;
    c[ImGuiCol_ChildBg]         = ImVec4(0,0,0,0);
    c[ImGuiCol_PopupBg]         = ImVec4(0.08f, 0.08f, 0.08f, 0.98f);
    c[ImGuiCol_Border]          = ImVec4(0,0,0,0);
    c[ImGuiCol_BorderShadow]    = ImVec4(0,0,0,0);
    // text
    c[ImGuiCol_Text]            = kColTitle;
    c[ImGuiCol_TextDisabled]    = kColDim;
    c[ImGuiCol_TextSelectedBg]  = kColFill;
    // frames (sliders, checkboxes)
    c[ImGuiCol_FrameBg]         = kColTrack;
    c[ImGuiCol_FrameBgHovered]  = ImVec4(0.12f, 0.12f, 0.14f, 1.0f);
    c[ImGuiCol_FrameBgActive]   = ImVec4(0.16f, 0.16f, 0.18f, 1.0f);
    // buttons
    c[ImGuiCol_Button]          = kColBg;
    c[ImGuiCol_ButtonHovered]   = kColBgHot;
    c[ImGuiCol_ButtonActive]    = kColFill;
    // checkmark / slider grab
    c[ImGuiCol_CheckMark]       = kColFill;
    c[ImGuiCol_SliderGrab]      = kColFill;
    c[ImGuiCol_SliderGrabActive]= ImVec4(0.40f, 0.65f, 1.0f, 1.0f);
    // header (combo, selectable)
    c[ImGuiCol_Header]          = kColBgHot;
    c[ImGuiCol_HeaderHovered]   = kColFill;
    c[ImGuiCol_HeaderActive]    = kColFill;
    // separator
    c[ImGuiCol_Separator]       = ImVec4(0.15f, 0.15f, 0.15f, 1.0f);
    c[ImGuiCol_SeparatorHovered]= kColFill;
    // scrollbar
    c[ImGuiCol_ScrollbarBg]     = ImVec4(0,0,0,0);
    c[ImGuiCol_ScrollbarGrab]   = ImVec4(0.15f, 0.15f, 0.15f, 1.0f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.22f, 0.22f, 0.22f, 1.0f);
    c[ImGuiCol_ScrollbarGrabActive]  = kColFill;
}

// ---- helpers ----

// Draw a card-style background rect behind the next items until the cursor
// moves past cardH.  Returns the Y position to restore after.
static float beginCard(float cardH, const ImVec4 &bg = kColRowNormal)
{
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 size(ImGui::GetContentRegionAvail().x, cardH);
    ImGui::GetWindowDrawList()->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                                               ImGui::ColorConvertFloat4ToU32(bg), 4.0f);
    ImGui::Dummy(ImVec2(8, 0));
    ImGui::SameLine();
    return pos.y;
}

// Section header text (title coloured, with separator)
static void sectionHeader(const char *text)
{
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::TextColored(kColTitle, "%s", text);
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 6));
}

// A toggle that looks like the pill switch from ui_kit (not ImGui's checkbox)
static bool pillToggle(const char *label, bool *val)
{
    ImGui::TextColored(kColTitle, "%s", label);
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 50);

    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = 44, h = 22;
    ImDrawList *dl = ImGui::GetWindowDrawList();

    bool hovered = ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + w, pos.y + h));
    bool clicked = hovered && ImGui::IsMouseClicked(0);

    ImVec4 trackCol = *val ? kColOn : kColOff;
    if (hovered && !*val) trackCol = ImVec4(0.24f, 0.24f, 0.26f, 1.0f);
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h),
                      ImGui::ColorConvertFloat4ToU32(trackCol), h * 0.5f);

    float knobX = *val ? pos.x + w - h * 0.5f : pos.x + h * 0.5f;
    dl->AddCircleFilled(ImVec2(knobX, pos.y + h * 0.5f), h * 0.42f,
                        ImGui::ColorConvertFloat4ToU32(kColWhite), 16);

    ImGui::Dummy(ImVec2(w, h));
    if (clicked) *val = !*val;
    return clicked;
}

// A stepper with - / + buttons and a value display
static void stepper(const char *label, float *val, float vmin, float vmax, float vstep,
                    const char *fmt)
{
    ImGui::TextColored(kColTitle, "%s", label);
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 180);

    char buf[32]; snprintf(buf, sizeof(buf), fmt, *val);
    float txtW = ImGui::CalcTextSize(buf).x;
    ImGui::TextColored(kColWhite, "%s", buf);
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(8, 0));
    ImGui::SameLine();

    ImGui::PushID(label);
    if (ImGui::Button("-", ImVec2(36, 0))) {
        *val = fmaxf(vmin, *val - vstep);
    }
    ImGui::SameLine(0, 4);
    if (ImGui::Button("+", ImVec2(36, 0))) {
        *val = fminf(vmax, *val + vstep);
    }
    ImGui::PopID();
}

// ---- Servers tab ----
static void buildServersTab()
{
    sectionHeader("Servers");

    auto err = getConnectionError();
    if (!err.empty()) {
        ImVec2 pos = ImGui::GetCursorScreenPos();
        float localY = ImGui::GetCursorPosY();
        float w = ImGui::GetContentRegionAvail().x;
        float h = 36;
        ImGui::GetWindowDrawList()->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h),
                ImGui::ColorConvertFloat4ToU32(kColErrBg), 4.0f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 10);
        ImGui::SetCursorPosY(localY + 8);
        ImGui::TextColored(kColErrTxt, "%s", err.c_str());
        ImGui::SetCursorPosY(localY + h + 8);
    }

    auto servers = getServerList();
    if (servers.empty()) {
        ImGui::Dummy(ImVec2(0, 30));
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImVec4(0.5f, 0.5f, 0.55f, 1.0f));
        ImGui::TextDisabled("Start a WiVRn server on your local network");
        ImGui::PopStyleColor();
    }

    for (int i = 0; i < (int)servers.size(); i++) {
        const auto &srv = servers[i];
        ImGui::PushID(i);

        float rowH = 100;
        ImVec2 pos = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        bool hovered = ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + w, pos.y + rowH));
        ImVec4 bg = hovered ? kColRowHover : kColRowNormal;
        ImGui::GetWindowDrawList()->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + rowH),
                ImGui::ColorConvertFloat4ToU32(bg), 4.0f);

        // Text area width (leave room for buttons on the right)
        float btnAreaW = 250;
        float textW = w - btnAreaW - 24;

        // Line 1: server name + discovered badge
        float line1Y = pos.y + 8;
        float line2Y = line1Y + ImGui::GetTextLineHeight() + 4;
        float textX = pos.x + 12;

        ImGui::SetCursorScreenPos(ImVec2(textX, line1Y));
        ImVec2 nameSize = ImGui::CalcTextSize(srv.name.c_str());
        if (nameSize.x > textW) {
            char trunc[128];
            snprintf(trunc, sizeof(trunc), "%.*s...", (int)(sizeof(trunc)-4), srv.name.c_str());
            ImGui::TextColored(kColTitle, "%s", trunc);
        } else {
            ImGui::TextColored(kColTitle, "%s", srv.name.c_str());
        }

        // Line 2: hostname
        ImGui::SetCursorScreenPos(ImVec2(textX, line2Y));
        char hp[160]; snprintf(hp, sizeof(hp), "%s:%d", srv.hostname.c_str(), srv.port);
        ImVec2 hostSize = ImGui::CalcTextSize(hp);
        if (hostSize.x > textW) {
            char trunc[160];
            snprintf(trunc, sizeof(trunc), "%.*s...", (int)(sizeof(trunc)-4), hp);
            ImGui::TextColored(kColHost, "%s", trunc);
        } else {
            ImGui::TextColored(kColHost, "%s", hp);
        }

        // Right side: buttons positioned with screen coords via SetCursorScreenPos
        float btnH = 44;
        float btnY = pos.y + (rowH - btnH) * 0.5f;
        float rightEdge = pos.x + w - 12;

        // X button (rightmost)
        ImGui::SetCursorScreenPos(ImVec2(rightEdge - 32, btnY));
        ImGui::PushStyleColor(ImGuiCol_Button, kColXBtn);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kColXBtnH);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kColXBtnH);
        if (ImGui::Button("X", ImVec2(32, btnH))) {
            if (gOnServerRemove) gOnServerRemove(srv.hostname, srv.port);
        }
        ImGui::PopStyleColor(3);

        // Connect button
        float connW = 160;
        ImGui::SetCursorScreenPos(ImVec2(rightEdge - 32 - 8 - connW, btnY));
        const char *btnLabel = isConnecting() ? "..." : "Connect";
        ImGui::PushStyleColor(ImGuiCol_Button, kColConnect);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kColConnectH);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kColConnectH);
        if (ImGui::Button(btnLabel, ImVec2(connW, btnH))) {
            clearConnectionError();
            if (gOnServerConnect) gOnServerConnect(srv);
        }
        ImGui::PopStyleColor(3);

        // Auto checkbox (left of Connect), vertically centered with buttons
        float autoRightEdge = rightEdge - 32 - 8 - connW - 12;
        float autoLeftEdge = autoRightEdge;
        if (!srv.manual) {
            bool autoConn = srv.autoconnect;
            float checkH = ImGui::GetFrameHeight();
            float checkY = pos.y + (rowH - checkH) * 0.5f;
            float autoW = ImGui::CalcTextSize("Auto").x + checkH + 12;
            autoLeftEdge = autoRightEdge - autoW;
            ImGui::SetCursorScreenPos(ImVec2(autoLeftEdge, checkY));
            if (ImGui::Checkbox("Auto", &autoConn)) {
                updateAutoconnect(srv.hostname, srv.port, autoConn);
                if (gOnServerAutoconnect)
                    gOnServerAutoconnect(srv.hostname, srv.port);
            }
        }

        // Discovered badge: left of the Auto checkbox
        if (srv.discovered) {
            ImVec2 discSize = ImGui::CalcTextSize("Discovered");
            float discY = pos.y + (rowH - discSize.y) * 0.5f;
            ImGui::SetCursorScreenPos(ImVec2(autoLeftEdge - discSize.x - 12, discY));
            ImGui::TextColored(kColBadgeTxt, "Discovered");
        }

        // Advance cursor past this row
        ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + rowH + 6));
        ImGui::Dummy(ImVec2(0, 0));
        ImGui::PopID();
    }

    ImGui::Dummy(ImVec2(0, 12));
    float refreshW = 140;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - refreshW) * 0.5f);
    ImGui::PushStyleColor(ImGuiCol_Button, kColRefresh);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kColRefreshH);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kColRefreshH);
    ImGui::PushStyleColor(ImGuiCol_Text, kColRefreshTxt);
    if (ImGui::Button("Refresh", ImVec2(refreshW, 36))) {
        if (gOnRefreshServers) gOnRefreshServers();
    }
    ImGui::PopStyleColor(4);
    ImGui::Dummy(ImVec2(0, 8));
}

// ---- Settings tab ----
static void buildSettingsTab()
{
    sectionHeader("Settings");

    // Software IPD
    float ipd = gSoftIpdMm.load();
    stepper("Software IPD", &ipd, kIpdMin, kIpdMax, kIpdStep, "%.1f mm");
    if (ipd != gSoftIpdMm.load()) {
        gSoftIpdMm.store(ipd);
        gIpdChangeNs.store(nowNs());
        gIpdDirty.store(true);
    }
    ImGui::Dummy(ImVec2(0, 6));

    // Brightness
    float bright = gBrightnessFrac.load();
    int brightPct = (int)(bright * 100.0f + 0.5f);
    if (ImGui::SliderInt("Brightness", &brightPct, 0, 100, "%d%%")) {
        bright = brightPct / 100.0f;
        gBrightnessFrac.store(bright);
        gBrightnessSaved.store(true);
        gBrightnessApply.store(true);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) saveBrightness();
    ImGui::Dummy(ImVec2(0, 4));

    // Field of view
    float fov = gStreamFovDeg.load();
    if (ImGui::SliderFloat("Field of view", &fov, kFovMin, kFovMax, "%.0f deg")) {
        gStreamFovDeg.store(roundf(fov));
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) gFovDirty.store(true);
    ImGui::Dummy(ImVec2(0, 4));

    // Resolution scale
    float res = gWivrnResolutionScale.load();
    int rw = (int)((1664 * res) / 2) * 2;
    int rh = (int)((1756 * res) / 2) * 2;
    char resFmt[32];
    snprintf(resFmt, sizeof(resFmt), "%%.2f (%dx%d)", rw, rh);
    if (ImGui::SliderFloat("Resolution scale", &res, 0.5f, 2.0f, resFmt)) {
        gWivrnResolutionScale.store(res);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        if (g_stream && g_stream->session) {
            constexpr int bw = 1664, bh = 1756;
            float s = gWivrnResolutionScale.load();
            int nrw = (int)(bw * s) / 2 * 2;
            int nrh = (int)(bh * s) / 2 * 2;
            g_stream->eye_width.store(nrw);
            g_stream->eye_height.store(nrh);
            g_stream->stream_eye_width.store(nrw);
            g_stream->stream_eye_height.store(nrh);
            g_stream->blit_pipeline.set_resolution(nrw, nrh);
            g_stream->resolution_dirty.store(true);
            g_stream->send_headset_info();
        }
        saveAllConfig();
    }
    ImGui::Dummy(ImVec2(0, 4));

    // Bitrate
    float bitrate = gWivrnBitrateMbps.load();
    if (ImGui::SliderFloat("Bitrate", &bitrate, 5.0f, 200.0f, "%.0f Mbps")) {
        gWivrnBitrateMbps.store(bitrate);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        int mbps = (int)gWivrnBitrateMbps.load();
        if (g_stream) {
            g_stream->bitrate_mbps.store(mbps);
            g_stream->send_bitrate_change(mbps);
        }
        saveAllConfig();
    }
    ImGui::Dummy(ImVec2(0, 4));

    // Eye-tracked foveation
    bool eyeFov = gWivrnEyeFoveation.load();
    if (!gEyeSupported.load()) ImGui::BeginDisabled();
    if (pillToggle("Eye-tracked foveation", &eyeFov)) {
        gWivrnEyeFoveation.store(eyeFov);
        saveAllConfig();
        gEyeFoveationDirty.store(true);
    }
    if (!gEyeSupported.load()) ImGui::EndDisabled();
    ImGui::Dummy(ImVec2(0, 4));

    // Microphone
    bool mic = gWivrnMicrophone.load();
    if (pillToggle("Microphone", &mic)) {
        gWivrnMicrophone.store(mic);
        if (g_stream) {
            g_stream->microphone_enabled.store(mic);
            if (g_stream->audio_handle)
                g_stream->audio_handle->set_mic_state(mic);
            g_stream->send_headset_info();
        }
        saveAllConfig();
    }
    ImGui::Dummy(ImVec2(0, 4));

    // Controller vibration
    float vib = gWivrnCtrlVibration.load();
    int vibPct = (int)(vib * 100.0f + 0.5f);
    if (ImGui::SliderInt("Controller vibration", &vibPct, 0, 100, "%d%%")) {
        gWivrnCtrlVibration.store(vibPct / 100.0f);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) saveAllConfig();
    ImGui::Dummy(ImVec2(0, 4));

    // Passthrough
    bool pt = gWivrnPassthrough.load();
    if (pillToggle("Passthrough", &pt)) {
        gWivrnPassthrough.store(pt);
        extern pico_passthrough *gPassthrough;
        if (pt) { if (gPassthrough && !gPassthrough->is_camera_on()) gPassthrough->start(); }
        else    { if (gPassthrough && gPassthrough->is_camera_on()) gPassthrough->stop(); }
        saveAllConfig();
    }
    ImGui::Dummy(ImVec2(0, 4));

    // Recenter
    if (ImGui::Button("Recenter", ImVec2(180, 0)))
        gWivrnRecenterReq.store(true);
    ImGui::Dummy(ImVec2(0, 4));

    // Eye debug
    bool eyeDbg = gEyeDebugOn.load();
    if (pillToggle("Eye debug", &eyeDbg)) {
        gEyeDebugOn.store(eyeDbg);
        saveEyeDebug();
        gEyeTrackReapply.store(true);
    }
    ImGui::Dummy(ImVec2(0, 4));

    // Diagnostics HUD
    const char *diagOpts[] = {"Off", "Pipeline", "System"};
    int diag = gDiagHudMode.load();
    if (ImGui::Combo("Diagnostics HUD", &diag, diagOpts, 3)) {
        gDiagHudMode.store(diag);
        saveDiagHud();
    }
}

// ---- Stats tab ----
static void buildStatsTab()
{
    sectionHeader("Performance");

    if (!g_stream) { ImGui::TextDisabled("Not streaming"); return; }

    int fps = g_stream->stats_fps;
    float lat = g_stream->stats_total_latency_ms;
    ImGui::TextColored(kColDim, "Framerate"); ImGui::SameLine(160);
    ImGui::TextColored(kColWhite, "%d fps", fps);
    ImGui::TextColored(kColDim, "Total latency"); ImGui::SameLine(160);
    ImGui::TextColored(kColWhite, "%.0f ms", lat);
    ImGui::Dummy(ImVec2(0, 6));

    ImGui::TextColored(kColDim, "Download"); ImGui::SameLine(160);
    ImGui::TextColored(kColWhite, "%.1f Mbit/s", g_stream->stats_bandwidth_rx * 1e-6f);
    ImGui::TextColored(kColDim, "Upload"); ImGui::SameLine(160);
    ImGui::TextColored(kColWhite, "%.1f Mbit/s", g_stream->stats_bandwidth_tx * 1e-6f);
    ImGui::Dummy(ImVec2(0, 6));

    ImGui::TextColored(kColDim, "CPU time"); ImGui::SameLine(160);
    ImGui::TextColored(kColWhite, "%.1f ms", g_stream->stats_cpu_time_ms);
    ImGui::TextColored(kColDim, "GPU time"); ImGui::SameLine(160);
    ImGui::TextColored(kColWhite, "%.1f ms", g_stream->stats_gpu_time_ms);
    ImGui::Dummy(ImVec2(0, 8));

    sectionHeader("Latency breakdown");
    struct { const char *name; float ms; ImVec4 col; } bars[] = {
        {"Encode",  g_stream->stats_encode_ms,      ImVec4(0.9f, 0.6f, 0.3f, 1.0f)},
        {"Send",    g_stream->stats_send_ms,        ImVec4(0.8f, 0.7f, 0.4f, 1.0f)},
        {"Network", g_stream->stats_network_ms,     ImVec4(0.4f, 0.7f, 0.9f, 1.0f)},
        {"Decode",  g_stream->stats_decode_ms,      ImVec4(0.5f, 0.8f, 0.5f, 1.0f)},
        {"Render",  g_stream->stats_render_wait_ms, ImVec4(0.7f, 0.5f, 0.8f, 1.0f)},
        {"Blit",    g_stream->stats_blit_ms,        ImVec4(0.6f, 0.6f, 0.7f, 1.0f)},
    };
    float maxMs = 1.0f;
    for (auto &b : bars) if (b.ms > maxMs) maxMs = b.ms;
    for (auto &b : bars) {
        ImGui::TextColored(kColDim, "%-10s", b.name);
        ImGui::SameLine(100);
        float frac = b.ms / maxMs;
        ImVec2 barPos = ImGui::GetCursorScreenPos();
        float barW = 200, barH = 16;
        ImGui::GetWindowDrawList()->AddRectFilled(barPos,
                ImVec2(barPos.x + barW, barPos.y + barH),
                ImGui::ColorConvertFloat4ToU32(kColTrack), 3.0f);
        ImGui::GetWindowDrawList()->AddRectFilled(barPos,
                ImVec2(barPos.x + barW * frac, barPos.y + barH),
                ImGui::ColorConvertFloat4ToU32(b.col), 3.0f);
        ImGui::Dummy(ImVec2(barW, barH));
        ImGui::SameLine();
        ImGui::TextColored(kColWhite, "%.1f ms", b.ms);
    }
    ImGui::Dummy(ImVec2(0, 8));

    sectionHeader("Stream info");
    ImGui::TextColored(kColDim, "Bitrate"); ImGui::SameLine(160);
    ImGui::TextColored(kColWhite, "%d Mbit/s", g_stream->current_bitrate_mbps.load());
    ImGui::TextColored(kColDim, "Resolution"); ImGui::SameLine(160);
    ImGui::TextColored(kColWhite, "%dx%d", g_stream->stream_eye_width.load(), g_stream->stream_eye_height.load());
    ImGui::TextColored(kColDim, "Microphone"); ImGui::SameLine(160);
    ImGui::TextColored(kColWhite, "%s", g_stream->microphone_enabled.load() ? "On" : "Off");
}

// ---- Apps tab ----
static void buildAppsTab()
{
    sectionHeader("Running apps");

    if (!g_stream) { ImGui::TextDisabled("Not streaming"); return; }

    std::vector<streaming_client::RunningApp> apps;
    { std::lock_guard<std::mutex> lk(g_stream->app_mutex); apps = g_stream->running_apps; }

    if (apps.empty()) { ImGui::TextDisabled("No apps running"); return; }

    // Poll running apps list once per second
    if (g_stream->session) {
        static uint64_t lastAppPoll = 0;
        uint64_t now = nowNs();
        if (now - lastAppPoll > 1000000000ULL) {
            lastAppPoll = now;
            try { g_stream->session->send_control(
                wivrn::from_headset::get_running_applications{}); }
            catch (...) {}
        }
    }

    for (int i = 0; i < (int)apps.size(); i++) {
        ImGui::PushID(i);
        float rowH = 48;
        ImVec2 pos = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        bool hovered = ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + w, pos.y + rowH));
        ImGui::GetWindowDrawList()->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + rowH),
                ImGui::ColorConvertFloat4ToU32(hovered ? kColRowHover : kColRowNormal), 4.0f);

        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 12);
        ImGui::SetCursorPosY(pos.y + rowH * 0.5f - 8);

        if (apps[i].active)
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f), "> ");
        else
            ImGui::Text("  ");
        ImGui::SameLine();
        ImGui::TextColored(kColWhite, "%s", apps[i].name.c_str());
        if (apps[i].overlay) {
            ImGui::SameLine();
            ImGui::TextColored(kColDim, "(overlay)");
        }

        // stop button
        ImGui::SameLine(w - 70);
        ImGui::PushStyleColor(ImGuiCol_Button, kColXBtn);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kColXBtnH);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kColXBtnH);
        if (ImGui::Button("Stop", ImVec2(58, 0))) {
            if (g_stream->session) {
                try { g_stream->session->send_control(
                    wivrn::from_headset::stop_application{apps[i].id}); }
                catch (std::exception &e) { LOGI("stop_application: %s", e.what()); }
            }
        }
        ImGui::PopStyleColor(3);

        ImGui::SetCursorPosY(pos.y + rowH + 4);
        ImGui::PopID();
    }
}

// ---- Launch tab ----
static void buildLaunchTab()
{
    sectionHeader("Launch application");

    if (!g_stream) { ImGui::TextDisabled("Not streaming"); return; }

    std::vector<streaming_client::AppEntry> apps;
    bool requested;
    std::string launching_id;
    { std::lock_guard<std::mutex> lk(g_stream->app_mutex);
      apps = g_stream->available_apps;
      requested = g_stream->app_list_requested;
      launching_id = g_stream->launching_app_id; }

    // Auto-request app list when entering the tab with an empty list
    if (apps.empty() && !requested && g_stream->session) {
        try { g_stream->session->send_control(
            wivrn::from_headset::get_application_list{"en","US",""}); }
        catch (...) {}
    }

    if (apps.empty()) {
        ImGui::TextDisabled("%s", requested ? "Loading..." : "No apps available");
        if (ImGui::Button("Refresh")) {
            if (g_stream->session) {
                try { g_stream->session->send_control(
                    wivrn::from_headset::get_application_list{"en","US",""}); }
                catch (std::exception &e) { LOGI("app list: %s", e.what()); }
            }
        }
        return;
    }

    for (int i = 0; i < (int)apps.size(); i++) {
        ImGui::PushID(i);
        float rowH = 48;
        ImVec2 pos = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        bool hovered = ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + w, pos.y + rowH));
        ImGui::GetWindowDrawList()->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + rowH),
                ImGui::ColorConvertFloat4ToU32(hovered ? kColRowHover : kColRowNormal), 4.0f);

        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 12);
        ImGui::SetCursorPosY(pos.y + rowH * 0.5f - 8);
        ImGui::TextColored(kColWhite, "%s", apps[i].name.c_str());

        ImGui::SameLine(w - 140);
        bool launching = (!launching_id.empty() && apps[i].id == launching_id);
        if (launching) {
            ImGui::TextColored(ImVec4(0.6f, 0.55f, 0.2f, 1.0f), "Launching...");
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, kColConnect);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kColConnectH);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, kColConnectH);
            if (ImGui::Button("Launch", ImVec2(110, 0))) {
                if (g_stream->session) {
                    try { g_stream->session->send_control(
                        wivrn::from_headset::start_app{apps[i].id});
                        g_stream->launching_app_id = apps[i].id; }
                    catch (std::exception &e) { LOGI("start_app: %s", e.what()); }
                }
            }
            ImGui::PopStyleColor(3);
        }

        ImGui::SetCursorPosY(pos.y + rowH + 4);
        ImGui::PopID();
    }
}

// ---- clickable link helper ----
static void openUrlOnDevice(const char *url)
{
    if (!gVM || !gActivity) return;
    JNIEnv *env = nullptr;
    bool attached = false;
    if (gVM->GetEnv((void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if (gVM->AttachCurrentThread(&env, nullptr) == JNI_OK) attached = true;
    }
    if (!env) return;
    jstring jurl = env->NewStringUTF(url);
    jclass cls = env->GetObjectClass(gActivity);
    jmethodID mid = env->GetMethodID(cls, "openUrl", "(Ljava/lang/String;)V");
    if (mid) env->CallVoidMethod(gActivity, mid, jurl);
    env->DeleteLocalRef(jurl);
    env->DeleteLocalRef(cls);
    if (attached) gVM->DetachCurrentThread();
}

static void linkLabel(const char *label, const char *url)
{
    ImVec4 linkCol(0.35f, 0.65f, 1.0f, 1.0f);
    ImVec4 hoverCol(0.5f, 0.8f, 1.0f, 1.0f);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::CalcTextSize(label);
    bool hovered = ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + size.x, pos.y + size.y));
    ImGui::TextColored(hovered ? hoverCol : linkCol, "%s", label);
    // underline
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(pos.x, pos.y + size.y),
        ImVec2(pos.x + size.x, pos.y + size.y),
        ImGui::ColorConvertFloat4ToU32(hovered ? hoverCol : linkCol));
    if (hovered && ImGui::IsMouseClicked(0))
        openUrlOnDevice(url);
    ImGui::SetCursorPosY(pos.y + size.y + 2);
    ImGui::Dummy(ImVec2(0, 0));
}

// ---- About tab ----
static void buildAboutTab()
{
    // Banner
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + 50),
            ImGui::ColorConvertFloat4ToU32(kColFill), 4.0f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 16);
    ImGui::SetCursorPosY(pos.y + 14);
    ImGui::TextColored(kColWhite, "WiVRn for Pico Neo 2");
    ImGui::SetCursorPosY(pos.y + 58);

    ImGui::Dummy(ImVec2(0, 4));
    ImGui::TextColored(kColTitle, "Stream PC VR games to your Pico Neo 2");
    ImGui::TextColored(kColTitle, "over Wi-Fi or USB with low latency");
    ImGui::Dummy(ImVec2(0, 12));

    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 6));

    ImGui::TextColored(kColFill, "Upstream");
    linkLabel("github.com/wivrn/wivrn", "https://github.com/wivrn/wivrn");
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::TextColored(kColFill, "This client");
    linkLabel("gitlab.com/httpanimations/piconeo2-wivrn", "https://gitlab.com/httpanimations/piconeo2-wivrn");
    ImGui::Dummy(ImVec2(0, 12));

    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::TextColored(kColTitle, "Licensed under AGPL v3");
    ImGui::TextColored(kColDim, "See Licenses tab for details");
}

// ---- Licenses tab ----
static void buildLicensesTab()
{
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + 50),
            ImGui::ColorConvertFloat4ToU32(kColFill), 4.0f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 16);
    ImGui::SetCursorPosY(pos.y + 14);
    ImGui::TextColored(kColWhite, "AGPL v3");
    ImGui::SetCursorPosY(pos.y + 58);

    ImGui::Dummy(ImVec2(0, 4));
    ImGui::TextColored(kColTitle, "This project is licensed under");
    ImGui::TextColored(kColTitle, "the GNU Affero General Public License v3");
    ImGui::Dummy(ImVec2(0, 12));

    ImGui::TextColored(kColFill, "Full license text");
    ImGui::Dummy(ImVec2(0, 4));
    linkLabel("gitlab.com/httpanimations/piconeo2-wivrn/-/raw/main/LICENSE",
              "https://gitlab.com/httpanimations/piconeo2-wivrn/-/raw/main/LICENSE");
    ImGui::Dummy(ImVec2(0, 12));

    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::TextColored(kColTitle, "Third-party components");
    ImGui::Dummy(ImVec2(0, 8));

    ImGui::TextColored(kColFill, "3D controller models - Pico Interactive");
    ImGui::TextColored(kColDim, "Owned by Pico Interactive");
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::TextColored(kColFill, "ALVR Pico Legacy - MIT License");
    linkLabel("github.com/Juspertinry/alvr-pico-legacy",
              "https://github.com/Juspertinry/alvr-pico-legacy");
}

// ---- Exit tab ----
static void buildExitTab()
{
    sectionHeader("Exit");
    ImGui::Dummy(ImVec2(0, 20));

    if (sStreamingMode) {
        if (ImGui::Button("Disconnect", ImVec2(250, 56))) {
            if (g_stream) {
                g_stream->shutdown = true;
                g_stream->auto_reconnect.store(false);
            }
        }
    } else {
        if (ImGui::Button("Quit WiVRn", ImVec2(250, 56))) {
            if (gOnExit) gOnExit();
        }
    }
}

// ---- tab button helper ----
static void tabButton(const char *name, int id, bool selected)
{
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    float h = 44;

    ImVec4 bg = selected ? kColTabActive : kColTabIdle;
    bool hovered = ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + w, pos.y + h));
    if (hovered && !selected) bg = kColTabHover;

    ImGui::GetWindowDrawList()->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h),
            ImGui::ColorConvertFloat4ToU32(bg), 4.0f);

    ImVec4 txtCol = selected ? kColWhite : kColTitle;
    ImVec2 txtSize = ImGui::CalcTextSize(name);
    float tx = pos.x + (w - txtSize.x) * 0.5f;
    float ty = pos.y + (h - txtSize.y) * 0.5f;
    ImGui::GetWindowDrawList()->AddText(ImVec2(tx, ty),
            ImGui::ColorConvertFloat4ToU32(txtCol), name);

    if (hovered && ImGui::IsMouseClicked(0)) sCurrentTab = id;
    ImGui::SetCursorPosY(pos.y + h + 6);
    ImGui::Dummy(ImVec2(0, 0));
}

// ---- Main entry point ----
void buildImGuiUI()
{
    applyImGuiTheme();
    sStreamingMode = gStreamingMode;
    if (sStreamingMode && sCurrentTab == 0) sCurrentTab = 1;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##lobby", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();

    float sidebarW = 220;

    // Sidebar: pure black background
    ImVec2 sbPos = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(sbPos,
            ImVec2(sbPos.x + sidebarW, sbPos.y + ImGui::GetContentRegionAvail().y),
            ImGui::ColorConvertFloat4ToU32(kColSidebar));

    ImGui::BeginChild("##sidebar", ImVec2(sidebarW, 0), false,
                      ImGuiWindowFlags_NoScrollbar);

    ImGui::Dummy(ImVec2(0, 8));

    // Top group
    struct TabDef { const char *name; int id; bool streamingOnly; bool hideWhileStreaming; };
    TabDef topTabs[] = {
        {"Servers",  0, false, true},
        {"Settings", 1, false, false},
        {"Stats",    2, true,  false},
        {"Apps",     3, true,  false},
        {"Launch",   4, true,  false},
    };
    for (auto &t : topTabs) {
        if (t.streamingOnly && !sStreamingMode) continue;
        if (t.hideWhileStreaming && sStreamingMode) continue;
        tabButton(t.name, t.id, sCurrentTab == t.id);
    }

    // Flexible spacer
    float remaining = ImGui::GetContentRegionAvail().y;
    int bottomCount = 3;
    float tabStep = 44 + 6 + ImGui::GetStyle().ItemSpacing.y;
    float bottomH = bottomCount * tabStep - ImGui::GetStyle().ItemSpacing.y;  // no trailing spacing
    if (remaining > bottomH)
        ImGui::Dummy(ImVec2(0, remaining - bottomH));

    // Bottom group
    TabDef bottomTabs[] = {
        {"About",    5, false},
        {"Licenses", 6, false},
        {"Exit",     7, false},
    };
    for (auto &t : bottomTabs)
        tabButton(t.name, t.id, sCurrentTab == t.id);

    ImGui::EndChild();

    ImGui::SameLine();

    // Content area
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 16));
    ImGui::BeginChild("##content", ImVec2(0, 0), false,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    ImGui::PopStyleVar();
    switch (sCurrentTab) {
        case 0: buildServersTab();   break;
        case 1: buildSettingsTab();  break;
        case 2: buildStatsTab();     break;
        case 3: buildAppsTab();      break;
        case 4: buildLaunchTab();    break;
        case 5: buildAboutTab();     break;
        case 6: buildLicensesTab();  break;
        case 7: buildExitTab();      break;
    }
    ImGui::EndChild();

    ImGui::End();
}
