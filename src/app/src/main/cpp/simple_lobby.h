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
    GLuint sky_prog_ = 0;
    GLint  sky_mvp_loc_ = -1;
    GLint  sky_dir_loc_ = -1;
    GLint  sky_top_loc_ = -1;
    GLint  sky_bot_loc_ = -1;
    GLint  sky_sun_loc_ = -1;
    GLint  sky_sun_col_loc_ = -1;
    GLint  sky_sun_size_loc_ = -1;
    GLuint grid_vao_ = 0;
    GLuint grid_vbo_ = 0;
    int    grid_vert_count_ = 0;
    GLuint sky_vao_ = 0;
    GLuint sky_vbo_ = 0;
    int    sky_vert_count_ = 0;
    bool   inited_ = false;

    // day/night state, refreshed each draw
    bool   is_night_ = false;
    float  sun_dir_[3] = {0, 1, 0};
    float  sun_color_[3] = {1, 1, 1};
    float  sun_size_ = 0.04f;
    float  sky_top_[3] = {0.1f, 0.2f, 0.5f};
    float  sky_bot_[3] = {0.5f, 0.7f, 0.9f};
    float  grid_color_[3] = {0.15f, 0.20f, 0.30f};

    void buildGrid();
    void buildSky();
    void updateTimeOfDay();
};
