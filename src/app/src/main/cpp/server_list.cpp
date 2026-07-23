#include "server_list.h"
#include "settings_panel.h"  // kCtX0, kCtX1, kCtTop, kCtBot
#include "pin_pad.h"         // gPinEntryRequested (cleared when connecting ends)
#include "ui_kit.h"
#include "font_atlas.h"
#include "log.h"
#include <mutex>
#include <cstring>

static std::mutex gSrvMutex;
static std::vector<ServerInfo> gServers;
static std::mutex gErrMutex;
static std::string gConnectionError;
static std::atomic<bool> gConnecting{false};

std::function<void(const ServerInfo &)> gOnServerConnect;
std::function<void(const std::string &hostname, int port)> gOnServerRemove;
std::function<void(const std::string &hostname, int port)> gOnServerAutoconnect;
std::function<void()> gOnRefreshServers;
std::function<void(bool connecting)> gOnConnectingChanged;

void setServerList(const std::vector<ServerInfo> & servers) {
    std::lock_guard<std::mutex> lk(gSrvMutex);
    gServers = servers;
}

std::vector<ServerInfo> getServerList() {
    std::lock_guard<std::mutex> lk(gSrvMutex);
    return gServers;
}

void setConnectionError(const std::string & msg) {
    std::lock_guard<std::mutex> lk(gErrMutex);
    gConnectionError = msg;
}

std::string getConnectionError() {
    std::lock_guard<std::mutex> lk(gErrMutex);
    return gConnectionError;
}

void clearConnectionError() {
    std::lock_guard<std::mutex> lk(gErrMutex);
    gConnectionError.clear();
}

void setConnecting(bool connecting) {
    gConnecting.store(connecting);
    if (gOnConnectingChanged)
        gOnConnectingChanged(connecting);
    if (!connecting)
        gPinEntryRequested.store(false);
}

bool isConnecting() {
    return gConnecting.load();
}

// Row layout constants (panel-local metres).
static constexpr float kRowH = 0.14f;
static constexpr float kRowGap = 0.02f;
static constexpr float kErrH = 0.06f;
static constexpr float kRefreshH = 0.10f;

float serverContentHeight() {
    auto servers = getServerList();
    float h = 0;
    if (servers.empty()) return 0.10f;
    h += servers.size() * kRowH + (servers.size() - 1) * kRowGap;
    auto err = getConnectionError();
    if (!err.empty()) h += kErrH + kRowGap;
    h += kRowGap + kRefreshH;
    return h;
}

