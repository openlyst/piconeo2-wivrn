#include "imgui_compositor.h"
#include "imgui_manager.h"
#include "log.h"
#include "gl_util.h"   // compile()

ImGuiCompositor gImGuiComposite;

static const char *kCompositeVS =
    "#version 300 es\n"
    "layout(location=0) in vec3 aPos;\n"
    "layout(location=1) in vec2 aUV;\n"
    "uniform mat4 uMVP;\n"
    "out vec2 vUV;\n"
    "void main(){ vUV=aUV; gl_Position=uMVP*vec4(aPos,1.0); }\n";

static const char *kCompositeFS =
    "#version 300 es\n"
    "precision mediump float;\n"
    "uniform sampler2D uTex;\n"
    "in vec2 vUV; out vec4 oColor;\n"
    "void main(){ oColor=texture(uTex,vUV); }\n";

ImGuiCompositor::~ImGuiCompositor()
{
    if (m_prog) glDeleteProgram(m_prog);
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
}

bool ImGuiCompositor::init()
{
    m_prog = glCreateProgram();
    GLuint vs = compile(GL_VERTEX_SHADER, kCompositeVS);
    GLuint fs = compile(GL_FRAGMENT_SHADER, kCompositeFS);
    glAttachShader(m_prog, vs);
    glAttachShader(m_prog, fs);
    glLinkProgram(m_prog);
    GLint ok = 0;
    glGetProgramiv(m_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(m_prog, 512, nullptr, log);
        LOGE("ImGui composite shader link failed: %s", log);
        return false;
    }
    m_mvpLoc = glGetUniformLocation(m_prog, "uMVP");
    m_texLoc = glGetUniformLocation(m_prog, "uTex");

    // Build a quad covering the panel area in panel-local metres.
    // pos.xyz + uv.xy = 5 floats per vertex.
    // UV (0,0) = top-left, (1,1) = bottom-right (matching ImGui texture).
    float verts[] = {
        // pos.x, pos.y, pos.z, u, v
        ImGuiManager::kPanelL, ImGuiManager::kPanelT, 0, 0, 0,   // top-left
        ImGuiManager::kPanelL, ImGuiManager::kPanelB, 0, 0, 1,   // bottom-left
        ImGuiManager::kPanelR, ImGuiManager::kPanelB, 0, 1, 1,   // bottom-right
        ImGuiManager::kPanelL, ImGuiManager::kPanelT, 0, 0, 0,   // top-left
        ImGuiManager::kPanelR, ImGuiManager::kPanelB, 0, 1, 1,   // bottom-right
        ImGuiManager::kPanelR, ImGuiManager::kPanelT, 0, 1, 0,   // top-right
    };

    glGenVertexArrays(1, &m_vao);
    glBindVertexArray(m_vao);
    glGenBuffers(1, &m_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(3 * sizeof(float)));
    glBindVertexArray(0);

    LOGI("ImGui compositor initialized prog=%u", m_prog);
    return true;
}

void ImGuiCompositor::draw(const float mvp[16], GLuint srcTex)
{
    glUseProgram(m_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, srcTex);
    glUniform1i(m_texLoc, 0);
    glUniformMatrix4fv(m_mvpLoc, 1, GL_FALSE, mvp);
    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
}
