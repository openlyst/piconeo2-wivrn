#include "server_list.h"
#include "settings_panel.h"  // kCtX0, kCtX1, kCtTop, kCtBot
#include "log.h"
#include <mutex>
#include <cstring>

static std::mutex gSrvMutex;
static std::vector<ServerInfo> gServers;

std::function<void(const ServerInfo &)> gOnServerConnect;

void setServerList(const std::vector<ServerInfo> & servers) {
    std::lock_guard<std::mutex> lk(gSrvMutex);
    gServers = servers;
}

std::vector<ServerInfo> getServerList() {
    std::lock_guard<std::mutex> lk(gSrvMutex);
    return gServers;
}

// Row layout constants (panel-local metres).
static constexpr float kRowH = 0.14f;
static constexpr float kRowGap = 0.02f;

float serverContentHeight() {
    auto servers = getServerList();
    if (servers.empty()) return 0.10f;
    return servers.size() * kRowH + (servers.size() - 1) * kRowGap;
}

float buildServerContent(std::vector<float> &v, float scrollY,
                         int hoverItem, int connectHot) {
    auto servers = getServerList();
    float contentW = kCtX1 - kCtX0;

    if (servers.empty()) {
        uiTextC(v, "START A WIVRN SERVER", (kCtX0 + kCtX1) * 0.5f,
                kCtTop - 0.05f - scrollY, kUiText * 1.8f, 0.5f, 0.5f, 0.55f);
        uiTextC(v, "ON YOUR LOCAL NETWORK", (kCtX0 + kCtX1) * 0.5f,
                kCtTop - 0.13f - scrollY, kUiText * 1.8f, 0.5f, 0.5f, 0.55f);
        return 0.20f;
    }

    float yTop = kCtTop - scrollY;
    for (int i = 0; i < (int)servers.size(); i++) {
        float yBot = yTop - kRowH;
        // Skip rows outside the viewport
        if (yTop < kCtBot || yBot > kCtTop) {
            yTop = yBot - kRowGap;
            continue;
        }

        const auto &srv = servers[i];
        bool hot = (i == hoverItem);

        // Row background card (wiVRn style: dark card with slight blue tint)
        float bg[3] = {0.08f, 0.09f, 0.12f};
        if (hot) { bg[0] = 0.14f; bg[1] = 0.18f; bg[2] = 0.24f; }
        appendQuad(v, kCtX0, yTop, kCtX1, yBot, bg[0], bg[1], bg[2]);

        // Server name (large)
        uiTextL(v, srv.name.c_str(), kCtX0 + 0.03f, yTop - 0.03f,
                kUiText * 1.5f, kUiTitle[0], kUiTitle[1], kUiTitle[2]);

        // Hostname:port (small, dim)
        char hostport[128];
        snprintf(hostport, sizeof(hostport), "%s:%d", srv.hostname.c_str(), srv.port);
        uiTextL(v, hostport, kCtX0 + 0.03f, yTop - 0.075f,
                kUiText * 0.9f, 0.5f, 0.55f, 0.6f);

        // Autoconnect toggle (non-manual only)
        if (!srv.manual) {
            UiRect tog = { kCtX1 - 0.35f, yTop - kRowH * 0.5f, 0.25f, 0.05f };
            uiToggle(v, tog, "AUTO", srv.autoconnect, hot && connectHot != i);
        }

        // Connect button (green, wiVRn style)
        float btnW = 0.16f, btnH = 0.08f;
        UiRect btn = { kCtX1 - btnW * 0.5f - 0.03f, yTop - kRowH * 0.5f, btnW, btnH };
        bool btnHot = (connectHot == i);
        if (btnHot) {
            appendQuad(v, btn.cx - btn.w*0.5f, btn.cy + btn.h*0.5f,
                       btn.cx + btn.w*0.5f, btn.cy - btn.h*0.5f,
                       0.2f, 0.8f, 0.3f);
        } else {
            appendQuad(v, btn.cx - btn.w*0.5f, btn.cy + btn.h*0.5f,
                       btn.cx + btn.w*0.5f, btn.cy - btn.h*0.5f,
                       0.1f, 0.4f, 0.15f);
        }
        uiTextC(v, "CONNECT", btn.cx, btn.cy + 3.5f * kUiText * 0.85f,
                kUiText * 0.85f, 1.0f, 1.0f, 1.0f);

        yTop = yBot - kRowGap;
    }

    return serverContentHeight();
}

SrvHover hitServerContent(float cx, float cy, float scrollY) {
    SrvHover h;
    auto servers = getServerList();
    float yTop = kCtTop - scrollY;

    for (int i = 0; i < (int)servers.size(); i++) {
        float yBot = yTop - kRowH;

        // Connect button
        float btnW = 0.16f, btnH = 0.08f;
        UiRect btn = { kCtX1 - btnW * 0.5f - 0.03f, yTop - kRowH * 0.5f, btnW, btnH };
        if (uiHit(btn, cx, cy)) {
            h.item = i; h.part = 1; h.grab = true;
            return h;
        }

        // Autoconnect toggle
        if (!servers[i].manual) {
            UiRect tog = { kCtX1 - 0.35f, yTop - kRowH * 0.5f, 0.25f, 0.05f };
            if (uiHit(tog, cx, cy)) {
                h.item = i; h.part = 2; h.grab = true;
                return h;
            }
        }

        // Row body
        if (cx >= kCtX0 && cx <= kCtX1 && cy <= yTop && cy >= yBot) {
            h.item = i; h.part = 0; h.grab = false;
            return h;
        }

        yTop = yBot - kRowGap;
    }

    return h;
}

void applyServerClick(const SrvHover &h, bool click) {
    if (!click) return;
    if (h.item < 0) return;
    auto servers = getServerList();
    if (h.item >= (int)servers.size()) return;
    if (h.part == 1) {
        if (gOnServerConnect)
            gOnServerConnect(servers[h.item]);
    }
}