float buildServerContent(std::vector<float> &v, float scrollY,
                         int hoverItem, int connectHot) {
    auto servers = getServerList();
    float contentW = kCtX1Content - kCtX0;

    if (servers.empty()) {
        uiTextC(v, "Start a WiVRn server", (kCtX0 + kCtX1Content) * 0.5f,
                kCtTop - 0.05f - scrollY, kUiText * 1.8f, 0.5f, 0.5f, 0.55f);
        uiTextC(v, "on your local network", (kCtX0 + kCtX1Content) * 0.5f,
                kCtTop - 0.13f - scrollY, kUiText * 1.8f, 0.5f, 0.5f, 0.55f);
        return 0.20f;
    }

    float yTop = kCtTop - scrollY;

    // Error banner at the top if there is one
    auto err = getConnectionError();
    if (!err.empty()) {
        float errBot = yTop - kErrH;
        if (yTop >= kCtBot && errBot <= kCtTop) {
            // red background
            appendQuad(v, kCtX0, yTop, kCtX1Content, errBot, 0.25f, 0.08f, 0.08f);
            // truncate long errors
            char buf[128];
            strncpy(buf, err.c_str(), sizeof(buf)-1);
            buf[sizeof(buf)-1] = 0;
            uiTextL(v, buf, kCtX0 + 0.02f, yTop - 0.02f,
                    kUiText * 0.9f, 1.0f, 0.7f, 0.7f);
        }
        yTop = errBot - kRowGap;
    }

    for (int i = 0; i < (int)servers.size(); i++) {
        float yBot = yTop - kRowH;
        // Skip rows outside the viewport
        if (yTop < kCtBot || yBot > kCtTop) {
            yTop = yBot - kRowGap;
            continue;
        }

        const auto &srv = servers[i];
        bool hot = (i == hoverItem);

        // Row background card
        float bg[3] = {0.08f, 0.09f, 0.12f};
        if (hot) { bg[0] = 0.14f; bg[1] = 0.18f; bg[2] = 0.24f; }
        appendQuad(v, kCtX0, yTop, kCtX1Content, yBot, bg[0], bg[1], bg[2]);

        // Server name (large)
        uiTextL(v, srv.name.c_str(), kCtX0 + 0.03f, yTop - 0.03f,
                kUiText * 1.5f, kUiTitle[0], kUiTitle[1], kUiTitle[2]);

        // Hostname:port (small, dim)
        char hostport[128];
        snprintf(hostport, sizeof(hostport), "%s:%d", srv.hostname.c_str(), srv.port);
        uiTextL(v, hostport, kCtX0 + 0.03f, yTop - 0.075f,
                kUiText * 0.9f, 0.5f, 0.55f, 0.6f);

        // Discovered badge (green dot + "Discovered" text)
        if (srv.discovered) {
            float badgeX = kCtX0 + 0.03f;
            // measure name width to place badge after it
            float nameW = gFont.textWidth(srv.name.c_str()) * (kUiText * 1.5f);
            badgeX += nameW + 0.04f;
            float badgeY = yTop - 0.035f;
            appendQuad(v, badgeX, badgeY + 0.01f, badgeX + 0.02f, badgeY - 0.01f,
                       0.2f, 0.7f, 0.3f);
            uiTextL(v, "Discovered", badgeX + 0.03f, badgeY,
                    kUiText * 0.7f, 0.3f, 0.7f, 0.4f);
        }

        // Layout right-to-left with small gaps: X | Connect | Auto
        // X button
        float xBtnW = 0.06f, xBtnH = 0.08f;
        float xBtnCx = kCtX1Content - xBtnW * 0.5f - 0.01f;
        float xBtnCy = yTop - kRowH * 0.5f;
        bool xHot = (hot && hoverItem == i && connectHot == -2);
        float xc[3] = {0.2f, 0.1f, 0.1f};
        if (xHot) { xc[0] = 0.5f; xc[1] = 0.2f; xc[2] = 0.2f; }
        appendQuad(v, xBtnCx - xBtnW*0.5f, xBtnCy + xBtnH*0.5f,
                   xBtnCx + xBtnW*0.5f, xBtnCy - xBtnH*0.5f,
                   xc[0], xc[1], xc[2]);
        uiTextC(v, "X", xBtnCx, xBtnCy + 3.5f * (kUiText * 0.85f),
                kUiText * 0.85f, 1, 1, 1);

        // Connect button (green, wiVRn style)
        float btnW = 0.16f, btnH = 0.08f;
        float btnCx = xBtnCx - xBtnW * 0.5f - 0.02f - btnW * 0.5f;
        UiRect btn = { btnCx, yTop - kRowH * 0.5f, btnW, btnH };
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
        // Show "Connecting..." if this is the active connection attempt
        const char *btnLabel = "Connect";
        if (isConnecting() && btnHot) btnLabel = "...";
        uiTextC(v, btnLabel, btn.cx, btn.cy + 3.5f * kUiText * 0.85f,
                kUiText * 0.85f, 1.0f, 1.0f, 1.0f);

        // Autoconnect toggle (non-manual only)
        if (!srv.manual) {
            float togCx = btnCx - btnW * 0.5f - 0.02f - 0.25f * 0.5f;
            UiRect tog = { togCx, yTop - kRowH * 0.5f, 0.25f, 0.05f };
            uiToggle(v, tog, "Auto", srv.autoconnect, hot && connectHot != i);
        }

        yTop = yBot - kRowGap;
    }

    // Refresh button at the bottom
    {
        float rBtnW = 0.20f, rBtnH = 0.08f;
        float rBtnCx = (kCtX0 + kCtX1Content) * 0.5f;
        float rBtnCy = yTop - rBtnH * 0.5f;
        bool rHot = (connectHot == -3);
        float rc[3] = {0.1f, 0.15f, 0.2f};
        if (rHot) { rc[0] = 0.15f; rc[1] = 0.25f; rc[2] = 0.35f; }
        appendQuad(v, rBtnCx - rBtnW*0.5f, rBtnCy + rBtnH*0.5f,
                   rBtnCx + rBtnW*0.5f, rBtnCy - rBtnH*0.5f,
                   rc[0], rc[1], rc[2]);
        uiTextC(v, "Refresh", rBtnCx, rBtnCy + 3.5f * kUiText * 0.85f,
                kUiText * 0.85f, 0.7f, 0.8f, 1.0f);
    }

    return serverContentHeight();
}

