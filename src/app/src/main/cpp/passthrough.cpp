#include "passthrough.h"
#include "log.h"
#include <cstring>
#include <cmath>
#include <vector>
#include <cstdio>

// Vertex shader: positions are already in clip space (pre-computed from
// the fisheye mesh tangent-space coords divided by tan(fov/2)).
static const char *vert_src = R"(#version 310 es
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
out vec2 v_uv;
void main()
{
    gl_Position = vec4(a_pos, 0.0, 1.0);
    v_uv = a_uv;
}
)";

// Fragment shader: the SDK delivers RGBA frames, so we sample all four
// channels directly. The previous Camera2 path only had luminance (Y plane)
// and expanded it to greyscale; now we get full colour from the tracking camera.
static const char *frag_src = R"(#version 310 es
precision mediump float;
uniform sampler2D u_tex;
in vec2 v_uv;
out vec4 frag;
void main()
{
    frag = texture(u_tex, v_uv).rgba;
}
)";

static bool check_shader(GLuint s, const char *name)
{
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[512] = {};
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        LOGE("passthrough: shader %s compile failed: %s", name, log);
        return false;
    }
    return true;
}

void pico_passthrough::build_shaders()
{
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vert_src, nullptr);
    glCompileShader(vs);
    if (!check_shader(vs, "vert")) { glDeleteShader(vs); return; }

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &frag_src, nullptr);
    glCompileShader(fs);
    if (!check_shader(fs, "frag")) { glDeleteShader(fs); glDeleteShader(vs); return; }

    program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked)
    {
        char log[512] = {};
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        LOGE("passthrough: program link failed: %s", log);
        glDeleteProgram(program);
        program = 0;
        return;
    }

    sampler_loc = glGetUniformLocation(program, "u_tex");
    LOGI("passthrough: shaders OK program=%u sampler=%d", program, sampler_loc);
}

