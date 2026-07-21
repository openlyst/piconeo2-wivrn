#pragma once
// wiVRn-style server list panel. Renders as native 3D geometry (no texture)
// using the ui_kit widget system. Panel-local metres, +x right / +y up.
#include <vector>
#include <string>
#include "ui_kit.h"

struct ServerInfo {
    std::string name;
    std::string hostname;
    int port = 0;
    bool tcp_only = false;
    bool manual = false;
    bool autoconnect = false;
};

// Set the discovered/known server list (thread-safe, called from JNI).
void setServerList(const std::vector<ServerInfo> & servers);

// Get the current server list (thread-safe).
std::vector<ServerInfo> getServerList();

// Connection request callback (set by render_thread).
#include <functional>
extern std::function<void(const ServerInfo &)> gOnServerConnect;

// Panel geometry (matches the settings panel size).
constexpr float kSrvPanelL = -0.66f, kSrvPanelR = 0.62f;
constexpr float kSrvPanelTop = 0.46f, kSrvPanelBot = -0.46f;
constexpr float kSrvHdrY = 0.40f;
constexpr UiRect kSrvClose = { 0.575f, 0.40f, 0.07f, 0.07f };

// Content viewport (same as settings panel).
constexpr float kSrvCtX0 = -0.42f, kSrvCtX1 = 0.60f;
constexpr float kSrvCtTop = 0.34f, kSrvCtBot = -0.42f;
constexpr float kSrvViewportH = kSrvCtTop - kSrvCtBot;

// Row height for each server entry.
constexpr float kSrvRowH = 0.12f;
constexpr float kSrvRowGap = 0.02f;

// Build the server list panel vertices.
// hoverItem: index of hovered server entry (-1 = none).
// closeHover: close button hovered.
// connectHot: which server's connect button is hovered.
void buildServerPanel(std::vector<float> &v, int hoverItem, bool closeHover, int connectHot);

// Hit-test the server list panel. Returns item index and sub-part.
// part: 0 = row body, 1 = connect button, 2 = autoconnect toggle
struct SrvHover { int item = -1; int part = 0; bool grab = false; };
SrvHover hitServerPanel(float cx, float cy);

// Apply a click on a server entry.
void applyServerClick(const SrvHover &h, bool click);
