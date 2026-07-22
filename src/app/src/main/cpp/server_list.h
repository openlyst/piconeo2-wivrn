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
    bool discovered = false;
};

void setServerList(const std::vector<ServerInfo> & servers);
std::vector<ServerInfo> getServerList();

// Set/get the last connection error message (shown in the server list UI).
void setConnectionError(const std::string & msg);
std::string getConnectionError();
void clearConnectionError();

// Set/get whether we are currently attempting to connect.
void setConnecting(bool connecting);
bool isConnecting();

extern std::function<void(const ServerInfo &)> gOnServerConnect;
extern std::function<void(const std::string &hostname, int port)> gOnServerRemove;
extern std::function<void(const std::string &hostname, int port)> gOnServerAutoconnect;
extern std::function<void()> gOnRefreshServers;

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
// part: 0 = row, 1 = connect, 2 = autoconnect toggle, 3 = remove (X), 4 = refresh
