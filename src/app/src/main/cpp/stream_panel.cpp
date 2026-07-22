#include "stream_panel.h"
#include "settings_panel.h"
#include "streaming/streaming_client.h"
#include "ui_kit.h"
#include "log.h"
#include <cstdio>
#include <cstring>
#include <cmath>

// Truncate s into buf (size bufSz) so the rendered width fits availW at scale px.
// Each char is 6 font-units wide; rendered width = (n*6 - 1) * px.
// Appends "..." if truncated.
static void truncateFit(char *buf, size_t bufSz, const char *s, float availW, float px) {
    if (bufSz == 0) return;
    size_t n = strlen(s);
    float w = (n * 6 - 1) * px;
    if (w <= availW || n < 4) {
        strncpy(buf, s, bufSz - 1);
        buf[bufSz - 1] = 0;
        return;
    }
    // Reserve 3 chars for "..." and find max that fits
    size_t maxChars = (size_t)((availW / px + 1) / 6);
    if (maxChars < 4) maxChars = 4;
    if (maxChars - 3 >= bufSz) maxChars = bufSz + 2; // will be clamped
    size_t keep = maxChars - 3;
    if (keep >= bufSz) keep = bufSz - 4;
    strncpy(buf, s, keep);
    buf[keep] = 0;
    strcat(buf, "...");
}

// ===========================================================================
// STATS category: FPS, latency, bandwidth, CPU/GPU, latency breakdown.
// MK_CUSTOM, self-positioning. MenuHover.part encodes sub-regions (none yet).
// ===========================================================================
static void statsBuild(std::vector<float> &v, const MenuHover &h) {
    (void)h;
    if (!g_stream) return;

    float x = kCtX0 + 0.04f;
    float y = kMenuTopY;
    float px = kUiText * 1.4f;
    float lineH = 0.06f;
    float colW = (kCtX1Content - kCtX0 - 0.08f) * 0.5f;
    float col2X = x + colW + 0.04f;

    // Title
    uiTextL(v, "Performance", x, y, px * 1.3f, kUiTitle[0], kUiTitle[1], kUiTitle[2]);
    y -= lineH * 1.8f;

    auto writeStat = [&](float cx, float cy, const char *label, const char *val) {
        uiTextL(v, label, cx, cy, px * 0.85f, 0.5f, 0.55f, 0.6f);
        uiTextL(v, val, cx, cy - lineH * 0.9f, px, kUiWhite[0], kUiWhite[1], kUiWhite[2]);
    };

    char buf[64];
    int fps = g_stream->stats_fps;
    snprintf(buf, sizeof(buf), "%d fps", fps);
    writeStat(x, y, "Framerate", buf);
    float lat = g_stream->stats_total_latency_ms;
    snprintf(buf, sizeof(buf), "%.0f ms", lat);
    writeStat(col2X, y, "Total latency", buf);
    y -= lineH * 2.5f;

    snprintf(buf, sizeof(buf), "%.1f Mbit/s", g_stream->stats_bandwidth_rx * 1e-6f);
    writeStat(x, y, "Download", buf);
    snprintf(buf, sizeof(buf), "%.1f Mbit/s", g_stream->stats_bandwidth_tx * 1e-6f);
    writeStat(col2X, y, "Upload", buf);
    y -= lineH * 2.5f;

    snprintf(buf, sizeof(buf), "%.1f ms", g_stream->stats_cpu_time_ms);
    writeStat(x, y, "CPU time", buf);
    snprintf(buf, sizeof(buf), "%.1f ms", g_stream->stats_gpu_time_ms);
    writeStat(col2X, y, "GPU time", buf);
    y -= lineH * 2.5f;

    // Latency breakdown
    uiTextL(v, "Latency breakdown", x, y, px * 1.1f, kUiTitle[0], kUiTitle[1], kUiTitle[2]);
    y -= lineH * 1.5f;

    struct { const char *name; float ms; float col[3]; } bars[] = {
        {"Encode",   g_stream->stats_encode_ms,   {0.9f, 0.6f, 0.3f}},
        {"Send",     g_stream->stats_send_ms,     {0.8f, 0.7f, 0.4f}},
        {"Network",  g_stream->stats_network_ms,  {0.4f, 0.7f, 0.9f}},
        {"Decode",   g_stream->stats_decode_ms,   {0.5f, 0.8f, 0.5f}},
        {"Render",   g_stream->stats_render_wait_ms, {0.7f, 0.5f, 0.8f}},
        {"Blit",     g_stream->stats_blit_ms,     {0.6f, 0.6f, 0.7f}},
    };
    float maxMs = 1.0f;
    for (auto &b : bars) if (b.ms > maxMs) maxMs = b.ms;
    float barW = (kCtX1Content - x - 0.08f) * 0.6f;
    float barH = 0.025f;
    for (auto &b : bars) {
        uiTextL(v, b.name, x, y, px * 0.8f, 0.5f, 0.55f, 0.6f);
        float frac = b.ms / maxMs;
        appendQuad(v, x, y - lineH * 0.5f, x + barW * frac, y - lineH * 0.5f - barH,
                   b.col[0], b.col[1], b.col[2]);
        snprintf(buf, sizeof(buf), "%.1f", b.ms);
        uiTextL(v, buf, x + barW + 0.02f, y - lineH * 0.5f - barH, px * 0.7f,
                kUiWhite[0], kUiWhite[1], kUiWhite[2]);
        y -= lineH * 1.2f;
    }

    y -= lineH * 0.5f;
    // Stream info
    uiTextL(v, "Stream info", x, y, px * 1.1f, kUiTitle[0], kUiTitle[1], kUiTitle[2]);
    y -= lineH * 1.5f;

    snprintf(buf, sizeof(buf), "%d Mbit/s", g_stream->current_bitrate_mbps.load());
    writeStat(x, y, "Bitrate", buf);
    int rw = g_stream->stream_eye_width.load(), rh = g_stream->stream_eye_height.load();
    snprintf(buf, sizeof(buf), "%dx%d", rw, rh);
    writeStat(col2X, y, "Resolution", buf);
    y -= lineH * 2.5f;

    bool mic = g_stream->microphone_enabled.load();
    writeStat(x, y, "Microphone", mic ? "On" : "Off");
}

