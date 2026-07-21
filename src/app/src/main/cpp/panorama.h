#pragma once
// Equirectangular panorama background for the lobby. Renders a textured sphere
// around the viewer using the head pose orientation.
#include <GLES3/gl3.h>
#include <string>

class panorama_bg {
    GLuint program = 0;
    GLint  mvp_loc = -1;
    GLint  tex_loc = -1;
    GLuint vao = 0, vbo = 0, ibo = 0;
    int    index_count = 0;
    GLuint texture = 0;
    int    tex_w = 0, tex_h = 0;
    bool   gl_ready = false;
    bool   has_texture = false;
    std::string current_path;

    void build_shader();
    void build_sphere();
    void upload_texture(const std::string &path);

public:
    void init();
    // Load (or reload) a panorama from a file path. If path is empty, clears.
    void load(const std::string &path);
    // Draw as background using the current view matrix (no translation).
    void draw(const float *proj, const float *view);
    bool is_ready() const { return gl_ready; }
    bool has_tex() const { return has_texture; }
};

extern panorama_bg *gPanorama;
