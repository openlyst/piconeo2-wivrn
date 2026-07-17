#include "passthrough.h"
#include "log.h"
#include <cstring>

// Fullscreen clip-space quad: position (xyz) + UV.
static const char *vert_src = R"(#version 310 es
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
out vec2 v_uv;
void main()
{
    gl_Position = vec4(a_pos, 1.0);
    v_uv = a_uv;
}
)";

static const char *frag_src = R"(#version 310 es
precision mediump sampler2D;
uniform sampler2D u_tex;
in vec2 v_uv;
out vec4 frag;
void main()
{
    frag = vec4(texture(u_tex, v_uv).rgb, 1.0);
}
)";

void pico_passthrough::build_shaders()
{
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vert_src, nullptr);
    glCompileShader(vs);

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &frag_src, nullptr);
    glCompileShader(fs);

    program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    sampler_loc = glGetUniformLocation(program, "u_tex");
    mvp_loc = glGetUniformLocation(program, "u_mvp");
}

void pico_passthrough::build_geometry()
{
    // Two triangles covering clip space [-1,1]
    static const float verts[] = {
        -1, -1, 0,  0, 0,
         1, -1, 0,  1, 0,
        -1,  1, 0,  0, 1,
        -1,  1, 0,  0, 1,
         1, -1, 0,  1, 0,
         1,  1, 0,  1, 1,
    };
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20, (void *)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 20, (void *)12);
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
}

void pico_passthrough::upload_frame(int w, int h, const uint8_t *y_data, int y_row_stride)
{
    if (!y_data || w <= 0 || h <= 0) return;

    if (y_tex == 0)
    {
        glGenTextures(1, &y_tex);
        glBindTexture(GL_TEXTURE_2D, y_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, w, h, 0,
                     GL_LUMINANCE, GL_UNSIGNED_BYTE, nullptr);
        tex_w = w;
        tex_h = h;
    }
    else
    {
        glBindTexture(GL_TEXTURE_2D, y_tex);
        if (w != tex_w || h != tex_h)
        {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, w, h, 0,
                         GL_LUMINANCE, GL_UNSIGNED_BYTE, nullptr);
            tex_w = w;
            tex_h = h;
        }
    }

    // Upload row by row if stride != width
    if (y_row_stride == w)
    {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                        GL_LUMINANCE, GL_UNSIGNED_BYTE, y_data);
    }
    else
    {
        for (int row = 0; row < h; row++)
        {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row, w, 1,
                            GL_LUMINANCE, GL_UNSIGNED_BYTE,
                            y_data + row * y_row_stride);
        }
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

void pico_passthrough::init()
{
    if (gl_ready) return;
    build_shaders();
    build_geometry();
    gl_ready = true;
    LOGI("passthrough GL resources ready");
}

static void on_capture_failed(void *ctx, ACameraCaptureSession *s,
                              ACaptureRequest *r, ACameraCaptureFailure *f)
{
    (void)ctx; (void)s; (void)r; (void)f;
}

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

    // Create ImageReader for YUV_420_888 at the camera's native resolution
    // The Neo 2 tracking camera delivers 1280x400 (side-by-side stereo).
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

    // Build capture session: camera -> image reader surface
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

    // Build capture request: preview -> image reader
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

    // Start continuous capture
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

    // Retry starting the camera if it failed earlier (e.g. permission
    // not yet granted at init time). Throttle to once per second.
    if (!camera_on)
    {
        static int retry_count = 0;
        if (++retry_count % 120 == 0)
            start();
        if (!camera_on) return;
    }
    if (!img_reader) return;

    // Get the latest frame from the ImageReader
    AImage *image = nullptr;
    media_status_t rc = AImageReader_acquireLatestImage(img_reader, &image);
    if (rc != AMEDIA_OK || !image)
    {
        static int null_count = 0;
        if (++null_count % 300 == 0)
            LOGE("passthrough: acquireLatestImage rc=%d", rc);
        return;
    }

    // Extract Y plane (luminance = grayscale passthrough)
    int32_t src_w = 0, src_h = 0;
    AImage_getWidth(image, &src_w);
    AImage_getHeight(image, &src_h);

    uint8_t *y_data = nullptr;
    int y_len = 0, y_row_stride = 0, y_pix_stride = 0;
    AImage_getPlaneData(image, 0, &y_data, &y_len);
    AImage_getPlaneRowStride(image, 0, &y_row_stride);
    AImage_getPlanePixelStride(image, 0, &y_pix_stride);

    static int frame_count = 0;
    if (++frame_count % 300 == 0)
        LOGI("passthrough: frame %d %dx%d y_stride=%d y_pix=%d bytes=[%d,%d,%d,%d]",
             frame_count, src_w, src_h, y_row_stride, y_pix_stride,
             y_data[0], y_data[1], y_data[2], y_data[3]);

    // Side-by-side stereo: left eye gets left half, right eye gets right half.
    // The Y plane is contiguous with row stride = full width (1280), so
    // each eye's column offset is eye * half_w within every row.
    int half_w = src_w / 2;
    const uint8_t *eye_data = y_data + eye * half_w;

    upload_frame(half_w, src_h, eye_data, y_row_stride);

    AImage_delete(image);

    // Render
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);

    glUseProgram(program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, y_tex);
    glUniform1i(sampler_loc, 0);

    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);

    glDepthMask(GL_TRUE);
}

pico_passthrough::~pico_passthrough()
{
    stop();
    if (program) glDeleteProgram(program);
    if (vao) glDeleteVertexArrays(1, &vao);
    if (vbo) glDeleteBuffers(1, &vbo);
    if (y_tex) glDeleteTextures(1, &y_tex);
}
