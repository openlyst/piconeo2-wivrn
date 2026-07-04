/*
 * WiVRn VR streaming - Pico Native client (PvrSDK-Native)
 * Copyright (C) 2022  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022  Patrick Nicolas <patricknicolas@laposte.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "pico_client.h"

#include <spdlog/spdlog.h>

pico_client * g_client = nullptr;

PFNEGLCREATEIMAGEKHRPROC g_eglCreateImageKHR = nullptr;
PFNEGLDESTROYIMAGEKHRPROC g_eglDestroyImageKHR = nullptr;
PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC g_eglGetNativeClientBufferANDROID = nullptr;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC g_glEGLImageTargetTexture2DOES = nullptr;

void load_egl_procs()
{
	if (g_eglCreateImageKHR)
		return;
	g_eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
	g_eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
	g_eglGetNativeClientBufferANDROID = (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress("eglGetNativeClientBufferANDROID");
	g_glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
	spdlog::warn("EGL procs: createImage={} destroyImage={} getNativeBuffer={} imageTarget={}",
		!!g_eglCreateImageKHR, !!g_eglDestroyImageKHR, !!g_eglGetNativeClientBufferANDROID, !!g_glEGLImageTargetTexture2DOES);
}
