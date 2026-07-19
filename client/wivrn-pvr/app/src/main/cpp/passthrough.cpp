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

static const char *frag_src = R"(#version 310 es
precision mediump float;
uniform sampler2D u_tex;
in vec2 v_uv;
out vec4 frag;
void main()
{
    float y = texture(u_tex, v_uv).r;
    frag = vec4(vec3(y), 1.0);
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
    }
    float y_scale = 1.0f;
    if (y_max > y_min && y_max < 1.0f && y_min > -1.0f)
    {
        float y_range = y_max - y_min;
        if (y_range > 0.0f)
            y_scale = 2.0f / y_range;
    }
    LOGI("passthrough: y fill scale=%.3f (visible y=[%.3f, %.3f])", y_scale, y_min, y_max);

    // Build per-eye vertex data: pos (clip space) + uv (camera texture)
    // V is flipped because camera frames are top-down, GL textures bottom-up.
    struct Vert { float x, y, u, v; };
    std::vector<Vert> verts_left(gw * gh);
    std::vector<Vert> verts_right(gw * gh);

    for (int i = 0; i < gw * gh; i++)
    {
        auto &p = grid[i];
        verts_left[i]  = { p.xl / tan_half, (p.yl / tan_half) * y_scale, p.u, 1.0f - p.v };
        verts_right[i] = { p.xr / tan_half, (p.yr / tan_half) * y_scale, p.u, 1.0f - p.v };
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

void pico_passthrough::upload_eye(int eye, int w, int h, const uint8_t *data, int row_stride)
{
    if (!data || w <= 0 || h <= 0) return;
    GLuint tex = eye_tex[eye];

    if (tex == 0)
    {
        glGenTextures(1, &tex);
        eye_tex[eye] = tex;
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0,
                     GL_RED, GL_UNSIGNED_BYTE, nullptr);
    }
    else
    {
        glBindTexture(GL_TEXTURE_2D, tex);
    }

    if (row_stride != w)
    {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, row_stride);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                        GL_RED, GL_UNSIGNED_BYTE, data);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
    else
    {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                        GL_RED, GL_UNSIGNED_BYTE, data);
    }
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

static void on_capture_failed(void *ctx, ACameraCaptureSession *s,
                              ACaptureRequest *r, ACameraCaptureFailure *f)
{ (void)ctx; (void)s; (void)r; (void)f; }

static ACameraCaptureSession_captureCallbacks gCaptureCallbacks = {
    .onCaptureFailed = on_capture_failed,
};

static void on_dev_disconnected(void *ctx, ACameraDevice *d)
{ (void)ctx; (void)d; }
static void on_dev_error(void *ctx, ACameraDevice *d, int err)
{ (void)ctx; (void)d; (void)err; }
static ACameraDevice_StateCallbacks gDevCallbacks = {
    .onDisconnected = on_dev_disconnected,
    .onError = on_dev_error,
};

static void on_sess_active(void *ctx, ACameraCaptureSession *s)
{ (void)ctx; (void)s; }
static void on_sess_ready(void *ctx, ACameraCaptureSession *s)
{ (void)ctx; (void)s; }
static void on_sess_closed(void *ctx, ACameraCaptureSession *s)
{ (void)ctx; (void)s; }
static ACameraCaptureSession_stateCallbacks gSessionCallbacks = {
    .onActive = on_sess_active,
    .onReady = on_sess_ready,
    .onClosed = on_sess_closed,
};

