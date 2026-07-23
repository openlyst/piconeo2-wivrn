#pragma once
#include <GLES3/gl3.h>
#include <string>

// ImGui manager: renders ImGui UI to an offscreen texture, then the render
// thread composites it as a 3D quad in world space (same position as the
// existing settings panel). Input comes from the controller raycast mapped
// to ImGui's 2D pixel coordinate system.

class ImGuiManager {
public:
    static constexpr int kUiW = 1400;   // UI surface width in pixels
    static constexpr int kUiH = 1000;   // UI surface height in pixels

    ImGuiManager() = default;
    ~ImGuiManager();

    // Release all GL resources and ImGui state. Called before a relaunch
    // re-inits with a fresh EGL context.
    void reset();

    // Initialize ImGui + GLES3 backend + offscreen FBO. Call once from the
    // render thread after GL context is created.
    bool init();

    // Feed input from controller raycast. lx/ly are panel-local metres,
    // pressed is the trigger/OK held state. onPanel indicates the ray hit
    // the panel. clickEdge is a latched press edge (gOkClick) that may
    // fire even if pressed is already false by the time we poll.
    void setInput(float lx, float ly, bool pressed, bool onPanel, bool clickEdge);

    // Begin a new ImGui frame. Call this before any ImGui:: calls.
    void newFrame();

    // Render ImGui draw data into the offscreen FBO. Call after ImGui::Render().
    void render();

    // The offscreen texture that ImGui rendered into. Bind this when
    // compositing the UI quad into the eye texture.
    GLuint texture() const { return m_tex; }

    // Panel bounds in metres (matches settings_panel.h kSetPanelL/R/Top/Bot).
    static constexpr float kPanelL = -0.80f;
    static constexpr float kPanelR = 0.62f;
    static constexpr float kPanelT = 0.50f;
    static constexpr float kPanelB = -0.50f;

    // Convert panel-local metres to UI pixels.
    static float mToPxX(float m) { return (m - kPanelL) / (kPanelR - kPanelL) * kUiW; }
    static float mToPxY(float m) { return (kPanelT - m) / (kPanelT - kPanelB) * kUiH; }

    // Convert UI pixels to panel-local metres.
    static float pxToMX(float px) { return kPanelL + px / kUiW * (kPanelR - kPanelL); }
    static float pxToMY(float py) { return kPanelT - py / kUiH * (kPanelT - kPanelB); }

private:
    GLuint m_fbo = 0;
    GLuint m_tex = 0;
    bool m_initialized = false;
};

extern ImGuiManager gImGui;
