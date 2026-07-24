#pragma once
#include <GLES3/gl3.h>
#include <string>
#include <vector>
#include "server_list.h"

// Android View-based UI rendered to a bitmap in Java, uploaded to a GL
// texture here, and composited onto the 3D lobby panel.
//
// Replaces the ImGui manager + compositor. The Java side (VrUiPanel +
// VrUiController) handles all UI layout, input, and rendering. The native
// side just uploads the bitmap pixels to a texture and draws it.

// JNI bridge functions (defined in jni.cpp) for pushing data to Java.
void androidUiPushTouch(float x, float y, bool pressed, bool clickEdge, float stickY);
void androidUiPushServers(const std::vector<ServerInfo> &servers);
void androidUiPushConnecting(bool connecting);
void androidUiPushConnError(const std::string &err);
void androidUiPushStreaming(bool streaming);
void androidUiPushBattery(int hmdBatt, int leftBatt, bool leftConn, int rightBatt, bool rightConn);
void androidUiPushSettings();
void androidUiPushDiag(int mode, const float *pipeline, const float *system);

// Fetch pixels from Java's VrUiPanel and upload to the AndroidUi texture.
// Called from the render thread each frame.
void androidUiFetchAndUpload();

class AndroidUi {
public:
    static constexpr int kUiW = 1400;
    static constexpr int kUiH = 900;

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

    // Staging buffer: copy pixels here before GL upload so the Java
    // ByteBuffer can be safely reused by the render thread.
    std::vector<uint8_t> m_staging;
};
