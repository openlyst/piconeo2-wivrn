#include "imgui_ui.h"
#include "third_party/imgui/imgui.h"
#include "log.h"

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

static void buildServerListTab()
{
    ImGui::Text("Server List");
    ImGui::Separator();
    ImGui::Text("No servers discovered.");
    ImGui::Spacing();
    if (ImGui::Button("Add Server", ImVec2(200, 50)))
        LOGI("imgui: add server clicked");
}

static void buildSettingsTab()
{
    ImGui::Text("Settings");
    ImGui::Separator();

    static bool passthrough = true;
    static bool foveated = false;
    static float brightness = 1.0f;
    static int quality = 1;

    ImGui::Checkbox("Passthrough", &passthrough);
    ImGui::Checkbox("Foveated Encoding", &foveated);
    ImGui::SliderFloat("Brightness", &brightness, 0.0f, 1.0f);
    ImGui::Combo("Quality", &quality, "Low\0Medium\0High\0\0");
}

static void buildAboutTab()
{
    ImGui::Text("About");
    ImGui::Separator();
    ImGui::TextWrapped("WiVRn OXR - Pico Neo 2 VR streaming client");
    ImGui::TextWrapped("Licensed under GNU AGPL v3");
    ImGui::Spacing();
    ImGui::TextDisabled("github.com/meumeu/wivrn");
}

void buildImGuiUI()
{
    applyImGuiTheme();

    // Full-screen window covering the entire UI surface.
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("##lobby", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Sidebar tabs
    static int currentTab = 0;
    const char *tabs[] = {"Server List", "Settings", "About"};
    const int numTabs = 3;

    // Left sidebar
    ImGui::BeginChild("##sidebar", ImVec2(250, 0), true);
    for (int i = 0; i < numTabs; i++) {
        bool selected = (currentTab == i);
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f, 0.52f, 0.88f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.52f, 0.88f, 1.0f));
        }
        if (ImGui::Button(tabs[i], ImVec2(-1, 60)))
            currentTab = i;
        if (selected) ImGui::PopStyleColor(2);
        ImGui::Spacing();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Main content area
    ImGui::BeginChild("##content", ImVec2(0, 0), true);
    switch (currentTab) {
        case 0: buildServerListTab(); break;
        case 1: buildSettingsTab(); break;
        case 2: buildAboutTab(); break;
    }
    ImGui::EndChild();

    ImGui::End();
}
