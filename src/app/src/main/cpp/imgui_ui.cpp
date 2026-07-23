#include "imgui_ui.h"
#include "third_party/imgui/imgui.h"
#include "app_state.h"
#include "settings_panel.h"
#include "server_list.h"
#include "eye_tracking.h"
#include "passthrough.h"
#include "streaming/streaming_client.h"
#include "log.h"
#include <cstdio>
#include <cstring>
#include <cmath>

// Tab indices match the settingsModel() order:
// 0=Servers, 1=Settings, 2=Stats, 3=Apps, 4=Launch, 5=About, 6=Licenses, 7=Exit
// We keep our own tab index since we don't use the MenuModel directly.
static int sCurrentTab = 0;
static bool sStreamingMode = false;

void applyImGuiTheme()
{
    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowRounding = 8.0f;
    s.FrameRounding = 6.0f;
    s.GrabRounding = 4.0f;
    s.PopupRounding = 6.0f;
    s.ChildRounding = 6.0f;
    s.ScrollbarRounding = 6.0f;
    s.WindowBorderSize = 0.0f;
    s.FrameBorderSize = 0.0f;
    s.WindowPadding = ImVec2(16, 12);
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(10, 8);
    s.ItemInnerSpacing = ImVec2(8, 6);

    ImVec4 *c = s.Colors;
    c[ImGuiCol_WindowBg]        = ImVec4(0.10f, 0.10f, 0.10f, 0.94f);
    c[ImGuiCol_ChildBg]         = ImVec4(0.08f, 0.08f, 0.08f, 1.0f);
    c[ImGuiCol_PopupBg]         = ImVec4(0.12f, 0.12f, 0.12f, 0.96f);
    c[ImGuiCol_Border]          = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_Text]            = ImVec4(0.90f, 0.90f, 0.92f, 1.0f);
    c[ImGuiCol_TextDisabled]    = ImVec4(0.50f, 0.52f, 0.56f, 1.0f);
    c[ImGuiCol_FrameBg]         = ImVec4(0.13f, 0.13f, 0.13f, 1.0f);
    c[ImGuiCol_FrameBgHovered]  = ImVec4(0.26f, 0.34f, 0.46f, 1.0f);
    c[ImGuiCol_FrameBgActive]   = ImVec4(0.24f, 0.52f, 0.88f, 1.0f);
    c[ImGuiCol_Button]          = ImVec4(0.13f, 0.13f, 0.13f, 1.0f);
    c[ImGuiCol_ButtonHovered]   = ImVec4(0.26f, 0.34f, 0.46f, 1.0f);
    c[ImGuiCol_ButtonActive]    = ImVec4(0.24f, 0.52f, 0.88f, 1.0f);
    c[ImGuiCol_CheckMark]       = ImVec4(0.24f, 0.52f, 0.88f, 1.0f);
    c[ImGuiCol_SliderGrab]      = ImVec4(0.24f, 0.52f, 0.88f, 1.0f);
    c[ImGuiCol_SliderGrabActive]= ImVec4(0.40f, 0.65f, 1.0f, 1.0f);
    c[ImGuiCol_Header]          = ImVec4(0.26f, 0.34f, 0.46f, 1.0f);
    c[ImGuiCol_HeaderHovered]   = ImVec4(0.24f, 0.52f, 0.88f, 1.0f);
    c[ImGuiCol_HeaderActive]    = ImVec4(0.24f, 0.52f, 0.88f, 1.0f);
    c[ImGuiCol_ScrollbarBg]     = ImVec4(0.05f, 0.05f, 0.05f, 0.5f);
    c[ImGuiCol_ScrollbarGrab]   = ImVec4(0.20f, 0.20f, 0.20f, 1.0f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.30f, 0.30f, 0.30f, 1.0f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.24f, 0.52f, 0.88f, 1.0f);
    c[ImGuiCol_Separator]       = ImVec4(0.20f, 0.20f, 0.20f, 1.0f);
    c[ImGuiCol_SeparatorHovered]= ImVec4(0.24f, 0.52f, 0.88f, 1.0f);
}

