#pragma once

#include "pico_blit.h"
#include "pico_decoder.h"
#include "streaming_client.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <android/hardware_buffer.h>

class oxr_blit
{
	EGLDisplay dpy = EGL_NO_DISPLAY;
	GLuint eye_textures[3]{0, 0, 0};
	EGLImageKHR eye_egl_images[3]{EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR};
	EGLImageKHR eye_prev_egl_images[3]{EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR};
	AHardwareBuffer * last_hb[3]{nullptr, nullptr, nullptr};
	GLuint fbo = 0;
	bool initialized = false;

public:
	oxr_blit() = default;
	~oxr_blit();

	void init(EGLDisplay display, int eye_w, int eye_h);
	void blit(streaming_client * client, GLuint swapchain_tex[2], int eye_w, int eye_h);
	bool is_initialized() const { return initialized; }
};
