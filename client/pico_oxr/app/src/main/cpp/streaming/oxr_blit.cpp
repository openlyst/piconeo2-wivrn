#include "oxr_blit.h"

#include <spdlog/spdlog.h>

oxr_blit::~oxr_blit()
{
	for (int i = 0; i < 3; i++)
	{
		if (eye_textures[i])
			glDeleteTextures(1, &eye_textures[i]);
		if (eye_egl_images[i] != EGL_NO_IMAGE_KHR && g_eglDestroyImageKHR)
			g_eglDestroyImageKHR(dpy, eye_egl_images[i]);
		if (eye_prev_egl_images[i] != EGL_NO_IMAGE_KHR && g_eglDestroyImageKHR)
			g_eglDestroyImageKHR(dpy, eye_prev_egl_images[i]);
	}
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

void oxr_blit::blit(streaming_client * client, int eye, GLuint swapchain_tex, int eye_w, int eye_h)
{
	if (!initialized || !client)
		return;

	std::shared_ptr<pico_decoded_frame> frames[3];
	{
		std::lock_guard lock(client->decoded_frame_mutex);
		frames[0] = client->latest_decoded_frames[0];
		frames[1] = client->latest_decoded_frames[1];
		frames[2] = client->latest_decoded_frames[2];
	}

	for (int i = 0; i < 3; i++)
	{
		auto & decoded = frames[i];
		if (!decoded || !decoded->valid || !decoded->hardware_buffer)
			continue;

		AHardwareBuffer * hb = decoded->hardware_buffer;

		if (last_hb[i] != hb)
		{
			if (eye_prev_egl_images[i] != EGL_NO_IMAGE_KHR)
			{
				g_eglDestroyImageKHR(dpy, eye_prev_egl_images[i]);
				eye_prev_egl_images[i] = EGL_NO_IMAGE_KHR;
			}
			eye_prev_egl_images[i] = eye_egl_images[i];
			eye_egl_images[i] = EGL_NO_IMAGE_KHR;
			last_hb[i] = hb;

			EGLClientBuffer client_buffer = g_eglGetNativeClientBufferANDROID(hb);
			if (client_buffer)
			{
				EGLint attrs[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
				eye_egl_images[i] = g_eglCreateImageKHR(
					dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, client_buffer, attrs);
			}

			if (eye_egl_images[i] != EGL_NO_IMAGE_KHR)
			{
				glBindTexture(GL_TEXTURE_EXTERNAL_OES, eye_textures[i]);
				g_glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, eye_egl_images[i]);
				glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
			}
		}
	}

	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);

	int stream_idx = eye;
	if (eye_textures[stream_idx] == 0 || !frames[stream_idx] || !frames[stream_idx]->valid)
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

		client->blit_pipeline.draw(eye, eye_textures[stream_idx],
			frames[stream_idx]->foveation[eye],
			frames[stream_idx]->width, frames[stream_idx]->height);
	}

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
