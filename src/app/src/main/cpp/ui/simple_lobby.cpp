#include "simple_lobby.h"
#include "gl_util.h"
#include "log.h"
#include <cmath>
#include <ctime>
#include <vector>

// Grid shader: flat color with distance fade
static const char *kGridVtx =
    "#version 300 es\n"
    "layout(location=0) in vec3 aPos;\n"
    "uniform mat4 uMVP;\n"
    "out float vDist;\n"
    "void main(){ vDist = length(aPos.xz); gl_Position = uMVP * vec4(aPos, 1.0); }\n";

static const char *kGridFrag =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in float vDist;\n"
    "uniform vec4 uColor;\n"
    "out vec4 frag;\n"
    "void main(){\n"
    "  float fade = 1.0 - clamp(vDist / 25.0, 0.0, 1.0);\n"
    "  frag = vec4(uColor.rgb, uColor.a * fade);\n"
    "}\n";

// Sky shader: vertical gradient + sun/moon disc
static const char *kSkyVtx =
    "#version 300 es\n"
    "layout(location=0) in vec3 aPos;\n"
    "uniform mat4 uMVP;\n"
    "out vec3 vDir;\n"
    "void main(){\n"
    "  vDir = normalize(aPos);\n"
    "  gl_Position = uMVP * vec4(aPos, 1.0);\n"
    "}\n";

static const char *kSkyFrag =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec3 vDir;\n"
    "uniform vec3 uSkyTop;\n"
    "uniform vec3 uSkyBot;\n"
    "uniform vec3 uSunDir;\n"
    "uniform vec3 uSunColor;\n"
    "uniform float uSunSize;\n"
    "out vec4 frag;\n"
    "void main(){\n"
    "  float t = clamp(vDir.y * 0.5 + 0.5, 0.0, 1.0);\n"
    "  vec3 sky = mix(uSkyBot, uSkyTop, t);\n"
    "  float d = max(dot(normalize(vDir), normalize(uSunDir)), 0.0);\n"
    "  float disc = smoothstep(1.0 - uSunSize, 1.0 - uSunSize * 0.5, d);\n"
    "  float glow = pow(d, 8.0) * 0.3;\n"
    "  sky += uSunColor * disc;\n"
    "  sky += uSunColor * glow;\n"
    "  frag = vec4(sky, 1.0);\n"
    "}\n";

simple_lobby::simple_lobby() {}

simple_lobby::~simple_lobby() {
    if (grid_vao_) glDeleteVertexArrays(1, &grid_vao_);
    if (grid_vbo_) glDeleteBuffers(1, &grid_vbo_);
    if (sky_vao_) glDeleteVertexArrays(1, &sky_vao_);
    if (sky_vbo_) glDeleteBuffers(1, &sky_vbo_);
    if (prog_) glDeleteProgram(prog_);
    if (sky_prog_) glDeleteProgram(sky_prog_);
}

void simple_lobby::init() {
    if (inited_) return;

    // grid program
    prog_ = glCreateProgram();
    GLuint vs = compile(GL_VERTEX_SHADER, kGridVtx);
    GLuint gfs = compile(GL_FRAGMENT_SHADER, kGridFrag);
    glAttachShader(prog_, vs);
    glAttachShader(prog_, gfs);
    glLinkProgram(prog_);
    GLint ok = 0;
    glGetProgramiv(prog_, GL_LINK_STATUS, &ok);
    if (!ok) { char log[512]; glGetProgramInfoLog(prog_, 512, nullptr, log); LOGE("simple_lobby grid link: %s", log); }
    glDeleteShader(vs);
    glDeleteShader(gfs);
    mvp_loc_ = glGetUniformLocation(prog_, "uMVP");
    color_loc_ = glGetUniformLocation(prog_, "uColor");

    // sky program
    sky_prog_ = glCreateProgram();
    GLuint svs = compile(GL_VERTEX_SHADER, kSkyVtx);
    GLuint sfs = compile(GL_FRAGMENT_SHADER, kSkyFrag);
    glAttachShader(sky_prog_, svs);
    glAttachShader(sky_prog_, sfs);
    glLinkProgram(sky_prog_);
    glGetProgramiv(sky_prog_, GL_LINK_STATUS, &ok);
    if (!ok) { char log[512]; glGetProgramInfoLog(sky_prog_, 512, nullptr, log); LOGE("simple_lobby sky link: %s", log); }
    glDeleteShader(svs);
    glDeleteShader(sfs);
    sky_mvp_loc_ = glGetUniformLocation(sky_prog_, "uMVP");
    sky_dir_loc_ = glGetUniformLocation(sky_prog_, "uSunDir");
    sky_top_loc_ = glGetUniformLocation(sky_prog_, "uSkyTop");
    sky_bot_loc_ = glGetUniformLocation(sky_prog_, "uSkyBot");
    sky_sun_loc_ = glGetUniformLocation(sky_prog_, "uSunColor");
    sky_sun_col_loc_ = sky_sun_loc_;
    sky_sun_size_loc_ = glGetUniformLocation(sky_prog_, "uSunSize");

    buildGrid();
    buildSky();
    inited_ = true;
    LOGI("simple_lobby initialized");
}

