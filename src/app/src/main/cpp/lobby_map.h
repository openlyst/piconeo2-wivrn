#pragma once
#include <GLES3/gl3.h>
#include "math3d.h"

class lobby_map {
public:
    lobby_map();
    ~lobby_map();
    void load(const char *obj_path);
    void draw(const Mat4 &proj, const Mat4 &view);
    bool is_loaded() const { return loaded_; }
    void set_model(const Mat4 &m) { model_ = m; }

private:
    bool loaded_ = false;
    GLuint prog_ = 0;
    GLint  mvp_loc_ = -1;
    GLint  model_loc_ = -1;
    GLint  normal_mat_loc_ = -1;
    GLint  light_dir_loc_ = -1;
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    int    vert_count_ = 0;
    Mat4   model_;
};
