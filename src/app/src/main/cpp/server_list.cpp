#include "server_list.h"
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

// Panel background + header + close button (same style as settings panel).
static void buildChrome(std::vector<float> &v, bool closeHover) {
    // Dark panel background (matches wiVRn's IM_COL32(8,8,8,224) ~ (0.03,0.03,0.03))
    appendQuad(v, kSrvPanelL, kSrvPanelTop, kSrvPanelR, kSrvPanelBot,
               0.03f, 0.03f, 0.03f);
    // Sidebar (left strip, darker)
    float sideR = kSrvPanelL + 0.24f;
    appendQuad(v, kSrvPanelL, kSrvPanelTop, sideR, kSrvPanelBot,
               0.01f, 0.01f, 0.01f);
    // Header bar
    appendQuad(v, sideR, kSrvPanelTop, kSrvPanelR, kSrvHdrY - 0.04f,
               0.05f, 0.05f, 0.06f);
    // Header text
    uiTextL(v, "SERVERS", sideR + 0.03f, kSrvHdrY, kUiText * 1.6f,
            kUiTitle[0], kUiTitle[1], kUiTitle[2]);
    // Close button
    uiButton(v, kSrvClose, "X", closeHover);
    // Sidebar label
    uiTextL(v, "WIVRN", kSrvPanelL + 0.02f, kSrvPanelTop - 0.06f,
            kUiText * 1.4f, kUiFill[0], kUiFill[1], kUiFill[2]);
}

void buildServerPanel(std::vector<float> &v, int hoverItem, bool closeHover, int connectHot) {
    buildChrome(v, closeHover);

    auto servers = getServerList();
    if (servers.empty()) {
        // "Start a WiVRn server on your local network" centered
        const char *msg = "START A WIVRN SERVER";
        uiTextC(v, msg, (kSrvPanelL + kSrvPanelR) * 0.5f, 0.05f,
                kUiText * 1.5f, 0.5f, 0.5f, 0.55f);
        const char *msg2 = "ON YOUR LOCAL NETWORK";
        uiTextC(v, msg2, (kSrvPanelL + kSrvPanelR) * 0.5f, -0.02f,
                kUiText * 1.5f, 0.5f, 0.5f, 0.55f);
        return;
    }

    float sideR = kSrvPanelL + 0.24f;
    float yTop = kSrvCtTop;
    float rowH = kSrvRowH;
    float gap = kSrvRowGap;

    for (int i = 0; i < (int)servers.size() && yTop - rowH > kSrvCtBot; i++) {
        const auto &srv = servers[i];
        float yBot = yTop - rowH;
        bool hot = (i == hoverItem);

        // Row background
        float bg[3] = {0.06f, 0.06f, 0.08f};
        if (hot) { bg[0] = 0.12f; bg[1] = 0.16f; bg[2] = 0.22f; }
        appendQuad(v, sideR + 0.02f, yTop, kSrvPanelR - 0.02f, yBot,
                   bg[0], bg[1], bg[2]);

        // Server name
        uiTextL(v, srv.name.c_str(), sideR + 0.05f, yTop - 0.025f,
                kUiText * 1.3f, kUiTitle[0], kUiTitle[1], kUiTitle[2]);

        // Hostname:port
        char hostport[128];
        snprintf(hostport, sizeof(hostport), "%s:%d", srv.hostname.c_str(), srv.port);
        uiTextL(v, hostport, sideR + 0.05f, yTop - 0.065f,
                kUiText * 0.9f, 0.5f, 0.55f, 0.6f);

        // Autoconnect toggle (for non-manual servers)
        if (!srv.manual) {
            UiRect tog = { kSrvPanelR - 0.30f, yTop - rowH * 0.5f, 0.22f, 0.05f };
            uiToggle(v, tog, "AUTO", srv.autoconnect, hot && (connectHot == i ? false : true));
        }

        // Connect button (green, wiVRn style)
        UiRect btn = { kSrvPanelR - 0.09f, yTop - rowH * 0.5f, 0.12f, 0.07f };
        bool btnHot = (connectHot == i);
        if (btnHot) {
            // Hovered: bright green
            appendQuad(v, btn.cx - btn.w*0.5f, btn.cy + btn.h*0.5f,
                       btn.cx + btn.w*0.5f, btn.cy - btn.h*0.5f,
                       0.2f, 0.8f, 0.3f);
        } else {
            // Normal: dim green
            appendQuad(v, btn.cx - btn.w*0.5f, btn.cy + btn.h*0.5f,
                       btn.cx + btn.w*0.5f, btn.cy - btn.h*0.5f,
                       0.1f, 0.4f, 0.15f);
        }
        uiTextC(v, "CONNECT", btn.cx, btn.cy + 3.5f * kUiText * 0.8f,
                kUiText * 0.8f, 1.0f, 1.0f, 1.0f);

        yTop = yBot - gap;
    }
}

SrvHover hitServerPanel(float cx, float cy) {
    SrvHover h;

    // Close button
    if (uiHit(kSrvClose, cx, cy)) {
        h.item = -2;  // close
        h.part = 0;
        h.grab = true;
        return h;
    }

    auto servers = getServerList();
    float sideR = kSrvPanelL + 0.24f;
    float yTop = kSrvCtTop;
    float rowH = kSrvRowH;
    float gap = kSrvRowGap;

    for (int i = 0; i < (int)servers.size() && yTop - rowH > kSrvCtBot; i++) {
        float yBot = yTop - rowH;

        // Connect button
        UiRect btn = { kSrvPanelR - 0.09f, yTop - rowH * 0.5f, 0.12f, 0.07f };
        if (uiHit(btn, cx, cy)) {
            h.item = i;
            h.part = 1;  // connect
            h.grab = true;
            return h;
        }

        // Autoconnect toggle (non-manual only)
        if (!servers[i].manual) {
            UiRect tog = { kSrvPanelR - 0.30f, yTop - rowH * 0.5f, 0.22f, 0.05f };
            if (uiHit(tog, cx, cy)) {
                h.item = i;
                h.part = 2;  // autoconnect
                h.grab = true;
                return h;
            }
        }

        // Row body
        if (cx >= sideR + 0.02f && cx <= kSrvPanelR - 0.02f &&
            cy <= yTop && cy >= yBot) {
            h.item = i;
            h.part = 0;
            h.grab = false;
            return h;
        }

        yTop = yBot - gap;
    }

    return h;
}

void applyServerClick(const SrvHover &h, bool click) {
    if (!click) return;
    if (h.item < 0) return;

    auto servers = getServerList();
    if (h.item >= (int)servers.size()) return;

    if (h.part == 1) {
        // Connect button
        if (gOnServerConnect)
            gOnServerConnect(servers[h.item]);
    }
    // part 2 (autoconnect toggle) would need persistence; skip for POC
}