void pico_passthrough::start()
{
    if (camera_on) return;

    cam_mgr = ACameraManager_create();
    if (!cam_mgr) { LOGE("passthrough: ACameraManager_create failed"); return; }

    ACameraIdList *ids = nullptr;
    ACameraManager_getCameraIdList(cam_mgr, &ids);
    if (!ids || ids->numCameras == 0)
    {
        LOGE("passthrough: no cameras found");
        if (ids) ACameraManager_deleteCameraIdList(ids);
        return;
    }
    LOGI("passthrough: found %d cameras, using id[0]=%s", ids->numCameras, ids->cameraIds[0]);

    camera_status_t rc = ACameraManager_openCamera(cam_mgr, ids->cameraIds[0],
        &gDevCallbacks, &cam_dev);
    if (rc != ACAMERA_OK || !cam_dev)
    {
        LOGE("passthrough: openCamera failed rc=%d", rc);
        ACameraManager_deleteCameraIdList(ids);
        return;
    }

    media_status_t mrc = AImageReader_newWithUsage(1280, 400, AIMAGE_FORMAT_YUV_420_888,
        AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, 4, &img_reader);
    if (mrc != AMEDIA_OK || !img_reader)
    {
        LOGE("passthrough: AImageReader_new failed rc=%d", mrc);
        ACameraManager_deleteCameraIdList(ids);
        return;
    }

    AImageReader_getWindow(img_reader, &img_reader_win);
    ANativeWindow_acquire(img_reader_win);

    ACameraManager_deleteCameraIdList(ids);

    ACaptureSessionOutput *output = nullptr;
    ACaptureSessionOutputContainer *outputs = nullptr;
    ACaptureSessionOutputContainer_create(&outputs);
    ACaptureSessionOutput_create(img_reader_win, &output);
    ACaptureSessionOutputContainer_add(outputs, output);

    rc = ACameraDevice_createCaptureSession(cam_dev, outputs, &gSessionCallbacks,
        &cam_session);
    if (rc != ACAMERA_OK)
    {
        LOGE("passthrough: createCaptureSession failed rc=%d", rc);
        ACaptureSessionOutput_free(output);
        ACaptureSessionOutputContainer_free(outputs);
        return;
    }

    rc = ACameraDevice_createCaptureRequest(cam_dev, TEMPLATE_PREVIEW, &cam_request);
    if (rc != ACAMERA_OK)
    {
        LOGE("passthrough: createCaptureRequest failed rc=%d", rc);
        ACaptureSessionOutput_free(output);
        ACaptureSessionOutputContainer_free(outputs);
        return;
    }

    ACameraOutputTarget *target = nullptr;
    ACameraOutputTarget_create(img_reader_win, &target);
    ACaptureRequest_addTarget(cam_request, target);

    rc = ACameraCaptureSession_setRepeatingRequest(cam_session, &gCaptureCallbacks,
        1, &cam_request, nullptr);
    if (rc != ACAMERA_OK)
        LOGE("passthrough: setRepeatingRequest failed rc=%d", rc);
    else
        LOGI("passthrough: camera capture started");

    ACameraOutputTarget_free(target);
    ACaptureSessionOutput_free(output);
    ACaptureSessionOutputContainer_free(outputs);

    camera_on = true;
    LOGI("passthrough camera started");
}

void pico_passthrough::stop()
{
    if (!camera_on) return;

    if (pending_image) { AImage_delete(pending_image); pending_image = nullptr; }

    if (cam_session)
    {
        ACameraCaptureSession_stopRepeating(cam_session);
        ACameraCaptureSession_close(cam_session);
        cam_session = nullptr;
    }
    if (cam_request) { ACaptureRequest_free(cam_request); cam_request = nullptr; }
    if (cam_dev) { ACameraDevice_close(cam_dev); cam_dev = nullptr; }
    if (img_reader_win) { ANativeWindow_release(img_reader_win); img_reader_win = nullptr; }
    if (img_reader) { AImageReader_delete(img_reader); img_reader = nullptr; }
    if (cam_mgr) { ACameraManager_delete(cam_mgr); cam_mgr = nullptr; }

    camera_on = false;
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
    if (!img_reader) return;

    static int acq_ok = 0, acq_empty = 0, acq_err = 0;
    static int log_tick = 0;
    static int draw_counts[2] = {0, 0};

    if (eye == 0)
        got_new_this_frame = false;

    if (eye == 0)
    {
        AImage *image = nullptr;
        media_status_t rc = AImageReader_acquireLatestImage(img_reader, &image);

        if (rc == AMEDIA_OK && image)
        {
            if (pending_image)
                AImage_delete(pending_image);

            int32_t w = 0, h = 0;
            AImage_getWidth(image, &w);
            AImage_getHeight(image, &h);
            uint8_t *y_data = nullptr;
            int y_len = 0, y_stride = 0, y_pix = 0;
            AImage_getPlaneData(image, 0, &y_data, &y_len);
            AImage_getPlaneRowStride(image, 0, &y_stride);
            AImage_getPlanePixelStride(image, 0, &y_pix);

            pending_image = image;
            pending_w = w;
            pending_h = h;
            pending_y = y_data;
            pending_stride = y_stride;
            acq_ok++;
            got_new_this_frame = true;

            static int frame_count = 0;
            if (++frame_count % 300 == 0)
                LOGI("passthrough: cam frame %d %dx%d stride=%d", frame_count, w, h, y_stride);
        }
        else if (rc == AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE)
        {
            acq_empty++;
        }
        else
        {
            acq_err++;
            if (pending_image)
            {
                AImage_delete(pending_image);
                pending_image = nullptr;
                pending_y = nullptr;
            }
        }

        if (++log_tick % 300 == 0)
            LOGI("passthrough: acquire ok=%d empty=%d err=%d", acq_ok, acq_empty, acq_err);
    }

    if (!pending_image || !pending_y) return;

    // Split SBS frame: left eye = left half, right eye = right half.
    int half_w = pending_w / 2;
    const uint8_t *eye_data = pending_y + eye * half_w;

    if (got_new_this_frame || eye_tex[eye] == 0)
        upload_eye(eye, half_w, pending_h, eye_data, pending_stride);

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
