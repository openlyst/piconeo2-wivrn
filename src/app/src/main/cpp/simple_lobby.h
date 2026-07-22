#pragma once
#include <GLES3/gl3.h>
#include "math3d.h"

class simple_lobby {
public:
    simple_lobby();
    ~simple_lobby();
    void init();
    void draw(const Mat4 &proj, const Mat4 &view);

private:
    GLuint prog_ = 0;
    GLint  mvp_loc_ = -1;
    GLint  color_loc_ = -1;
    GLuint grid_vao_ = 0;
    GLuint grid_vbo_ = 0;
    int    grid_vert_count_ = 0;
    GLuint sky_vao_ = 0;
    GLuint sky_vbo_ = 0;
    int    sky_vert_count_ = 0;
    bool   inited_ = false;

    void buildGrid();
    void buildSky();
};
