#pragma once
#include <GLES3/gl3.h>
#include <vector>

// Composites the ImGui offscreen texture as a 3D quad in world space.
// The quad is positioned at the same location as the settings panel.
// Uses a simple pos.xyz + uv.xy vertex format with an MVP matrix.

class ImGuiCompositor {
public:
    ImGuiCompositor() = default;
    ~ImGuiCompositor();

    // Compile the composite shader and create the VAO/VBO.
    bool init();

    // Draw the ImGui texture as a quad in world space.
    // mvp is the model-view-projection matrix for the panel.
    void draw(const float mvp[16], GLuint srcTex);

private:
    GLuint m_prog = 0;
    GLuint m_vao = 0;
    GLuint m_vbo = 0;
    GLint m_mvpLoc = -1;
    GLint m_texLoc = -1;
};

extern ImGuiCompositor gImGuiComposite;
