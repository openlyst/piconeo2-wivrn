#pragma once

// ImGui-based lobby UI. Replaces the old ui_kit immediate-mode widgets.
// Called from the render thread each frame between gImGui.newFrame()
// and gImGui.render().

// Build the ImGui UI for the current frame. This is called every frame
// from the render thread. It uses ImGui::Begin/End to create windows
// that match the old lobby layout: server list, settings, about.
void buildImGuiUI();

// Apply the WiVRn dark theme (dark background, blue accent) to ImGui.
void applyImGuiTheme();
