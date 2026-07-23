#include "imgui_manager.h"
#include "log.h"
#include "third_party/imgui/imgui.h"
#include "third_party/imgui/imgui_impl_gles3.h"
#include <vector>

// Font data loaded from APK assets by jni.cpp.
extern std::vector<unsigned char> gFontData;
extern std::vector<unsigned char> gFontDataBold;

ImGuiManager gImGui;

ImGuiManager::~ImGuiManager()
{
    if (m_initialized) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui::DestroyContext();
    }
    if (m_fbo) glDeleteFramebuffers(1, &m_fbo);
    if (m_tex) glDeleteTextures(1, &m_tex);
}

bool ImGuiManager::init()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;   // don't write imgui.ini
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    // Load Roboto TTF at a size readable in VR. The font data is loaded
    // from APK assets by jni.cpp::nativeStart. Fall back to the default
    // bitmap font if the TTF isn't available.
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    cfg.PixelSnapH = false;

    float fontSize = 42.0f;
    if (!gFontData.empty()) {
        io.Fonts->AddFontFromMemoryTTF(gFontData.data(), (int)gFontData.size(),
                                       fontSize, &cfg);
        if (!gFontDataBold.empty()) {
            cfg.MergeMode = true;
            io.Fonts->AddFontFromMemoryTTF(gFontDataBold.data(), (int)gFontDataBold.size(),
                                           fontSize, &cfg);
            cfg.MergeMode = false;
        }
    } else {
        LOGE("ImGui: TTF font data not available, falling back to default");
        io.Fonts->AddFontDefault();
    }
    io.FontGlobalScale = 1.0f;

    if (!ImGui_ImplOpenGL3_Init("#version 300 es")) {
        LOGE("ImGui_ImplOpenGL3_Init failed");
        return false;
    }

    // Build the font atlas texture via ImGui's backend.
    ImGui_ImplOpenGL3_CreateFontsTexture();

    // Create offscreen FBO for rendering ImGui.
    glGenTextures(1, &m_tex);
    glBindTexture(GL_TEXTURE_2D, m_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kUiW, kUiH, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &m_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, m_tex, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("ImGui FBO incomplete: 0x%x", st);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Clear the texture to transparent.
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glViewport(0, 0, kUiW, kUiH);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    m_initialized = true;
    LOGI("ImGui initialized: %dx%d offscreen FBO=%u tex=%u",
         kUiW, kUiH, m_fbo, m_tex);
    return true;
}

void ImGuiManager::setInput(float lx, float ly, bool pressed, bool onPanel)
{
    ImGuiIO &io = ImGui::GetIO();
    if (onPanel) {
        io.AddMousePosEvent(mToPxX(lx), mToPxY(ly));
    } else {
        io.AddMousePosEvent(-1, -1);  // off-screen
    }
    io.AddMouseButtonEvent(0, pressed);
}

void ImGuiManager::newFrame()
{
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)kUiW, (float)kUiH);
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    // Delta time - use a reasonable default; ImGui doesn't need precise timing.
    static double lastTime = 0.0;
    double now = ImGui::GetTime();
    io.DeltaTime = (float)(now - lastTime);
    lastTime = now;
    if (io.DeltaTime <= 0.0f) io.DeltaTime = 1.0f / 60.0f;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
}

void ImGuiManager::render()
{
    ImGui::Render();
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glViewport(0, 0, kUiW, kUiH);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