// Load the fisheye undistortion mesh from grid_point_coord.txt.
// Format: "u v x_left y_left x_right y_right" per line, 33x21 grid.
// (u,v) are camera texture coords [0,1]; (x,y) are tangent-space screen positions.
void pico_passthrough::build_mesh()
{
    FILE *f = fopen("/system/etc/pvr/boundary/grid_point_coord.txt", "r");
    if (!f)
    {
        LOGE("passthrough: cannot open grid_point_coord.txt");
        return;
    }

    struct GridPt { float u, v, xl, yl, xr, yr; };
    std::vector<GridPt> pts;
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        GridPt p;
        if (sscanf(line, "%f %f %f %f %f %f",
                   &p.u, &p.v, &p.xl, &p.yl, &p.xr, &p.yr) == 6)
            pts.push_back(p);
    }
    fclose(f);

    if (pts.empty())
    {
        LOGE("passthrough: mesh file empty or parse failed");
        return;
    }

    std::vector<float> u_vals, v_vals;
    for (auto &p : pts)
    {
        bool found_u = false, found_v = false;
        for (float u : u_vals) if (fabsf(u - p.u) < 1e-5f) { found_u = true; break; }
        for (float v : v_vals) if (fabsf(v - p.v) < 1e-5f) { found_v = true; break; }
        if (!found_u) u_vals.push_back(p.u);
        if (!found_v) v_vals.push_back(p.v);
    }
    int gw = u_vals.size();
    int gh = v_vals.size();
    LOGI("passthrough: mesh grid %dx%d (%d points)", gw, gh, (int)pts.size());

    // Sort points into grid order (row-major: v outer, u inner)
    std::vector<GridPt> grid(gw * gh);
    for (auto &p : pts)
    {
        int ix = -1, iy = -1;
        for (int i = 0; i < gw; i++) if (fabsf(u_vals[i] - p.u) < 1e-5f) { ix = i; break; }
        for (int i = 0; i < gh; i++) if (fabsf(v_vals[i] - p.v) < 1e-5f) { iy = i; break; }
        if (ix >= 0 && iy >= 0)
            grid[iy * gw + ix] = p;
    }

    // Convert tangent space to clip space. The mesh was calibrated for
    // the headset's display FOV (~101 degrees).
    const float fov_deg = 101.0f;
    const float tan_half = tanf(fov_deg * 0.5f * (float)M_PI / 180.0f);

    // The camera's vertical FOV is narrower than the display, so the mesh
    // doesn't reach y=±1 in clip space where x is visible. Find the actual
    // y extent within the visible x range and scale y to fill the screen.
    // Use both eyes so the scale is symmetric (left-only caused a slight
    // vertical offset on the right eye).
    float y_min = 1e9f, y_max = -1e9f;
    for (auto &p : grid)
    {
        float cx = p.xl / tan_half;
        if (cx >= -1.0f && cx <= 1.0f)
        {
            float cy = p.yl / tan_half;
            if (cy < y_min) y_min = cy;
            if (cy > y_max) y_max = cy;
        }
        float cxr = p.xr / tan_half;
        if (cxr >= -1.0f && cxr <= 1.0f)
        {
            float cy = p.yr / tan_half;
            if (cy < y_min) y_min = cy;
            if (cy > y_max) y_max = cy;
        }
    }
    float y_scale = 1.0f;
    if (y_max > y_min && y_max < 1.0f && y_min > -1.0f)
    {
        float y_range = y_max - y_min;
        if (y_range > 0.0f)
            y_scale = 2.0f / y_range;
    }
    // Back off slightly so the extreme edge rows of the fisheye mesh
    // (which have the most distortion and least reliable camera data)
    // don't get stretched all the way to y=±1. This removes the faint
    // "wrapping" artefact at the top and bottom of the passthrough image.
    y_scale *= 0.97f;
    LOGI("passthrough: y fill scale=%.3f (visible y=[%.3f, %.3f])", y_scale, y_min, y_max);

    // Build per-eye vertex data: pos (clip space) + uv (camera texture)
    // V is flipped because camera frames are top-down, GL textures bottom-up.
    // UVs are clamped to [0,1] to prevent edge texel bleeding even though the
    // texture uses CLAMP_TO_EDGE (some grid files have UVs that drift just
    // past the boundary).
    struct Vert { float x, y, u, v; };
    std::vector<Vert> verts_left(gw * gh);
    std::vector<Vert> verts_right(gw * gh);

    auto clamp01 = [](float f) {
        if (f < 0.0f) return 0.0f;
        if (f > 1.0f) return 1.0f;
        return f;
    };

    for (int i = 0; i < gw * gh; i++)
    {
        auto &p = grid[i];
        float u = clamp01(p.u);
        float v = clamp01(1.0f - p.v);
        verts_left[i]  = { p.xl / tan_half, (p.yl / tan_half) * y_scale, u, v };
        verts_right[i] = { p.xr / tan_half, (p.yr / tan_half) * y_scale, u, v };
    }

    // Build index buffer for the grid triangles
    std::vector<GLuint> indices;
    indices.reserve((gw - 1) * (gh - 1) * 6);
    for (int y = 0; y < gh - 1; y++)
    {
        for (int x = 0; x < gw - 1; x++)
        {
            GLuint a = y * gw + x;
            GLuint b = y * gw + x + 1;
            GLuint c = (y + 1) * gw + x;
            GLuint d = (y + 1) * gw + x + 1;
            indices.push_back(a); indices.push_back(c); indices.push_back(b);
            indices.push_back(b); indices.push_back(c); indices.push_back(d);
        }
    }
    index_count = indices.size();

    glGenBuffers(1, &ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(GLuint),
                 indices.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    for (int eye = 0; eye < 2; eye++)
    {
        auto &verts = (eye == 0) ? verts_left : verts_right;
        glGenVertexArrays(1, &eye_vao[eye]);
        glBindVertexArray(eye_vao[eye]);
        glGenBuffers(1, &eye_vbo[eye]);
        glBindBuffer(GL_ARRAY_BUFFER, eye_vbo[eye]);
        glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(Vert),
                     verts.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vert), (void *)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vert), (void *)8);
        glEnableVertexAttribArray(1);
        glBindVertexArray(0);
    }

    LOGI("passthrough: mesh loaded %d verts, %d indices", gw * gh, index_count);
}

void pico_passthrough::upload_eye(int eye, int w, int h, const uint8_t *data)
{
    if (!data || w <= 0 || h <= 0) return;
    GLuint tex = eye_tex[eye];

    // (Re)allocate the texture if dimensions changed or it doesn't exist yet.
    if (tex == 0 || eye_tex_w[eye] != w || eye_tex_h[eye] != h)
    {
        if (tex == 0)
            glGenTextures(1, &tex);
        eye_tex[eye] = tex;
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        // The SDK delivers RGBA frames (4 bytes/pixel), matching the OpenXR
        // xrGetSeeThroughDataPICO contract.
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        eye_tex_w[eye] = w;
        eye_tex_h[eye] = h;
    }
    else
    {
        glBindTexture(GL_TEXTURE_2D, tex);
    }

    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                    GL_RGBA, GL_UNSIGNED_BYTE, data);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void pico_passthrough::init()
{
    if (gl_ready) return;
    build_shaders();
    if (!program) { LOGE("passthrough: shader init failed"); return; }
    build_mesh();
    if (index_count == 0) { LOGE("passthrough: mesh init failed"); return; }
    gl_ready = true;
    LOGI("passthrough GL resources ready");
}