static void statsHit(float, float, MenuHover &) {}
static void statsAct(const MenuHover &, bool, bool, float, float) {}

// ===========================================================================
// APPS category: running XR applications with stop buttons.
// MenuHover.part: 0 = row, 100+i = stop button for app i.
// ===========================================================================
static void appsBuild(std::vector<float> &v, const MenuHover &h) {
    if (!g_stream) return;
    std::vector<streaming_client::RunningApp> apps;
    {
        std::lock_guard<std::mutex> lk(g_stream->app_mutex);
        apps = g_stream->running_apps;
    }

    float x = kCtX0 + 0.04f;
    float y = kMenuTopY;
    float px = kUiText * 1.4f;

    uiTextL(v, "Running apps", x, y, px * 1.3f, kUiTitle[0], kUiTitle[1], kUiTitle[2]);
    y -= 0.10f;

    if (apps.empty()) {
        uiTextL(v, "No apps running", x, y, px, 0.5f, 0.5f, 0.55f);
        return;
    }

    float rowH = 0.08f;
    float rowGap = 0.02f;
    float contentW = kCtX1Content - kCtX0 - 0.08f;

    for (int i = 0; i < (int)apps.size(); i++) {
        float yBot = y - rowH;
        bool hot = (h.item == i);
        bool isOverlay = apps[i].overlay;
        bool isActive = apps[i].active;

        float bg[3] = {0.08f, 0.09f, 0.12f};
        if (hot) { bg[0] = 0.14f; bg[1] = 0.18f; bg[2] = 0.24f; }
        appendQuad(v, kCtX0, y, kCtX1Content, yBot, bg[0], bg[1], bg[2]);

        // Active marker
        float tx = x;
        if (isActive) {
            uiTextL(v, ">", tx, y - 0.02f, px, 0.4f, 0.8f, 0.4f);
            tx += 0.04f;
        }
        // App name (truncate to fit before the stop button)
        float btnSz_apps = 0.05f;
        float btnX_apps = kCtX1Content - btnSz_apps - 0.03f;
        float availW = btnX_apps - tx - 0.02f;
        char name[48];
        truncateFit(name, sizeof(name), apps[i].name.c_str(), availW, px);
        uiTextL(v, name, tx, y - 0.02f, px, kUiWhite[0], kUiWhite[1], kUiWhite[2]);

        if (isOverlay) {
            uiTextL(v, "overlay", tx, y - 0.05f, px * 0.7f, 0.5f, 0.55f, 0.6f);
        }

        // Stop button (red X)
        float btnSz = 0.05f;
        float btnX = kCtX1Content - btnSz - 0.03f;
        float btnY = y - rowH * 0.5f;
        float sb[3] = {0.3f, 0.1f, 0.1f};
        if (hot && h.part >= 100) { sb[0] = 0.7f; sb[1] = 0.2f; sb[2] = 0.2f; }
        appendQuad(v, btnX, btnY + btnSz*0.5f, btnX + btnSz, btnY - btnSz*0.5f,
                   sb[0], sb[1], sb[2]);
        uiTextC(v, "X", btnX + btnSz*0.5f, btnY + 3.5f * (px * 0.8f), px * 0.8f,
                1, 1, 1);

        y = yBot - rowGap;
    }
}