SrvHover hitServerContent(float cx, float cy, float scrollY) {
    SrvHover h;
    auto servers = getServerList();
    float yTop = kCtTop - scrollY;

    // Skip error banner
    auto err = getConnectionError();
    if (!err.empty()) {
        yTop -= kErrH + kRowGap;
    }

    for (int i = 0; i < (int)servers.size(); i++) {
        float yBot = yTop - kRowH;

        // Layout right-to-left: X | Connect | Auto
        float xBtnW = 0.06f, xBtnH = 0.08f;
        float xBtnCx = kCtX1Content - xBtnW * 0.5f - 0.01f;
        float xBtnCy = yTop - kRowH * 0.5f;

        // X button
        if (cx >= xBtnCx - xBtnW*0.5f && cx <= xBtnCx + xBtnW*0.5f &&
            cy >= xBtnCy - xBtnH*0.5f && cy <= xBtnCy + xBtnH*0.5f) {
            h.item = i; h.part = 3; h.grab = true;
            return h;
        }

        // Connect button
        float btnW = 0.16f, btnH = 0.08f;
        float btnCx = xBtnCx - xBtnW * 0.5f - 0.02f - btnW * 0.5f;
        UiRect btn = { btnCx, yTop - kRowH * 0.5f, btnW, btnH };
        if (uiHit(btn, cx, cy)) {
            h.item = i; h.part = 1; h.grab = true;
            return h;
        }

        // Autoconnect toggle
        if (!servers[i].manual) {
            float togCx = btnCx - btnW * 0.5f - 0.02f - 0.25f * 0.5f;
            UiRect tog = { togCx, yTop - kRowH * 0.5f, 0.25f, 0.05f };
            if (uiHit(tog, cx, cy)) {
                h.item = i; h.part = 2; h.grab = true;
                return h;
            }
        }

        // Row body
        if (cx >= kCtX0 && cx <= kCtX1Content && cy <= yTop && cy >= yBot) {
            h.item = i; h.part = 0; h.grab = false;
            return h;
        }

        yTop = yBot - kRowGap;
    }

    // Refresh button
    {
        float rBtnW = 0.20f, rBtnH = 0.08f;
        float rBtnCx = (kCtX0 + kCtX1Content) * 0.5f;
        float rBtnCy = yTop - rBtnH * 0.5f;
        if (cx >= rBtnCx - rBtnW*0.5f && cx <= rBtnCx + rBtnW*0.5f &&
            cy >= rBtnCy - rBtnH*0.5f && cy <= rBtnCy + rBtnH*0.5f) {
            h.item = -1; h.part = 4; h.grab = true;
            return h;
        }
    }

    return h;
}

void applyServerClick(const SrvHover &h, bool click) {
    if (!click) return;

    // Refresh button (item = -1, part = 4)
    if (h.part == 4) {
        if (gOnRefreshServers)
            gOnRefreshServers();
        return;
    }

    if (h.item < 0) return;
    auto servers = getServerList();
    if (h.item >= (int)servers.size()) return;
    const auto &srv = servers[h.item];

    if (h.part == 1) {
        clearConnectionError();
        if (gOnServerConnect)
            gOnServerConnect(srv);
    } else if (h.part == 2) {
        if (gOnServerAutoconnect)
            gOnServerAutoconnect(srv.hostname, srv.port);
    } else if (h.part == 3) {
        if (gOnServerRemove)
            gOnServerRemove(srv.hostname, srv.port);
    }
}
