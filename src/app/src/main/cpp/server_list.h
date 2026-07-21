#pragma once
// wiVRn-style server list. Renders as native 3D geometry inside the unified
// lobby panel's content area. Panel-local metres, +x right / +y up.
#include <vector>
#include <string>
#include <functional>
#include "ui_kit.h"

struct ServerInfo {
    std::string name;
    std::string hostname;
    int port = 0;
    bool tcp_only = false;
    bool manual = false;
    bool autoconnect = false;
};

void setServerList(const std::vector<ServerInfo> & servers);
std::vector<ServerInfo> getServerList();

extern std::function<void(const ServerInfo &)> gOnServerConnect;

// Build server list rows inside the panel content area.
// Origin is top-left of content area (kCtX0, kCtTop). Scrolls vertically.
// scrollY: current scroll offset in metres (positive = scrolled down).
// hoverItem: hovered server index (-1 = none).
// connectHot: which server's connect button is hovered (-1 = none).
// Returns total content height in metres.
float buildServerContent(std::vector<float> &v, float scrollY,
                         int hoverItem, int connectHot);

// Hit-test server list content. cx/cy are panel-local coords.
// scrollY is the current scroll offset.
struct SrvHover { int item = -1; int part = 0; bool grab = false; };
SrvHover hitServerContent(float cx, float cy, float scrollY);

// Total height of all server rows.
float serverContentHeight();

// Apply a click on a server entry.
void applyServerClick(const SrvHover &h, bool click);