// ---- Servers tab ----
static void buildServersTab()
{
    ImGui::Text("Servers");
    ImGui::Separator();
    ImGui::Spacing();

    auto err = getConnectionError();
    if (!err.empty()) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.25f, 0.08f, 0.08f, 1.0f));
        ImGui::BeginChild("##err", ImVec2(0, 40), true);
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.7f, 1.0f), "%s", err.c_str());
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    auto servers = getServerList();
    if (servers.empty()) {
        ImGui::TextDisabled("Start a WiVRn server on your local network");
        ImGui::Spacing();
    }

    for (int i = 0; i < (int)servers.size(); i++) {
        const auto &srv = servers[i];
        ImGui::PushID(i);

        char rowLabel[256];
        snprintf(rowLabel, sizeof(rowLabel), "%s", srv.name.c_str());
        if (srv.discovered) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 0.7f, 0.4f, 1.0f));
            ImGui::Text("%s  * Discovered", srv.name.c_str());
            ImGui::PopStyleColor();
        } else {
            ImGui::Text("%s", srv.name.c_str());
        }
        ImGui::TextDisabled("  %s:%d", srv.hostname.c_str(), srv.port);

        ImGui::SameLine(ImGui::GetWindowWidth() - 280);

        if (!srv.manual) {
            bool autoConn = srv.autoconnect;
            if (ImGui::Checkbox("Auto", &autoConn)) {
                if (gOnServerAutoconnect)
                    gOnServerAutoconnect(srv.hostname, srv.port);
            }
            ImGui::SameLine();
        }

        const char *btnLabel = "Connect";
        if (isConnecting()) btnLabel = "...";
        if (ImGui::Button(btnLabel, ImVec2(120, 0))) {
            clearConnectionError();
            if (gOnServerConnect)
                gOnServerConnect(srv);
        }
        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.1f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.5f, 0.2f, 0.2f, 1.0f));
        if (ImGui::Button("X", ImVec2(40, 0))) {
            if (gOnServerRemove)
                gOnServerRemove(srv.hostname, srv.port);
        }
        ImGui::PopStyleColor(2);

        ImGui::Separator();
        ImGui::PopID();
    }

    ImGui::Spacing();
    if (ImGui::Button("Refresh", ImVec2(150, 40))) {
        if (gOnRefreshServers)
            gOnRefreshServers();
    }
}

