#include "pvr_blit.h"
#include "streaming_client.h"

#include <spdlog/spdlog.h>

pvr_blit::~pvr_blit()
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

void pvr_blit::init(EGLDisplay display, int eye_w, int eye_h)
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
	spdlog::info("pvr_blit initialized: {}x{}", eye_w, eye_h);
}

void pvr_blit::blit(pico_blit_pipeline * pipeline, std::shared_ptr<pico_decoded_frame> frame, int eye, GLuint dst_tex, int eye_w, int eye_h)
{
	if (!initialized)
		return;

	int stream_idx = eye;

	if (frame && frame->valid && frame->hardware_buffer)
	{
		AHardwareBuffer * hb = frame->hardware_buffer;

		if (last_hb[stream_idx] != hb)
		{
			if (eye_prev_egl_images[stream_idx] != EGL_NO_IMAGE_KHR)
			{
				g_eglDestroyImageKHR(dpy, eye_prev_egl_images[stream_idx]);
				eye_prev_egl_images[stream_idx] = EGL_NO_IMAGE_KHR;
			}
			eye_prev_egl_images[stream_idx] = eye_egl_images[stream_idx];
			eye_egl_images[stream_idx] = EGL_NO_IMAGE_KHR;
			last_hb[stream_idx] = hb;

			EGLClientBuffer client_buffer = g_eglGetNativeClientBufferANDROID(hb);
			if (client_buffer)
			{
				EGLint attrs[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
				eye_egl_images[stream_idx] = g_eglCreateImageKHR(
					dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, client_buffer, attrs);
			}

			if (eye_egl_images[stream_idx] != EGL_NO_IMAGE_KHR)
			{
				glBindTexture(GL_TEXTURE_EXTERNAL_OES, eye_textures[stream_idx]);
				g_glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, eye_egl_images[stream_idx]);
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
			GL_TEXTURE_2D, dst_tex, 0);
		glViewport(0, 0, eye_w, eye_h);
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
	}
	else
	{
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			GL_TEXTURE_2D, dst_tex, 0);

		glViewport(0, 0, eye_w, eye_h);

		pipeline->draw(eye, eye_textures[stream_idx],
			frame->foveation[eye],
			frame->width, frame->height);
	}

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