void pico_passthrough::start()
{
    if (camera_on) return;

    // Force the SDK to always show the see-through layer. The runtime
    // normally gates see-through on proximity to the boundary; setting
    // the distance threshold very large makes it think we're always
    // "close enough" so the camera stays on in the lobby.
    fDstcToShowSeeThrough = 1e9f;
    fDstcToShowSeeThroughComp = 1e9f;

    // Kick the SDK camera frame loop. Mode 0 = default preview.
    PVR_StartCameraPreview(0);

    // Set a reasonable delivery resolution. The SDK may clamp or ignore
    // this if it doesn't match a supported mode, but 1280x800 is the
    // documented native camera resolution for the Neo 2 tracking camera.
    PVR_SetCameraImageRect(1280, 800);

    // Gate runtime compositing of the camera layer on. Without this the
    // SDK may hold frames but never deliver them through GetSeeThroughData.
    Pvr_BoundarySetSeeThroughVisible(true);
    Pvr_BoundarySeeThroughSetVisible_(true);

    camera_on = true;
    LOGI("passthrough camera started via native SDK");
}

void pico_passthrough::stop()
{
    if (!camera_on) return;

    Pvr_BoundarySetSeeThroughVisible(false);
    Pvr_BoundarySeeThroughSetVisible_(false);

    // There is no explicit PVR_StopCameraPreview export; the SDK stops
    // the frame loop when visibility is turned off and the boundary system
    // is shut down. Reset the distance thresholds so we don't leave the
    // runtime in a forced-on state.
    fDstcToShowSeeThrough = 0.0f;
    fDstcToShowSeeThroughComp = 0.0f;

    camera_on = false;
    frame_w = 0;
    frame_h = 0;
    LOGI("passthrough camera stopped");
}

void pico_passthrough::draw(int eye)
{
    if (!gl_ready) return;
    if (eye < 0 || eye > 1) return;

    if (!camera_on)
    {
        static int retry_count = 0;
        if (++retry_count % 120 == 0)
            start();
        if (!camera_on) return;
    }

    static int acq_ok = 0, acq_empty = 0, acq_err = 0;
    static int log_tick = 0;
    static int draw_counts[2] = {0, 0};

    // Pull the latest frame for this eye from the SDK. The SDK manages the
    // buffer internally; the pointer is valid until the next call.
    bool valid = false;
    unsigned int w = 0, h = 0, count = 0;
    long long ts = 0;
    unsigned char *data = Pvr_BoundaryGetSeeThroughData(eye, &valid, &w, &h, &count, &ts);

    if (valid && data && w > 0 && h > 0)
    {
        upload_eye(eye, (int)w, (int)h, data);
        acq_ok++;

        if (frame_w != (int)w || frame_h != (int)h)
        {
            LOGI("passthrough: SDK frame %dx%d count=%u ts=%lld eye=%d",
                 w, h, count, ts, eye);
            frame_w = (int)w;
            frame_h = (int)h;
        }

        static int frame_count = 0;
        if (++frame_count % 300 == 0)
            LOGI("passthrough: SDK frame %d %ux%u ts=%lld", frame_count, w, h, ts);
    }
    else
    {
        acq_empty++;
    }

    if (++log_tick % 300 == 0)
        LOGI("passthrough: acquire ok=%d empty=%d err=%d", acq_ok, acq_empty, acq_err);

    if (eye_tex[eye] == 0) return;

    draw_counts[eye]++;
    if (draw_counts[eye] % 300 == 0)
        LOGI("passthrough: draw eye=%d count=%d", eye, draw_counts[eye]);

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);

    glUseProgram(program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, eye_tex[eye]);
    glUniform1i(sampler_loc, 0);

    glBindVertexArray(eye_vao[eye]);
    glDrawElements(GL_TRIANGLES, index_count, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);

    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);

    glDepthMask(GL_TRUE);
}

pico_passthrough::~pico_passthrough()
{
    stop();
    if (program) glDeleteProgram(program);
    for (int i = 0; i < 2; i++)
    {
        if (eye_vao[i]) glDeleteVertexArrays(1, &eye_vao[i]);
        if (eye_vbo[i]) glDeleteBuffers(1, &eye_vbo[i]);
        if (eye_tex[i]) glDeleteTextures(1, &eye_tex[i]);
    }
    if (ibo) glDeleteBuffers(1, &ibo);
}