// ---- Settings tab ----
static void buildSettingsTab()
{
    ImGui::Text("Settings");
    ImGui::Separator();
    ImGui::Spacing();

    // Software IPD (stepper)
    float ipd = gSoftIpdMm.load();
    ImGui::Text("Software IPD: %.1f mm", ipd);
    ImGui::SameLine();
    ImGui::PushID("ipd");
    if (ImGui::Button("-")) {
        ipd = fmaxf(kIpdMin, ipd - kIpdStep);
        gSoftIpdMm.store(ipd);
        gIpdChangeNs.store(nowNs());
        gIpdDirty.store(true);
    }
    ImGui::SameLine();
    if (ImGui::Button("+")) {
        ipd = fminf(kIpdMax, ipd + kIpdStep);
        gSoftIpdMm.store(ipd);
        gIpdChangeNs.store(nowNs());
        gIpdDirty.store(true);
    }
    ImGui::PopID();
    ImGui::Spacing();

    // Brightness (fader)
    float bright = gBrightnessFrac.load();
    if (ImGui::SliderFloat("Brightness", &bright, 0.0f, 1.0f, "%.0f%%")) {
        gBrightnessFrac.store(bright);
        gBrightnessSaved.store(true);
        gBrightnessApply.store(true);
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
        saveBrightness();
    ImGui::Spacing();

    // Field of view (fader)
    float fov = gStreamFovDeg.load();
    if (ImGui::SliderFloat("Field of view", &fov, kFovMin, kFovMax, "%.0f deg")) {
        gStreamFovDeg.store(roundf(fov));
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
        gFovDirty.store(true);
    ImGui::Spacing();

    // Resolution scale (fader)
    float res = gWivrnResolutionScale.load();
    char resLabel[64];
    int rw = (int)((1664 * res) / 2) * 2;
    int rh = (int)((1756 * res) / 2) * 2;
    snprintf(resLabel, sizeof(resLabel), "Resolution scale: %d%% (%dx%d)", (int)(res * 100), rw, rh);
    if (ImGui::SliderFloat(resLabel, &res, 0.5f, 2.0f, "%.2f")) {
        gWivrnResolutionScale.store(res);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        if (g_stream && g_stream->session) {
            constexpr int base_w = 1664, base_h = 1756;
            float s = gWivrnResolutionScale.load();
            int nrw = (int)(base_w * s) / 2 * 2;
            int nrh = (int)(base_h * s) / 2 * 2;
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
    ImGui::Spacing();

    // Bitrate (fader)
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
    ImGui::Spacing();

    // Eye-tracked foveation (toggle)
    bool eyeFov = gWivrnEyeFoveation.load();
    if (!gEyeSupported.load()) ImGui::BeginDisabled();
    if (ImGui::Checkbox("Eye-tracked foveation", &eyeFov)) {
        gWivrnEyeFoveation.store(eyeFov);
        saveAllConfig();
        gEyeFoveationDirty.store(true);
    }
    if (!gEyeSupported.load()) ImGui::EndDisabled();
    ImGui::Spacing();

    // Microphone (toggle)
    bool mic = gWivrnMicrophone.load();
    if (ImGui::Checkbox("Microphone", &mic)) {
        gWivrnMicrophone.store(mic);
        if (g_stream) {
            g_stream->microphone_enabled.store(mic);
            if (g_stream->audio_handle)
                g_stream->audio_handle->set_mic_state(mic);
            g_stream->send_headset_info();
        }
        saveAllConfig();
    }
    ImGui::Spacing();

    // Controller vibration (fader)
    float vib = gWivrnCtrlVibration.load();
    if (ImGui::SliderFloat("Controller vibration", &vib, 0.0f, 1.0f, "%.0f%%")) {
        gWivrnCtrlVibration.store(vib);
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
        saveAllConfig();
    ImGui::Spacing();

    // Passthrough (toggle)
    bool pt = gWivrnPassthrough.load();
    if (ImGui::Checkbox("Passthrough", &pt)) {
        gWivrnPassthrough.store(pt);
        extern pico_passthrough *gPassthrough;
        if (pt) {
            if (gPassthrough && !gPassthrough->is_camera_on()) gPassthrough->start();
        } else {
            if (gPassthrough && gPassthrough->is_camera_on()) gPassthrough->stop();
        }
        saveAllConfig();
    }
    ImGui::Spacing();

    // Recenter (button)
    if (ImGui::Button("Recenter", ImVec2(200, 0))) {
        gWivrnRecenterReq.store(true);
    }
    ImGui::Spacing();

    // Eye debug (toggle)
    bool eyeDbg = gEyeDebugOn.load();
    if (ImGui::Checkbox("Eye debug", &eyeDbg)) {
        gEyeDebugOn.store(eyeDbg);
        saveEyeDebug();
        gEyeTrackReapply.store(true);
    }
    ImGui::Spacing();

    // Diagnostics HUD (dropdown)
    const char *diagOpts[] = {"Off", "Pipeline", "System"};
    int diag = gDiagHudMode.load();
    if (ImGui::Combo("Diagnostics HUD", &diag, diagOpts, 3)) {
        gDiagHudMode.store(diag);
        saveDiagHud();
    }
}

// ---- Stats tab (streaming only) ----
static void buildStatsTab()
{
    ImGui::Text("Performance");
    ImGui::Separator();
    ImGui::Spacing();

    if (!g_stream) {
        ImGui::TextDisabled("Not streaming");
        return;
    }

    int fps = g_stream->stats_fps;
    float lat = g_stream->stats_total_latency_ms;
    ImGui::Text("Framerate: %d fps", fps);
    ImGui::Text("Total latency: %.0f ms", lat);
    ImGui::Spacing();

    ImGui::Text("Download: %.1f Mbit/s", g_stream->stats_bandwidth_rx * 1e-6f);
    ImGui::Text("Upload: %.1f Mbit/s", g_stream->stats_bandwidth_tx * 1e-6f);
    ImGui::Spacing();

    ImGui::Text("CPU time: %.1f ms", g_stream->stats_cpu_time_ms);
    ImGui::Text("GPU time: %.1f ms", g_stream->stats_gpu_time_ms);
    ImGui::Spacing();

    ImGui::Text("Latency breakdown");
    ImGui::Separator();
    struct { const char *name; float ms; } bars[] = {
        {"Encode",  g_stream->stats_encode_ms},
        {"Send",    g_stream->stats_send_ms},
        {"Network", g_stream->stats_network_ms},
        {"Decode",  g_stream->stats_decode_ms},
        {"Render",  g_stream->stats_render_wait_ms},
        {"Blit",    g_stream->stats_blit_ms},
    };
    float maxMs = 1.0f;
    for (auto &b : bars) if (b.ms > maxMs) maxMs = b.ms;
    for (auto &b : bars) {
        float frac = b.ms / maxMs;
        ImGui::Text("%-10s", b.name);
        ImGui::SameLine(100);
        ImGui::ProgressBar(frac, ImVec2(200, 0), "");
        ImGui::SameLine();
        ImGui::Text("%.1f ms", b.ms);
    }
    ImGui::Spacing();

    ImGui::Text("Stream info");
    ImGui::Separator();
    ImGui::Text("Bitrate: %d Mbit/s", g_stream->current_bitrate_mbps.load());
    ImGui::Text("Resolution: %dx%d", g_stream->stream_eye_width.load(), g_stream->stream_eye_height.load());
    ImGui::Text("Microphone: %s", g_stream->microphone_enabled.load() ? "On" : "Off");
}

// ---- Apps tab (streaming only) ----
static void buildAppsTab()
{
    ImGui::Text("Running apps");
    ImGui::Separator();
    ImGui::Spacing();

    if (!g_stream) {
        ImGui::TextDisabled("Not streaming");
        return;
    }

    std::vector<streaming_client::RunningApp> apps;
    {
        std::lock_guard<std::mutex> lk(g_stream->app_mutex);
        apps = g_stream->running_apps;
    }

    if (apps.empty()) {
        ImGui::TextDisabled("No apps running");
        return;
    }

    for (int i = 0; i < (int)apps.size(); i++) {
        ImGui::PushID(i);
        const char *marker = apps[i].active ? "> " : "  ";
        ImGui::Text("%s%s%s", marker, apps[i].name.c_str(),
                    apps[i].overlay ? " (overlay)" : "");
        ImGui::SameLine(ImGui::GetWindowWidth() - 80);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.1f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.2f, 0.2f, 1.0f));
        if (ImGui::Button("Stop", ImVec2(60, 0))) {
            if (g_stream->session) {
                try {
                    g_stream->session->send_control(
                        wivrn::from_headset::stop_application{apps[i].id});
                } catch (std::exception &e) {
                    LOGI("stop_application failed: %s", e.what());
                }
            }
        }
        ImGui::PopStyleColor(2);
        if (apps[i].active && !apps[i].overlay && g_stream->session) {
            // already active
        } else if (!apps[i].overlay && ImGui::IsItemHovered() == false && apps[i].active == false) {
            // click on row to set active - handled by button below
        }
        ImGui::PopID();
    }
}

// ---- Launch tab (streaming only) ----
static void buildLaunchTab()
{
    ImGui::Text("Launch application");
    ImGui::Separator();
    ImGui::Spacing();

    if (!g_stream) {
        ImGui::TextDisabled("Not streaming");
        return;
    }

    std::vector<streaming_client::AppEntry> apps;
    bool requested;
    std::string launching_id;
    {
        std::lock_guard<std::mutex> lk(g_stream->app_mutex);
        apps = g_stream->available_apps;
        requested = g_stream->app_list_requested;
        launching_id = g_stream->launching_app_id;
    }

    if (apps.empty()) {
        ImGui::TextDisabled("%s", requested ? "Loading..." : "No apps available");
        if (ImGui::Button("Refresh")) {
            if (g_stream->session) {
                try {
                    g_stream->session->send_control(
                        wivrn::from_headset::get_application_list{"en", "US", ""});
                } catch (std::exception &e) {
                    LOGI("get_application_list failed: %s", e.what());
                }
            }
        }
        return;
    }

    for (int i = 0; i < (int)apps.size(); i++) {
        ImGui::PushID(i);
        ImGui::Text("%s", apps[i].name.c_str());
        ImGui::SameLine(ImGui::GetWindowWidth() - 160);
        bool launching = (!launching_id.empty() && apps[i].id == launching_id);
        if (launching) {
            ImGui::TextDisabled("Launching...");
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.3f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.5f, 0.25f, 1.0f));
            if (ImGui::Button("Launch", ImVec2(120, 0))) {
                if (g_stream->session) {
                    try {
                        g_stream->session->send_control(
                            wivrn::from_headset::start_app{apps[i].id});
                        g_stream->launching_app_id = apps[i].id;
                    } catch (std::exception &e) {
                        LOGI("start_app failed: %s", e.what());
                    }
                }
            }
            ImGui::PopStyleColor(2);
        }
        ImGui::PopID();
    }
}

// ---- About tab ----
static void buildAboutTab()
{
    ImGui::Text("WiVRn for Pico Neo 2");
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextWrapped("Stream PC VR games to your Pico Neo 2 over Wi-Fi or USB with low latency");
    ImGui::Spacing();
    ImGui::TextDisabled("Upstream: github.com/wivrn/wivrn");
    ImGui::TextDisabled("This client: gitlab.com/httpanimations/piconeo2-wivrn");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Licensed under AGPL v3");
    ImGui::TextDisabled("See Licenses tab for details");
}

// ---- Licenses tab ----
static void buildLicensesTab()
{
    ImGui::Text("AGPL v3");
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextWrapped("This project is licensed under the GNU Affero General Public License v3");
    ImGui::Spacing();
    ImGui::Text("Full license text:");
    ImGui::TextDisabled("gitlab.com/httpanimations/piconeo2-wivrn/-/raw/main/LICENSE");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Third-party components");
    ImGui::Spacing();
    ImGui::TextDisabled("ALVR client library - MIT License");
    ImGui::TextDisabled("github.com/alvr-org/alvr");
    ImGui::Spacing();
    ImGui::TextDisabled("3D controller models - Pico Interactive");
}

// ---- Exit tab ----
static void buildExitTab()
{
    ImGui::Text("Exit");
    ImGui::Separator();
    ImGui::Spacing();

    if (sStreamingMode) {
        if (ImGui::Button("Disconnect", ImVec2(250, 60))) {
            if (g_stream) {
                g_stream->shutdown = true;
                g_stream->auto_reconnect.store(false);
            }
        }
    } else {
        if (ImGui::Button("Quit WiVRn", ImVec2(250, 60))) {
            if (gOnExit) gOnExit();
        }
    }
}

// ---- Main entry point ----
void buildImGuiUI()
{
    applyImGuiTheme();

    // Track streaming mode (updated by render thread)
    sStreamingMode = gStreamingMode;

    // Full-screen window covering the entire UI surface.
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("##lobby", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Sidebar width
    float sidebarW = 250;

    // Left sidebar
    ImGui::BeginChild("##sidebar", ImVec2(sidebarW, 0), true);

    // Top group: Servers, Settings, [Stats, Apps, Launch]
    struct TabDef { const char *name; int id; bool streamingOnly; };
    TabDef topTabs[] = {
        {"Servers",  0, false},
        {"Settings", 1, false},
        {"Stats",    2, true},
        {"Apps",     3, true},
        {"Launch",   4, true},
    };
    for (auto &t : topTabs) {
        if (t.streamingOnly && !sStreamingMode) continue;
        bool selected = (sCurrentTab == t.id);
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.10f, 0.35f, 0.70f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.10f, 0.35f, 0.70f, 1.0f));
        }
        if (ImGui::Button(t.name, ImVec2(-1, 50)))
            sCurrentTab = t.id;
        if (selected) ImGui::PopStyleColor(2);
        ImGui::Spacing();
    }

    // Spacer to push bottom group down
    ImGui::Dummy(ImVec2(0, 20));

    // Bottom group: About, Licenses, Exit
    TabDef bottomTabs[] = {
        {"About",    5, false},
        {"Licenses", 6, false},
        {"Exit",     7, false},
    };
    for (auto &t : bottomTabs) {
        bool selected = (sCurrentTab == t.id);
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.10f, 0.35f, 0.70f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.10f, 0.35f, 0.70f, 1.0f));
        }
        if (ImGui::Button(t.name, ImVec2(-1, 50)))
            sCurrentTab = t.id;
        if (selected) ImGui::PopStyleColor(2);
        ImGui::Spacing();
    }

    ImGui::EndChild();

    ImGui::SameLine();

    // Main content area with scrolling
    ImGui::BeginChild("##content", ImVec2(0, 0), true,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
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
