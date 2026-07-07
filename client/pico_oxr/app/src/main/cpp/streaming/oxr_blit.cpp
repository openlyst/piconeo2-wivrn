#include "oxr_blit.h"

#include <spdlog/spdlog.h>

oxr_blit::~oxr_blit()
{
	for (int i = 0; i < 3; i++)
	{
		if (eye_textures[i])
			glDeleteTextures(1, &eye_textures[i]);
	}
	for (auto & [hb, entry] : egl_image_cache)
	{
		if (entry.img != EGL_NO_IMAGE_KHR && g_eglDestroyImageKHR)
			g_eglDestroyImageKHR(dpy, entry.img);
	}
	egl_image_cache.clear();
	if (fbo)
		glDeleteFramebuffers(1, &fbo);
}

void oxr_blit::init(EGLDisplay display, int eye_w, int eye_h)
{
	dpy = display;

	for (int i = 0; i < 3; i++)
	{
		glGenTextures(1, &eye_textures[i]);
		glBindTexture(GL_TEXTURE_EXTERNAL_OES, eye_textures[i]);
		glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

	glGenFramebuffers(1, &fbo);
	initialized = true;
	spdlog::info("oxr_blit initialized: {}x{}", eye_w, eye_h);
}

void oxr_blit::blit(pico_blit_pipeline * pipeline, std::shared_ptr<pico_decoded_frame> frame, int eye, GLuint swapchain_tex, int eye_w, int eye_h)
{
	if (!initialized)
		return;

	int stream_idx = eye;

	if (frame && frame->valid && frame->hardware_buffer)
	{
		AHardwareBuffer * hb = frame->hardware_buffer;

		if (current_hb[stream_idx] != hb)
		{
			current_hb[stream_idx] = hb;

			auto it = egl_image_cache.find(hb);
			if (it == egl_image_cache.end())
			{
				EGLClientBuffer client_buffer = g_eglGetNativeClientBufferANDROID(hb);
				EGLImageKHR img = EGL_NO_IMAGE_KHR;
				if (client_buffer)
				{
					EGLint attrs[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
					img = g_eglCreateImageKHR(
						dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, client_buffer, attrs);
				}
				egl_image_cache[hb] = { img, 1 };
			}
			else
			{
				it->second.ref_count++;
			}

			EGLImageKHR egl_img = egl_image_cache[hb].img;
			if (egl_img != EGL_NO_IMAGE_KHR)
			{
				glBindTexture(GL_TEXTURE_EXTERNAL_OES, eye_textures[stream_idx]);
				g_glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, egl_img);
				glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
			}
		}
	}

	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);

	if (eye_textures[stream_idx] == 0 || !frame || !frame->valid)
	{
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			GL_TEXTURE_2D, swapchain_tex, 0);
		glViewport(0, 0, eye_w, eye_h);
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
	}
	else
	{
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			GL_TEXTURE_2D, swapchain_tex, 0);

		glViewport(0, 0, eye_w, eye_h);
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);

		pipeline->draw(eye, eye_textures[stream_idx],
			frame->foveation[eye],
			frame->width, frame->height);
	}

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
