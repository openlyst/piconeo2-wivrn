#include "simple_lobby.h"
#include "gl_util.h"
#include "log.h"
#include <cmath>
#include <vector>

static const char *kVtxSrc =
    "#version 300 es\n"
    "layout(location=0) in vec3 aPos;\n"
    "uniform mat4 uMVP;\n"
    "out float vDist;\n"
    "void main(){ vDist = length(aPos.xz); gl_Position = uMVP * vec4(aPos, 1.0); }\n";

static const char *kGridFragSrc =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in float vDist;\n"
    "uniform vec4 uColor;\n"
    "out vec4 frag;\n"
    "void main(){\n"
    "  float fade = 1.0 - clamp(vDist / 20.0, 0.0, 1.0);\n"
    "  frag = vec4(uColor.rgb, uColor.a * fade);\n"
    "}\n";

static const char *kSkyFragSrc =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in float vDist;\n"
    "uniform vec4 uColor;\n"
    "out vec4 frag;\n"
    "void main(){ frag = uColor; }\n";

simple_lobby::simple_lobby() {}
simple_lobby::~simple_lobby() {
    if (grid_vao_) glDeleteVertexArrays(1, &grid_vao_);
    if (grid_vbo_) glDeleteBuffers(1, &grid_vbo_);
    if (sky_vao_) glDeleteVertexArrays(1, &sky_vao_);
    if (sky_vbo_) glDeleteBuffers(1, &sky_vbo_);
    if (prog_) glDeleteProgram(prog_);
}

void simple_lobby::init() {
    if (inited_) return;

    prog_ = glCreateProgram();
    GLuint vs = compile(GL_VERTEX_SHADER, kVtxSrc);
    GLuint gfs = compile(GL_FRAGMENT_SHADER, kGridFragSrc);
    glAttachShader(prog_, vs);
    glAttachShader(prog_, gfs);
    glLinkProgram(prog_);
    GLint ok = 0;
    glGetProgramiv(prog_, GL_LINK_STATUS, &ok);
    if (!ok) { char log[512]; glGetProgramInfoLog(prog_, 512, nullptr, log); LOGE("simple_lobby link: %s", log); }
    glDeleteShader(vs);
    glDeleteShader(gfs);
    mvp_loc_ = glGetUniformLocation(prog_, "uMVP");
    color_loc_ = glGetUniformLocation(prog_, "uColor");

    buildGrid();
    buildSky();
    inited_ = true;
    LOGI("simple_lobby initialized");
}

void simple_lobby::buildGrid() {
    std::vector<float> verts;
    float halfSize = 20.0f;
    float step = 1.0f;
    int lines = (int)(halfSize / step) * 2 + 1;

    for (int i = 0; i < lines; i++) {
        float x = -halfSize + i * step;
        // line along Z
        verts.push_back(x); verts.push_back(0.0f); verts.push_back(-halfSize);
        verts.push_back(x); verts.push_back(0.0f); verts.push_back(halfSize);
        // line along X
        verts.push_back(-halfSize); verts.push_back(0.0f); verts.push_back(x);
        verts.push_back(halfSize); verts.push_back(0.0f); verts.push_back(x);
    }

    grid_vert_count_ = (int)(verts.size() / 3);

    glGenVertexArrays(1, &grid_vao_);
    glGenBuffers(1, &grid_vbo_);
    glBindVertexArray(grid_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, grid_vbo_);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void *)0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void simple_lobby::buildSky() {
    // Large inverted cube for the sky background
    float s = 50.0f;
    // 12 triangles (36 verts), inward-facing
    float cube[] = {
        // bottom (facing up)
        -s, -s, -s,   s, -s, -s,   s, -s,  s,
        -s, -s, -s,   s, -s,  s,  -s, -s,  s,
        // top (facing down)
        -s,  s, -s,  -s,  s,  s,   s,  s,  s,
        -s,  s, -s,   s,  s,  s,   s,  s, -s,
        // front (facing -Z)
        -s, -s,  s,   s, -s,  s,   s,  s,  s,
        -s, -s,  s,   s,  s,  s,  -s,  s,  s,
        // back (facing +Z)
        -s, -s, -s,  -s,  s, -s,   s,  s, -s,
        -s, -s, -s,   s,  s, -s,   s, -s, -s,
        // left (facing -X)
        -s, -s, -s,  -s, -s,  s,  -s,  s,  s,
        -s, -s, -s,  -s,  s,  s,  -s,  s, -s,
        // right (facing +X)
         s, -s, -s,   s,  s, -s,   s,  s,  s,
         s, -s, -s,   s,  s,  s,   s, -s,  s,
    };
    sky_vert_count_ = 36;

    glGenVertexArrays(1, &sky_vao_);
    glGenBuffers(1, &sky_vbo_);
    glBindVertexArray(sky_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, sky_vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(cube), cube, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void *)0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void simple_lobby::draw(const Mat4 &proj, const Mat4 &view) {
    if (!inited_) return;

    glUseProgram(prog_);

    // Sky: draw first, no depth write
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glUniformMatrix4fv(mvp_loc_, 1, GL_FALSE, proj.m);
    // sky only uses projection (infinite distance), but we still apply view
    // to keep it camera-relative. Strip translation from view so sky follows camera.
    Mat4 skyView = view;
    skyView.m[12] = 0; skyView.m[13] = 0; skyView.m[14] = 0;
    Mat4 skyMvp = mat4Mul(proj, skyView);
    glUniformMatrix4fv(mvp_loc_, 1, GL_FALSE, skyMvp.m);
    glUniform4f(color_loc_, 0.05f, 0.06f, 0.10f, 1.0f);
    glBindVertexArray(sky_vao_);
    glDrawArrays(GL_TRIANGLES, 0, sky_vert_count_);
    glBindVertexArray(0);

    // Floor grid
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);

    Mat4 mvp = mat4Mul(proj, view);
    glUniformMatrix4fv(mvp_loc_, 1, GL_FALSE, mvp.m);

    // major grid lines
    glUniform4f(color_loc_, 0.15f, 0.20f, 0.30f, 0.8f);
    glBindVertexArray(grid_vao_);
    glDrawArrays(GL_LINES, 0, grid_vert_count_);
    glBindVertexArray(0);

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
}
