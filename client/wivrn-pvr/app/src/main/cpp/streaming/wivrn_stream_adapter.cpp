#include "wivrn_stream_adapter.h"
#include "streaming_client.h"
#include "pico_decoder.h"
#include "log.h"
#include "latency_tracker.h"

#include <android/hardware_buffer.h>

extern PFNEGLCREATEIMAGEKHRPROC g_eglCreateImageKHR;
extern PFNEGLDESTROYIMAGEKHRPROC g_eglDestroyImageKHR;
extern PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC g_eglGetNativeClientBufferANDROID;
extern PFNGLEGLIMAGETARGETTEXTURE2DOESPROC g_glEGLImageTargetTexture2DOES;

static GLuint g_eye_textures[2] = {0, 0};
static EGLImageKHR g_eye_images[2] = {EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR};
static AHardwareBuffer * g_last_hb[2] = {nullptr, nullptr};

bool wivrn_streaming()
{
    return g_stream && g_stream->streaming.load();
}

bool wivrn_stream_ready()
{
    if (!g_stream) return false;
    return g_stream->streaming.load() && g_stream->video_ready;
}

bool wivrn_stream_resolution(int *w, int *h)
{
    if (!g_stream || !g_stream->video_desc) return false;
    std::lock_guard lock(g_stream->video_mutex);
    if (!g_stream->video_desc) return false;
    *w = g_stream->video_desc->width;
    *h = g_stream->video_desc->height;
    return true;
}

bool wivrn_blit_eye(int eye, int viewport_w, int viewport_h)
{
    if (!g_stream) return false;

    auto frame = g_stream->get_latest_frame(eye);
    if (!frame || !frame->valid || !frame->hardware_buffer)
        return false;

    if (g_eye_textures[eye] == 0)
    {
        glGenTextures(1, &g_eye_textures[eye]);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, g_eye_textures[eye]);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
    }

    if (frame->hardware_buffer != g_last_hb[eye])
    {
        if (g_eye_images[eye] != EGL_NO_IMAGE_KHR)
        {
            g_eglDestroyImageKHR(eglGetCurrentDisplay(), g_eye_images[eye]);
            g_eye_images[eye] = EGL_NO_IMAGE_KHR;
        }

        EGLClientBuffer client_buffer = g_eglGetNativeClientBufferANDROID(frame->hardware_buffer);
        if (client_buffer)
        {
            EGLint attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
            g_eye_images[eye] = g_eglCreateImageKHR(
                eglGetCurrentDisplay(), EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                client_buffer, attrs);
        }

        if (g_eye_images[eye] != EGL_NO_IMAGE_KHR)
        {
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, g_eye_textures[eye]);
            g_glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, g_eye_images[eye]);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
        }
        g_last_hb[eye] = frame->hardware_buffer;
    }

    if (g_eye_images[eye] == EGL_NO_IMAGE_KHR)
        return false;

    if (!g_stream->blit_pipeline.is_initialized())
        g_stream->blit_pipeline.init(viewport_w, viewport_h);
    g_stream->blit_pipeline.set_resolution(viewport_w, viewport_h);

    g_stream->blit_pipeline.draw(eye, g_eye_textures[eye],
                                frame->foveation[eye], frame->width, frame->height);

    g_latency.on_frame_rendered(frame->frame_index, eye);
    return true;
}

bool wivrn_get_server_pose(XrPosef out[2])
{
    if (!g_stream) return false;
    auto frame = g_stream->get_latest_frame(0);
    if (!frame || !frame->valid) return false;
    out[0] = frame->server_pose[0];
    out[1] = frame->server_pose[1];
    return true;
}
