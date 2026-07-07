#pragma once

#include "pico_blit.h"
#include "pico_decoder.h"
#include "streaming_client.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <android/hardware_buffer.h>

#include <unordered_map>

class oxr_blit
{
	struct HbHash { size_t operator()(AHardwareBuffer * h) const { return std::hash<void*>()(h); } };

	EGLDisplay dpy = EGL_NO_DISPLAY;
	GLuint eye_textures[3]{0, 0, 0};

	struct EGLImageCacheEntry {
		EGLImageKHR img = EGL_NO_IMAGE_KHR;
		int ref_count = 0;
	};
	std::unordered_map<AHardwareBuffer*, EGLImageCacheEntry, HbHash> egl_image_cache;

	AHardwareBuffer * current_hb[3]{nullptr, nullptr, nullptr};
	GLuint fbo = 0;
	bool initialized = false;

public:
	oxr_blit() = default;
	~oxr_blit();

	void init(EGLDisplay display, int eye_w, int eye_h);
	void blit(pico_blit_pipeline * pipeline, std::shared_ptr<pico_decoded_frame> frame, int eye, GLuint swapchain_tex, int eye_w, int eye_h);
	bool is_initialized() const { return initialized; }
};
