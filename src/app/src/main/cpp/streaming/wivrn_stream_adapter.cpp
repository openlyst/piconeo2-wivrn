#include "wivrn_stream_adapter.h"
#include "streaming_client.h"
#include "pico_decoder.h"
#include "log.h"
#include "latency_tracker.h"

#include <android/hardware_buffer.h>
#include <mutex>

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

bool wivrn_get_synced_frames(std::shared_ptr<pico_decoded_frame> out_frames[2],
                             XrPosef out_server_pose[2])
{
    if (!g_stream) return false;

    uint64_t idx0 = g_stream->latest_decoded_frame_index_per_stream[0].load(std::memory_order_acquire);
    uint64_t idx1 = g_stream->latest_decoded_frame_index_per_stream[1].load(std::memory_order_acquire);

    if (idx0 > 0 && idx1 > 0)
    {
        uint64_t chosen = std::min(idx0, idx1);
        out_frames[0] = g_stream->get_frame(chosen, 0);
        out_frames[1] = g_stream->get_frame(chosen, 1);
        if (!out_frames[0] || !out_frames[1] || !out_frames[0]->valid || !out_frames[1]->valid)
        {
            out_frames[0] = g_stream->get_latest_frame(0);
            out_frames[1] = g_stream->get_latest_frame(1);
        }
    }
    else
    {
        out_frames[0] = g_stream->get_latest_frame(0);
        out_frames[1] = g_stream->get_latest_frame(1);
    }

    for (int e = 0; e < 2; e++)
    {
        if (out_frames[e] && out_frames[e]->valid)
            out_server_pose[e] = out_frames[e]->server_pose[e];
        else
            out_server_pose[e] = {};
    }
    return true;
}

bool wivrn_blit_eye_frame(int eye, const std::shared_ptr<pico_decoded_frame> & frame,
                          int viewport_w, int viewport_h, XrPosef * out_pose)
{
    if (!frame || !frame->valid || !frame->hardware_buffer)
        return false;

    if (out_pose)
        *out_pose = frame->server_pose[eye];

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

bool wivrn_blit_eye(int eye, int viewport_w, int viewport_h, XrPosef * out_pose)
{
    if (!g_stream) return false;
    auto frame = g_stream->get_latest_frame(eye);
    return wivrn_blit_eye_frame(eye, frame, viewport_w, viewport_h, out_pose);
}

bool wivrn_get_server_pose(XrPosef out[2])
{
    std::lock_guard<std::mutex> lk(gServerPoseMutex);
    out[0] = gLastServerPoses[0];
    out[1] = gLastServerPoses[1];
    return (gLastServerPoses[0].orientation.w != 0 || gLastServerPoses[0].orientation.x != 0);
}