void simple_lobby::buildGrid() {
    std::vector<float> verts;
    float halfSize = 25.0f;
    float step = 1.0f;
    int lines = (int)(halfSize / step) * 2 + 1;

    for (int i = 0; i < lines; i++) {
        float x = -halfSize + i * step;
        verts.push_back(x); verts.push_back(0.0f); verts.push_back(-halfSize);
        verts.push_back(x); verts.push_back(0.0f); verts.push_back(halfSize);
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
    float s = 50.0f;
    float cube[] = {
        -s, -s, -s,   s, -s, -s,   s, -s,  s,
        -s, -s, -s,   s, -s,  s,  -s, -s,  s,
        -s,  s, -s,  -s,  s,  s,   s,  s,  s,
        -s,  s, -s,   s,  s,  s,   s,  s, -s,
        -s, -s,  s,   s, -s,  s,   s,  s,  s,
        -s, -s,  s,   s,  s,  s,  -s,  s,  s,
        -s, -s, -s,  -s,  s, -s,   s,  s, -s,
        -s, -s, -s,   s,  s, -s,   s, -s, -s,
        -s, -s, -s,  -s, -s,  s,  -s,  s,  s,
        -s, -s, -s,  -s,  s,  s,  -s,  s, -s,
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

void simple_lobby::updateTimeOfDay() {
    time_t now = time(nullptr);
    struct tm *lt = localtime(&now);
    float hour = lt->tm_hour + lt->tm_min / 60.0f;

    // sun angle: at 6am sun rises in the east (+X), peaks at noon (top),
    // sets at 6pm in the west (-X). At night the moon follows the same path
    // offset by 12 hours.
    float dayFrac;
    if (hour >= 6.0f && hour < 18.0f) {
        // daytime
        is_night_ = false;
        dayFrac = (hour - 6.0f) / 12.0f;  // 0 at sunrise, 1 at sunset
    } else {
        is_night_ = true;
        float nightHour = (hour < 6.0f) ? hour + 6.0f : hour - 18.0f;
        dayFrac = nightHour / 12.0f;
    }

    // sun/moon travels in an arc from east to west
    float angle = dayFrac * 3.14159f;  // 0 to pi
    float sx = cosf(angle);
    float sy = sinf(angle);
    // normalize
    float sl = sqrtf(sx * sx + sy * sy);
    if (sl > 1e-5f) { sx /= sl; sy /= sl; }

    sun_dir_[0] = sx;
    sun_dir_[1] = sy;
    sun_dir_[2] = 0.0f;

    if (is_night_) {
        // night: moon, dark blue sky
        sun_color_[0] = 0.8f; sun_color_[1] = 0.85f; sun_color_[2] = 1.0f;
        sun_size_ = 0.025f;
        sky_top_[0] = 0.02f; sky_top_[1] = 0.03f; sky_top_[2] = 0.08f;
        sky_bot_[0] = 0.05f; sky_bot_[1] = 0.06f; sky_bot_[2] = 0.12f;
        grid_color_[0] = 0.08f; grid_color_[1] = 0.10f; grid_color_[2] = 0.18f;
    } else {
        // day: sun, blue sky. color shifts with sun height.
        float h = sy;  // 0 at horizon, 1 at zenith
        sun_color_[0] = 1.0f; sun_color_[1] = 0.9f; sun_color_[2] = 0.7f;
        sun_size_ = 0.035f;
        // sky brighter when sun is high
        sky_top_[0] = 0.1f + 0.2f * h;  sky_top_[1] = 0.3f + 0.2f * h;  sky_top_[2] = 0.6f + 0.2f * h;
        sky_bot_[0] = 0.4f + 0.2f * h;  sky_bot_[1] = 0.6f + 0.2f * h;  sky_bot_[2] = 0.85f + 0.1f * h;
        // warm horizon near sunrise/sunset
        float horizon = 1.0f - h;
        sky_bot_[0] += horizon * 0.2f;
        sky_bot_[1] -= horizon * 0.1f;
        grid_color_[0] = 0.15f; grid_color_[1] = 0.25f; grid_color_[2] = 0.35f;
    }
}

void simple_lobby::draw(const Mat4 &proj, const Mat4 &view) {
    if (!inited_) return;

    updateTimeOfDay();

    // Sky
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);

    glUseProgram(sky_prog_);
    Mat4 skyView = view;
    skyView.m[12] = 0; skyView.m[13] = 0; skyView.m[14] = 0;
    Mat4 skyMvp = mat4Mul(proj, skyView);
    glUniformMatrix4fv(sky_mvp_loc_, 1, GL_FALSE, skyMvp.m);
    glUniform3fv(sky_dir_loc_, 1, sun_dir_);
    glUniform3fv(sky_top_loc_, 1, sky_top_);
    glUniform3fv(sky_bot_loc_, 1, sky_bot_);
    glUniform3fv(sky_sun_col_loc_, 1, sun_color_);
    glUniform1f(sky_sun_size_loc_, sun_size_);
    glBindVertexArray(sky_vao_);
    glDrawArrays(GL_TRIANGLES, 0, sky_vert_count_);
    glBindVertexArray(0);

    glDepthMask(GL_TRUE);

    // Floor grid
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);

    glUseProgram(prog_);
    Mat4 mvp = mat4Mul(proj, view);
    glUniformMatrix4fv(mvp_loc_, 1, GL_FALSE, mvp.m);
    glUniform4f(color_loc_, grid_color_[0], grid_color_[1], grid_color_[2], 0.7f);
    glBindVertexArray(grid_vao_);
    glDrawArrays(GL_LINES, 0, grid_vert_count_);
    glBindVertexArray(0);

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
}
