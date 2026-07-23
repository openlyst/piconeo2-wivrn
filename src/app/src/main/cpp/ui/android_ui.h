#pragma once
#include <GLES3/gl3.h>

// Android View-based UI rendered to a bitmap in Java, uploaded to a GL
// texture here, and composited onto the 3D lobby panel.
//
// Replaces the ImGui manager + compositor. The Java side (VrUiPanel +
// VrUiController) handles all UI layout, input, and rendering. The native
// side just uploads the bitmap pixels to a texture and draws it.

class AndroidUi {
public:
    static constexpr int kUiW = 1400;
    static constexpr int kUiH = 1000;

    // Panel bounds in metres (same as the old ImGui panel)
    static constexpr float kPanelL = -0.80f;
    static constexpr float kPanelR =  0.62f;
    static constexpr float kPanelT =  0.50f;
    static constexpr float kPanelB = -0.50f;

    AndroidUi() = default;
    ~AndroidUi();

    void reset();
    bool init();

    // Upload pixel data from Java's Bitmap into the GL texture.
    // pixels must be kUiW * kUiH * 4 bytes (RGBA8888).
    void uploadPixels(const void *pixels);

    // Set touch state from controller raycast for Java dispatch.
    void setInput(float lx, float ly, bool pressed, bool onPanel, bool clickEdge);

    // Get the GL texture for compositing.
    GLuint texture() const { return m_tex; }

    // Draw the UI texture onto the 3D panel quad.
    void draw(const float mvp[16]);

    // Coordinate conversion helpers (metres <-> pixels)
    static float mToPxX(float m);
    static float mToPxY(float m);
    static float pxToMX(float px);
    static float pxToMY(float py);

private:
    GLuint m_tex = 0;
    GLuint m_fbo = 0;
    GLuint m_prog = 0;
    GLuint m_vao = 0;
    GLuint m_vbo = 0;
    GLint  m_mvpLoc = -1;
    GLint  m_texLoc = -1;

    // Touch state
    float m_touchX = -1, m_touchY = -1;
    bool  m_touchPressed = false;
    bool  m_touchClickEdge = false;
};