static void appsHit(float cx, float cy, MenuHover &h) {
    if (!g_stream) return;
    std::vector<streaming_client::RunningApp> apps;
    {
        std::lock_guard<std::mutex> lk(g_stream->app_mutex);
        apps = g_stream->running_apps;
    }
    if (apps.empty()) return;

    float yTop = kMenuTopY - 0.10f;
    float rowH = 0.08f, rowGap = 0.02f;
    float btnSz = 0.05f;

    for (int i = 0; i < (int)apps.size(); i++) {
        float y = yTop - i * (rowH + rowGap);
        float yBot = y - rowH;
        if (cy > y || cy < yBot) continue;

        // Stop button
        float btnX = kCtX1Content - btnSz - 0.03f;
        float btnY = y - rowH * 0.5f;
        if (cx >= btnX && cx <= btnX + btnSz &&
            cy >= btnY - btnSz*0.5f && cy <= btnY + btnSz*0.5f) {
            h.item = i; h.part = 100 + i; h.grab = true; return;
        }
        h.item = i; h.part = 0; h.grab = false; return;
    }
}

static void appsAct(const MenuHover &h, bool click, bool, float, float) {
    if (!g_stream || !click) return;
    if (h.part >= 100) {
        int idx = h.part - 100;
        std::vector<streaming_client::RunningApp> apps;
        {
            std::lock_guard<std::mutex> lk(g_stream->app_mutex);
            apps = g_stream->running_apps;
        }
        if (idx < (int)apps.size() && g_stream->session) {
            try {
                g_stream->session->send_control(
                    wivrn::from_headset::stop_application{apps[idx].id});
                LOGI("stop_application id=%u", apps[idx].id);
            } catch (std::exception &e) {
                LOGI("stop_application failed: %s", e.what());
            }
        }
    } else if (h.part == 0 && h.item >= 0) {
        // Set active application
        std::vector<streaming_client::RunningApp> apps;
        {
            std::lock_guard<std::mutex> lk(g_stream->app_mutex);
            apps = g_stream->running_apps;
        }
        if (h.item < (int)apps.size() && !apps[h.item].overlay && g_stream->session) {
            try {
                g_stream->session->send_control(
                    wivrn::from_headset::set_active_application{apps[h.item].id});
                LOGI("set_active_application id=%u", apps[h.item].id);
            } catch (std::exception &e) {
                LOGI("set_active_application failed: %s", e.what());
            }
        }
    }
}

// ===========================================================================
// LAUNCH category: available applications list with launch buttons.
// MenuHover.part: 0 = row, 100+i = launch button for app i, 200 = refresh.
// ===========================================================================
static void launchBuild(std::vector<float> &v, const MenuHover &h) {
    if (!g_stream) return;
    std::vector<streaming_client::AppEntry> apps;
    bool requested;
    {
        std::lock_guard<std::mutex> lk(g_stream->app_mutex);
        apps = g_stream->available_apps;
        requested = g_stream->app_list_requested;
    }

    float x = kCtX0 + 0.04f;
    float y = kMenuTopY;
    float px = kUiText * 1.4f;

    uiTextL(v, "Launch application", x, y, px * 1.3f, kUiTitle[0], kUiTitle[1], kUiTitle[2]);
    y -= 0.10f;

    if (apps.empty()) {
        if (requested) {
            uiTextL(v, "Loading...", x, y, px, 0.5f, 0.5f, 0.55f);
        } else {
            uiTextL(v, "No apps available", x, y, px, 0.5f, 0.5f, 0.55f);
        }
        // Refresh button
        UiRect btn = { x + 0.10f, y - 0.06f, 0.18f, 0.06f };
        uiButton(v, btn, "Refresh", h.part == 200);
        return;
    }

    float rowH = 0.08f;
    float rowGap = 0.02f;

    for (int i = 0; i < (int)apps.size(); i++) {
        float yBot = y - rowH;
        bool hot = (h.item == i);

        float bg[3] = {0.08f, 0.09f, 0.12f};
        if (hot) { bg[0] = 0.14f; bg[1] = 0.18f; bg[2] = 0.24f; }
        appendQuad(v, kCtX0, y, kCtX1Content, yBot, bg[0], bg[1], bg[2]);

        // App name (truncate to fit before the launch button)
        float btnW_launch = 0.16f;
        float btnX_launch = kCtX1Content - btnW_launch - 0.03f;
        float availW = btnX_launch - x - 0.02f;
        char name[48];
        truncateFit(name, sizeof(name), apps[i].name.c_str(), availW, px);
        uiTextL(v, name, x, y - 0.02f, px, kUiWhite[0], kUiWhite[1], kUiWhite[2]);

        // Launch button (green)
        float btnW = 0.16f, btnH = 0.06f;
        float btnX = kCtX1Content - btnW - 0.03f;
        float btnY = y - rowH * 0.5f;
        float gc[3] = {0.1f, 0.3f, 0.15f};
        if (hot && h.part >= 100) { gc[0] = 0.2f; gc[1] = 0.5f; gc[2] = 0.25f; }
        appendQuad(v, btnX, btnY + btnH*0.5f, btnX + btnW, btnY - btnH*0.5f,
                   gc[0], gc[1], gc[2]);
        uiTextC(v, "Launch", btnX + btnW*0.5f, btnY + 3.5f * (px * 0.7f), px * 0.7f, 1, 1, 1);

        y = yBot - rowGap;
    }
}

