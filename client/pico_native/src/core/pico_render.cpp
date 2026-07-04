#include "pico_client.h"
#include "pico_stutter.h"

#include <spdlog/spdlog.h>
#include <GLES3/gl3.h>
#include <android/bitmap.h>
#include <cmath>
#include <cstring>

using namespace wivrn;

extern "C" {

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnDrawEye(JNIEnv * env, jobject thiz, jlong ptr, jint eye)
{
	if (!g_client || !g_client->gl_initialized)
	{
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		return;
	}

	if (eye < 0 || eye > 1)
	{
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		return;
	}

	std::shared_ptr<pico_decoded_frame> decoded;
	decoded = g_client->render_frames[eye];

	GLuint ext_tex = 0;
	if (decoded && decoded->valid && decoded->hardware_buffer)
	{
		AHardwareBuffer * hb = decoded->hardware_buffer;

		if (g_client->eye_textures[eye] == 0)
		{
			glGenTextures(1, &g_client->eye_textures[eye]);
			glBindTexture(GL_TEXTURE_EXTERNAL_OES, g_client->eye_textures[eye]);
			glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
		}

		if (g_client->last_hb[eye] != hb)
		{
			if (g_client->eye_prev_egl_images[eye] != EGL_NO_IMAGE_KHR)
			{
				g_eglDestroyImageKHR(eglGetDisplay(EGL_DEFAULT_DISPLAY), g_client->eye_prev_egl_images[eye]);
				g_client->eye_prev_egl_images[eye] = EGL_NO_IMAGE_KHR;
			}
			g_client->eye_prev_egl_images[eye] = g_client->eye_egl_images[eye];
			g_client->eye_egl_images[eye] = EGL_NO_IMAGE_KHR;

			g_client->eye_current_frames[eye] = decoded;
			g_client->last_hb[eye] = hb;

			EGLClientBuffer client_buffer = g_eglGetNativeClientBufferANDROID(hb);
			if (client_buffer)
			{
				EGLint attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
				g_client->eye_egl_images[eye] = g_eglCreateImageKHR(
					eglGetDisplay(EGL_DEFAULT_DISPLAY), EGL_NO_CONTEXT,
					EGL_NATIVE_BUFFER_ANDROID, client_buffer, attrs);
			}

			if (g_client->eye_egl_images[eye] != EGL_NO_IMAGE_KHR)
			{
				glBindTexture(GL_TEXTURE_EXTERNAL_OES, g_client->eye_textures[eye]);
				g_glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, g_client->eye_egl_images[eye]);
				glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
			}
		}

		ext_tex = g_client->eye_textures[eye];
	}

	bool show_lobby = (ext_tex == 0 || !g_client->streaming.load() || g_client->stream_ui_visible.load());

	if (eye == 0 && g_client->streaming.load())
	{
		controller_sample cs[2];
		g_client->tracker.get_controllers(cs);

		bool both_click = cs[0].connected && cs[1].connected &&
			cs[0].thumbstick_click && cs[1].thumbstick_click;
		bool prev_both = g_client->prev_thumbstick_click[0] && g_client->prev_thumbstick_click[1];

		if (both_click && !prev_both)
		{
			g_client->stream_ui_visible = !g_client->stream_ui_visible;
			spdlog::info("Stream UI toggled: {}", g_client->stream_ui_visible.load());

			if (g_client->vm && g_client->activity)
			{
				JNIEnv * env2 = nullptr;
				bool attached = false;
				if (g_client->vm->GetEnv((void **)&env2, JNI_VERSION_1_6) != JNI_OK)
				{
					if (g_client->vm->AttachCurrentThread(&env2, nullptr) == JNI_OK)
						attached = true;
				}
				if (env2 && g_client->activity)
				{
					jclass clazz = env2->GetObjectClass(g_client->activity);
					jmethodID method = env2->GetMethodID(clazz, "onLobbyTouch", "(FFZZF)V");
					if (method)
						env2->CallVoidMethod(g_client->activity, method, -1.0f, -1.0f, false, false, 0.0f);
					env2->DeleteLocalRef(clazz);
				}
				if (attached)
					g_client->vm->DetachCurrentThread();
			}
		}
		g_client->prev_thumbstick_click[0] = cs[0].thumbstick_click;
		g_client->prev_thumbstick_click[1] = cs[1].thumbstick_click;

		int64_t now_ns = g_client->get_timestamp_ns();
		if (g_client->stats_last_time == 0)
			g_client->stats_last_time = now_ns;

		int64_t elapsed = now_ns - g_client->stats_last_time;
		if (elapsed >= 500000000LL)
		{
			float dt = elapsed * 1e-9f;
			int fps = (int)(g_client->stats_frame_count / dt);
			g_client->stats_frame_count = 0;
			g_client->stats_last_time = now_ns;

			uint64_t rx = g_client->session ? g_client->session->bytes_received() : 0;
			uint64_t tx = g_client->session ? g_client->session->bytes_sent() : 0;
			float bw_rx = (float)(rx - g_client->stats_bytes_rx) / dt;
			float bw_tx = (float)(tx - g_client->stats_bytes_tx) / dt;
			g_client->stats_bytes_rx = rx;
			g_client->stats_bytes_tx = tx;

			g_client->stats_bandwidth_rx = 0.8f * g_client->stats_bandwidth_rx + 0.2f * bw_rx;
			g_client->stats_bandwidth_tx = 0.8f * g_client->stats_bandwidth_tx + 0.2f * bw_tx;

			int latency_ms = 0;
			if (g_client->stats_last_encode_begin > 0)
			{
				int64_t latency = now_ns - g_client->stats_last_encode_begin;
				if (latency > 0 && latency < 1000000000LL)
					latency_ms = (int)(latency / 1000000LL);
			}

			int bitrate_mbps = 50;

			g_client->notify_stream_stats(fps, latency_ms,
				g_client->stats_bandwidth_rx * 8, g_client->stats_bandwidth_tx * 8,
				bitrate_mbps);
		}
	}

	if (show_lobby)
	{
		float h_orient[4], h_pos[3];
		g_client->tracker.get_head_pose(h_orient, h_pos);
		controller_sample cs[2];
		g_client->tracker.get_controllers(cs);

		g_client->lobby.draw(eye, h_orient, h_pos, cs, g_client->eye_fov[eye], 0.064f);

		if (eye == 0 && g_client->vm && g_client->activity)
		{
			for (int h = 0; h < 2; h++)
			{
				bool has_hit = g_client->lobby.lobby_touch_x[h] >= 0;
				bool has_click = g_client->lobby.lobby_touch_down[h] || g_client->lobby.lobby_touch_pressed[h];

				if (has_hit || has_click)
				{
					JNIEnv * env2 = nullptr;
					bool attached = false;
					if (g_client->vm->GetEnv((void **)&env2, JNI_VERSION_1_6) != JNI_OK)
					{
						if (g_client->vm->AttachCurrentThread(&env2, nullptr) == JNI_OK)
							attached = true;
					}

					if (env2 && g_client->activity)
					{
						jclass clazz = env2->GetObjectClass(g_client->activity);
						jmethodID method = env2->GetMethodID(clazz, "onLobbyTouch", "(FFZZF)V");
						if (method)
						{
							env2->CallVoidMethod(g_client->activity, method,
								g_client->lobby.lobby_touch_x[h],
								g_client->lobby.lobby_touch_y[h],
								g_client->lobby.lobby_touch_down[h],
								g_client->lobby.lobby_touch_pressed[h],
								g_client->lobby.lobby_thumbstick_y[h]);
						}
						env2->DeleteLocalRef(clazz);
					}

					if (attached)
						g_client->vm->DetachCurrentThread();

					break;
				}
			}
			if (g_client->lobby.lobby_touch_x[0] < 0 && g_client->lobby.lobby_touch_x[1] < 0)
			{
				JNIEnv * env2 = nullptr;
				bool attached = false;
				if (g_client->vm->GetEnv((void **)&env2, JNI_VERSION_1_6) != JNI_OK)
				{
					if (g_client->vm->AttachCurrentThread(&env2, nullptr) == JNI_OK)
						attached = true;
				}

				if (env2 && g_client->activity)
				{
					jclass clazz = env2->GetObjectClass(g_client->activity);
					jmethodID method = env2->GetMethodID(clazz, "onLobbyTouch", "(FFZZF)V");
					if (method)
					{
						env2->CallVoidMethod(g_client->activity, method, -1.0f, -1.0f, false, false, 0.0f);
					}
					env2->DeleteLocalRef(clazz);
				}

				if (attached)
					g_client->vm->DetachCurrentThread();
			}
		}
	}
	else
	{
		g_client->blit_pipeline.draw(eye, ext_tex, {}, {}, {});
	}
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnFrameEnd(JNIEnv * env, jobject thiz, jlong ptr)
{
	if (!g_client)
		return;

	if (!g_client->streaming.load() || g_client->stream_ui_visible.load())
		return;

	g_stutter.on_frame_end();

	auto left = g_client->render_frames[0];
	auto right = g_client->render_frames[1];

	std::shared_ptr<pico_decoded_frame> pose_source;
	if (left && right)
		pose_source = (left->frame_index <= right->frame_index) ? left : right;
	else
		pose_source = left ? left : right;

	for (int eye = 0; eye < 2; eye++)
	{
		if (!pose_source || !pose_source->valid)
			continue;

		XrPosef pose = pose_source->server_pose[eye];

		float n2 = pose.orientation.x * pose.orientation.x
		         + pose.orientation.y * pose.orientation.y
		         + pose.orientation.z * pose.orientation.z
		         + pose.orientation.w * pose.orientation.w;
		if (n2 < 1e-6f)
		{
			pose.orientation = {0, 0, 0, 1};
		}
		else if (std::abs(n2 - 1.0f) > 1e-4f)
		{
			float inv = 1.0f / std::sqrt(n2);
			pose.orientation.x *= inv;
			pose.orientation.y *= inv;
			pose.orientation.z *= inv;
			pose.orientation.w *= inv;
		}

		pvrSensorState sensor;
		memset(&sensor, 0, sizeof(sensor));
		float * f = reinterpret_cast<float *>(sensor.data);
		f[0] = pose.orientation.x;
		f[1] = pose.orientation.y;
		f[2] = pose.orientation.z;
		f[3] = pose.orientation.w;
		f[4] = pose.position.x;
		f[5] = pose.position.y;
		f[6] = pose.position.z;

		PVR_ChangeRenderPose(eye, &sensor);
	}
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnInitGL(JNIEnv * env, jobject thiz, jlong ptr, jint w, jint h)
{
	if (!g_client)
		return;

	spdlog::warn("nativeInitGL: {}x{}", w, h);

	g_client->eye_width = w;
	g_client->eye_height = h;

	g_client->blit_pipeline.init(w, h);
	g_client->lobby.init(w, h);
	load_egl_procs();

	for (int e = 0; e < 2; e++)
	{
		for (int i = 0; i < g_client->kSwapLen; i++)
		{
			if (g_client->swap_tex[e][i] == 0)
			{
				glGenTextures(1, &g_client->swap_tex[e][i]);
				glBindTexture(GL_TEXTURE_2D, g_client->swap_tex[e][i]);
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
				glBindTexture(GL_TEXTURE_2D, 0);
			}
		}
	}
	if (g_client->stream_fbo == 0)
		glGenFramebuffers(1, &g_client->stream_fbo);

	g_client->gl_initialized = true;

	spdlog::warn("GLES initialized");
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnDeInitGL(JNIEnv * env, jobject thiz, jlong ptr)
{
	spdlog::info("nativeWivrnDeInitGL");
	if (g_client)
	{
		g_client->gl_initialized = false;
	}
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnSurfaceChanged(JNIEnv * env, jobject thiz, jlong ptr, jint w, jint h)
{
	spdlog::info("nativeSurfaceChanged: {}x{}", w, h);
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeUpdateLobbyTexture(JNIEnv * env, jobject thiz, jlong ptr, jobject bitmap)
{
	if (!g_client || !g_client->gl_initialized || !bitmap)
		return;

	AndroidBitmapInfo info;
	if (AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS)
	{
		spdlog::error("AndroidBitmap_getInfo failed");
		return;
	}

	if (info.format != ANDROID_BITMAP_FORMAT_RGBA_8888)
	{
		spdlog::error("Lobby bitmap format is not RGBA_8888 (got {})", info.format);
		return;
	}

	void * pixels = nullptr;
	if (AndroidBitmap_lockPixels(env, bitmap, &pixels) != ANDROID_BITMAP_RESULT_SUCCESS)
	{
		spdlog::error("AndroidBitmap_lockPixels failed");
		return;
	}

	g_client->lobby.update_texture(info.width, info.height, pixels);

	AndroidBitmap_unlockPixels(env, bitmap);
}

} // extern "C"