static void launchHit(float cx, float cy, MenuHover &h) {
    if (!g_stream) return;
    std::vector<streaming_client::AppEntry> apps;
    bool requested;
    {
        std::lock_guard<std::mutex> lk(g_stream->app_mutex);
        apps = g_stream->available_apps;
        requested = g_stream->app_list_requested;
    }

    float yTop = kMenuTopY - 0.10f;

    if (apps.empty()) {
        // Refresh button
        UiRect btn = { kCtX0 + 0.14f, yTop - 0.06f, 0.18f, 0.06f };
        if (uiHit(btn, cx, cy)) { h.item = 0; h.part = 200; h.grab = true; }
        return;
    }

    float rowH = 0.08f, rowGap = 0.02f;
    float btnW = 0.16f, btnH = 0.06f;

    for (int i = 0; i < (int)apps.size(); i++) {
        float y = yTop - i * (rowH + rowGap);
        float yBot = y - rowH;
        if (cy > y || cy < yBot) continue;

        // Launch button
        float btnX = kCtX1Content - btnW - 0.03f;
        float btnY = y - rowH * 0.5f;
        if (cx >= btnX && cx <= btnX + btnW &&
            cy >= btnY - btnH*0.5f && cy <= btnY + btnH*0.5f) {
            h.item = i; h.part = 100 + i; h.grab = true; return;
        }
        h.item = i; h.part = 0; h.grab = false; return;
    }
}

static void launchAct(const MenuHover &h, bool click, bool, float, float) {
    if (!g_stream || !click) return;
    if (h.part == 200) {
        // Refresh
        if (g_stream->session) {
            try {
                g_stream->session->send_control(
                    wivrn::from_headset::get_application_list{"en", "US", ""});
                LOGI("requested application list");
            } catch (std::exception &e) {
                LOGI("get_application_list failed: %s", e.what());
            }
        }
        return;
    }
    if (h.part >= 100) {
        int idx = h.part - 100;
        std::vector<streaming_client::AppEntry> apps;
        {
            std::lock_guard<std::mutex> lk(g_stream->app_mutex);
            apps = g_stream->available_apps;
        }
        if (idx < (int)apps.size() && g_stream->session) {
            try {
                g_stream->session->send_control(
                    wivrn::from_headset::start_app{apps[idx].id});
                LOGI("start_app id=%s", apps[idx].id.c_str());
            } catch (std::exception &e) {
                LOGI("start_app failed: %s", e.what());
            }
        }
    }
}

// ===========================================================================
void buildStreamCategories(MenuModel &m) {
    // STATS
    MenuCategory stats; stats.name = "Stats"; stats.custom = true; stats.streamingOnly = true;
    {
        MenuItem it; it.kind = MK_CUSTOM;
        it.customH = 0.8f;
        it.cBuild = statsBuild;
        it.cHit = statsHit;
        it.cAct = statsAct;
        stats.items.push_back(it);
    }
    m.push_back(stats);

    // APPS
    MenuCategory apps; apps.name = "Apps"; apps.custom = true; apps.streamingOnly = true;
    {
        MenuItem it; it.kind = MK_CUSTOM;
        it.customH = 0.8f;
        it.cBuild = appsBuild;
        it.cHit = appsHit;
        it.cAct = appsAct;
        apps.items.push_back(it);
    }
    m.push_back(apps);

    // LAUNCH
    MenuCategory launch; launch.name = "Launch"; launch.custom = true; launch.streamingOnly = true;
    {
        MenuItem it; it.kind = MK_CUSTOM;
        it.customH = 0.8f;
        it.cBuild = launchBuild;
        it.cHit = launchHit;
        it.cAct = launchAct;
        launch.items.push_back(it);
    }
    m.push_back(launch);
}
