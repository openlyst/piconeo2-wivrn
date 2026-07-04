#include "pico_render_thread.h"
#include "pico_client.h"
#include "pico_sdk.h"
#include "pico_sched.h"
#include "pico_stutter.h"
#include "pico_blit.h"

#include <spdlog/spdlog.h>
#include <android/hardware_buffer.h>
#include <GLES2/gl2ext.h>
#include <cstring>
#include <cmath>
#include <unistd.h>

#ifndef EGL_CONTEXT_PRIORITY_LEVEL_IMG
#define EGL_CONTEXT_PRIORITY_LEVEL_IMG 0x3100
#define EGL_CONTEXT_PRIORITY_HIGH_IMG  0x3101
#define EGL_CONTEXT_PRIORITY_MEDIUM_IMG 0x3102
#define EGL_CONTEXT_PRIORITY_LOW_IMG   0x3103
#endif

extern pico_client * g_client;
extern stutter_detector g_stutter;

extern PFNEGLCREATEIMAGEKHRPROC g_eglCreateImageKHR;
extern PFNEGLDESTROYIMAGEKHRPROC g_eglDestroyImageKHR;
extern PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC g_eglGetNativeClientBufferANDROID;
extern PFNGLEGLIMAGETARGETTEXTURE2DOESPROC g_glEGLImageTargetTexture2DOES;
extern void load_egl_procs();

pico_render_thread::pico_render_thread() = default;

pico_render_thread::~pico_render_thread()
{
	stop();
}

void pico_render_thread::start(pico_client * c)
{
	client = c;
	running = true;
	thread = std::thread([this] { run(); });
}

void pico_render_thread::stop()
{
	running = false;
	if (thread.joinable())
		thread.join();
}

void pico_render_thread::set_surface(ANativeWindow * window)
{
	{
		std::lock_guard lk(win_mutex);
		if (pending_window)
			ANativeWindow_release(pending_window);
		pending_window = window;
		if (pending_window)
			ANativeWindow_acquire(pending_window);
	}
	window_dirty = true;
}

void pico_render_thread::clear_surface()
{
	{
		std::lock_guard lk(win_mutex);
		if (pending_window)
		{
			ANativeWindow_release(pending_window);
			pending_window = nullptr;
		}
	}
	window_dirty = true;
}

void pico_render_thread::init_egl()
{
	dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	eglInitialize(dpy, nullptr, nullptr);

	const EGLint cfgAttribs[] = {
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_DEPTH_SIZE, 24, EGL_NONE
	};
	EGLint n = 0;
	eglChooseConfig(dpy, cfgAttribs, &cfg, 1, &n);
	if (!n)
	{
		spdlog::error("eglChooseConfig failed");
		return;
	}

	const EGLint ctxAttribsLow[] = {
		EGL_CONTEXT_CLIENT_VERSION, 3,
		EGL_CONTEXT_PRIORITY_LEVEL_IMG, EGL_CONTEXT_PRIORITY_LOW_IMG,
		EGL_NONE
	};

	ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttribsLow);
	if (ctx == EGL_NO_CONTEXT)
	{
		const EGLint ctxFallback[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
		ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxFallback);
	}

	const EGLint pbufAttribs[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
	pbuf = eglCreatePbufferSurface(dpy, cfg, pbufAttribs);
	eglMakeCurrent(dpy, pbuf, pbuf, ctx);

	spdlog::info("render thread EGL initialized");
}

void pico_render_thread::init_sdk()
{
	if (!client || !client->activity)
		return;

	JNIEnv * env = nullptr;
	bool attached = false;
	if (client->vm->GetEnv((void **)&env, JNI_VERSION_1_6) != JNI_OK)
	{
		if (client->vm->AttachCurrentThread(&env, nullptr) == JNI_OK)
			attached = true;
	}

	jclass vrClass = nullptr;
	if (env && client->activity)
	{
		jclass actClass = env->GetObjectClass(client->activity);
		vrClass = (jclass)env->NewGlobalRef(actClass);
		env->DeleteLocalRef(actClass);
	}

	Pvr_SetInitActivity((void *)client->activity, (void *)vrClass);
	Pvr_Enable6DofModule(true);
	int initRc = Pvr_Init(0);
	int sensRc = InitSensor();
	int startRc = Pvr_StartSensor(0);
	spdlog::info("Pvr_Init={} InitSensor={} Pvr_StartSensor={}", initRc, sensRc, startRc);

	bool guardian = Pvr_BoundaryGetConfigured();
	int originType = guardian ? 2 : 1;
	bool originRc = Pvr_SetTrackingOriginType(originType);
	spdlog::info("Pvr_SetTrackingOriginType({})={} floorHeight={:.3f} guardian={}",
		guardian ? "StageLevel" : "FloorLevel", originRc, Pvr_GetFloorHeight(), guardian);

	Pvr_DisableBoundary();
	Pvr_ShutdownSDKBoundary();

	float fov = 101.0f; // This should not be hardcoded but changeing it will crash the client
	Pvr_SetProjectionFov(fov, fov);
	spdlog::info("FOV set to {:.1f}", fov);

	sdk_inited = true;

	if (attached)
		client->vm->DetachCurrentThread();
}

void pico_render_thread::init_swapchain()
{
	for (int e = 0; e < 2; e++)
	{
		glGenTextures(kSwapLen, swap_tex[e]);
		for (int i = 0; i < kSwapLen; i++)
		{
			glBindTexture(GL_TEXTURE_2D, swap_tex[e][i]);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, eye_width, eye_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		}
		glBindTexture(GL_TEXTURE_2D, 0);
	}

	if (stream_fbo == 0)
		glGenFramebuffers(1, &stream_fbo);

	for (int e = 0; e < 2; e++)
	{
		if (eye_textures[e] == 0)
		{
			glGenTextures(1, &eye_textures[e]);
			glBindTexture(GL_TEXTURE_EXTERNAL_OES, eye_textures[e]);
			glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
		}
	}

	spdlog::info("swapchain initialized {}x{}", eye_width, eye_height);
}

void pico_render_thread::on_surface_changed()
{
	ANativeWindow * newWin = nullptr;
	{
		std::lock_guard lk(win_mutex);
		newWin = pending_window;
		pending_window = nullptr;
		window_dirty = false;
	}

	if (newWin == cur_win)
	{
		if (newWin) ANativeWindow_release(newWin);
		return;
	}

	if (cur_win)
	{
		if (rt_inited && warp_to_window)
		{
			RenderEventFunc re = (RenderEventFunc)GetRenderEventFunc();
			if (re) re(EV_Pause);
		}
		eglMakeCurrent(dpy, pbuf, pbuf, ctx);
		if (win_surface != EGL_NO_SURFACE)
		{
			eglDestroySurface(dpy, win_surface);
			win_surface = EGL_NO_SURFACE;
		}
		ANativeWindow_release(cur_win);
		cur_win = nullptr;
	}

	if (newWin)
	{
		cur_win = newWin;
		win_surface = eglCreateWindowSurface(dpy, cfg, cur_win, nullptr);
		if (win_surface == EGL_NO_SURFACE)
		{
			spdlog::error("eglCreateWindowSurface failed 0x{:x}", eglGetError());
			return;
		}

		eglQuerySurface(dpy, win_surface, EGL_WIDTH, &win_w);
		eglQuerySurface(dpy, win_surface, EGL_HEIGHT, &win_h);
		spdlog::info("window surface ready {}x{}", win_w, win_h);

		if (!rt_inited)
		{
			Pvr_SetSinglePassDepthBufferWidthHeight(win_w / 2, win_h);

			const EGLint ctxAttribsHigh[] = {
				EGL_CONTEXT_CLIENT_VERSION, 3,
				EGL_CONTEXT_PRIORITY_LEVEL_IMG, EGL_CONTEXT_PRIORITY_HIGH_IMG,
				EGL_NONE
			};
			warp_ctx = eglCreateContext(dpy, cfg, ctx, ctxAttribsHigh);
			if (warp_ctx == EGL_NO_CONTEXT)
			{
				const EGLint fb[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
				warp_ctx = eglCreateContext(dpy, cfg, ctx, fb);
			}

			eglMakeCurrent(dpy, win_surface, win_surface, warp_ctx);
			RenderEventFunc re = (RenderEventFunc)GetRenderEventFunc();
			if (re)
			{
				re(EV_InitRenderThread);
				spdlog::info("EV_InitRenderThread issued");
			}
			eglMakeCurrent(dpy, pbuf, pbuf, ctx);

			warp_to_window = true;
			rt_inited = true;

			Pvr_SetAsyncTimeWarp(1);
			atw_enabled = true;
			spdlog::info("ATW enabled");

			client->tracker.start();
			spdlog::info("tracker started after warp init");
		}
		else if (rt_inited && warp_to_window)
		{
			RenderEventFunc re = (RenderEventFunc)GetRenderEventFunc();
			eglMakeCurrent(dpy, win_surface, win_surface, warp_ctx);
			if (re)
			{
				re(EV_InitRenderThread);
				re(EV_Resume);
			}
			eglMakeCurrent(dpy, pbuf, pbuf, ctx);
			spdlog::info("warp re-pointed to new surface");
		}
	}
	else
	{
		spdlog::info("window surface destroyed");
	}
}

void pico_render_thread::blit_decoded_to_swap(std::shared_ptr<pico_decoded_frame> frames[2])
{
	for (int eye = 0; eye < 2; eye++)
	{
		auto & decoded = frames[eye];
		if (!decoded || !decoded->valid || !decoded->hardware_buffer)
			continue;

		AHardwareBuffer * hb = decoded->hardware_buffer;

		if (last_hb[eye] != hb)
		{
			if (eye_prev_egl_images[eye] != EGL_NO_IMAGE_KHR)
			{
				g_eglDestroyImageKHR(dpy, eye_prev_egl_images[eye]);
				eye_prev_egl_images[eye] = EGL_NO_IMAGE_KHR;
			}
			eye_prev_egl_images[eye] = eye_egl_images[eye];
			eye_egl_images[eye] = EGL_NO_IMAGE_KHR;
			last_hb[eye] = hb;
			last_frame_idx[eye] = decoded->frame_index;

			EGLClientBuffer client_buffer = g_eglGetNativeClientBufferANDROID(hb);
			if (client_buffer)
			{
				EGLint attrs[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
				eye_egl_images[eye] = g_eglCreateImageKHR(
					dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, client_buffer, attrs);
			}

			if (eye_egl_images[eye] != EGL_NO_IMAGE_KHR)
			{
				glBindTexture(GL_TEXTURE_EXTERNAL_OES, eye_textures[eye]);
				g_glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, eye_egl_images[eye]);
				glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
			}
		}
	}

	glBindFramebuffer(GL_FRAMEBUFFER, stream_fbo);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);

	for (int eye = 0; eye < 2; eye++)
	{
		if (eye_textures[eye] == 0)
			continue;

		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			GL_TEXTURE_2D, swap_tex[eye][swap_idx], 0);

		glViewport(0, 0, eye_width, eye_height);
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);

		if (client && frames[eye])
			client->blit_pipeline.draw(eye, eye_textures[eye],
				frames[eye]->foveation[eye], frames[eye]->width, frames[eye]->height);
	}

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void pico_render_thread::submit_to_warp(int slot_idx, uint64_t fence_wait_ns)
{
	if (slots[slot_idx].fence)
	{
		GLenum w = glClientWaitSync(slots[slot_idx].fence, GL_SYNC_FLUSH_COMMANDS_BIT, fence_wait_ns);
		if (w == GL_TIMEOUT_EXPIRED)
		{
			static std::atomic<int> fence_timeout_count{0};
			if (fence_timeout_count.fetch_add(1) % 300 == 0)
				spdlog::warn("fence timeout on slot {} (count={})", slot_idx, fence_timeout_count.load());
		}
	}

	PVR_CameraEndFrame(0, swap_tex[0][slot_idx]);
	PVR_CameraEndFrame(1, swap_tex[1][slot_idx]);

	for (int e = 0; e < 2; e++)
	{
		XrPosef pose = slots[slot_idx].pose[e];
		float n2 = pose.orientation.x * pose.orientation.x
		         + pose.orientation.y * pose.orientation.y
		         + pose.orientation.z * pose.orientation.z
		         + pose.orientation.w * pose.orientation.w;
		static XrQuaternionf last_good_q[2] = {{0,0,0,1}, {0,0,0,1}};
		if (n2 > 1e-6f)
		{
			float inv = 1.0f / std::sqrt(n2);
			pose.orientation.x *= inv;
			pose.orientation.y *= inv;
			pose.orientation.z *= inv;
			pose.orientation.w *= inv;
			last_good_q[e] = pose.orientation;
		}
		else
		{
			pose.orientation = last_good_q[e];
		}

		PvrPoseBlk blk;
		memset(&blk, 0, sizeof(blk));
		blk.v[0] = pose.orientation.x;
		blk.v[1] = pose.orientation.y;
		blk.v[2] = pose.orientation.z;
		blk.v[3] = pose.orientation.w;
		blk.v[4] = pose.position.x;
		blk.v[5] = pose.position.y;
		blk.v[6] = pose.position.z;
		PVR_ChangeRenderPose(e, 0, blk);
	}

	PVR_TimeWarpEvent(0);
}

void pico_render_thread::render_lobby()
{
	if (!client)
		return;

	static int frame_counter = 0;

	bool show_lobby = !client->streaming.load() || client->stream_ui_visible.load();
	if (!show_lobby)
		return;

	float h_orient[4], h_pos[3];
	client->tracker.get_head_pose(h_orient, h_pos);
	controller_sample cs[2];
	client->tracker.get_controllers(cs);

	if ((frame_counter++ % 120) == 0)
		spdlog::info("lobby head q=({:.3f},{:.3f},{:.3f},{:.3f}) p=({:.3f},{:.3f},{:.3f})",
			h_orient[0], h_orient[1], h_orient[2], h_orient[3],
			h_pos[0], h_pos[1], h_pos[2]);

	client->lobby.flush_pending_texture();

	glBindFramebuffer(GL_FRAMEBUFFER, stream_fbo);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);

	for (int eye = 0; eye < 2; eye++)
	{
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			GL_TEXTURE_2D, swap_tex[eye][swap_idx], 0);
		glViewport(0, 0, eye_width, eye_height);
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);

		client->lobby.draw(eye, h_orient, h_pos, cs, client->eye_fov[eye], 0.064f);
	}

	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	if (client->vm && client->activity)
	{
		bool any_hit = false;
		int hit_hand = -1;
		for (int h = 0; h < 2; h++)
		{
			bool has_hit = client->lobby.lobby_touch_x[h] >= 0;
			bool has_click = client->lobby.lobby_touch_down[h] || client->lobby.lobby_touch_pressed[h];
			if (has_hit || has_click)
			{
				any_hit = true;
				hit_hand = h;
				break;
			}
		}

		bool prev_any = (prev_touch_hand >= 0);
		bool state_changed = (any_hit != prev_any) || (hit_hand != prev_touch_hand);

		float tx = -1.0f, ty = -1.0f;
		bool tdown = false, tpressed = false;
		float tthumb = 0.0f;
		if (any_hit)
		{
			tx = client->lobby.lobby_touch_x[hit_hand];
			ty = client->lobby.lobby_touch_y[hit_hand];
			tdown = client->lobby.lobby_touch_down[hit_hand];
			tpressed = client->lobby.lobby_touch_pressed[hit_hand];
			tthumb = client->lobby.lobby_thumbstick_y[hit_hand];
		}

		if (state_changed || (any_hit && (tdown || tpressed)))
		{
			JNIEnv * env2 = nullptr;
			bool attached = false;
			if (client->vm->GetEnv((void **)&env2, JNI_VERSION_1_6) != JNI_OK)
			{
				if (client->vm->AttachCurrentThread(&env2, nullptr) == JNI_OK)
					attached = true;
			}

			if (env2 && client->activity)
			{
				jclass clazz = env2->GetObjectClass(client->activity);
				jmethodID method = env2->GetMethodID(clazz, "onLobbyTouch", "(FFZZF)V");
				if (method)
					env2->CallVoidMethod(client->activity, method, tx, ty, tdown, tpressed, tthumb);
				env2->DeleteLocalRef(clazz);
			}

			if (attached)
				client->vm->DetachCurrentThread();
		}
		prev_touch_hand = any_hit ? hit_hand : -1;
	}

	for (int e = 0; e < 2; e++)
	{
		slots[swap_idx].pose[e].orientation = {h_orient[0], h_orient[1], h_orient[2], h_orient[3]};
		slots[swap_idx].pose[e].position = {h_pos[0], h_pos[1], h_pos[2]};
	}

	if (slots[swap_idx].fence) glDeleteSync(slots[swap_idx].fence);
	slots[swap_idx].fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	glFlush();

	if (prev_swap_valid)
		submit_to_warp(prev_swap_idx, 5000000ULL);
	else
		submit_to_warp(swap_idx, 50000000ULL);

	prev_swap_idx = swap_idx;
	prev_swap_valid = true;
	swap_idx = (swap_idx + 1) % kSwapLen;
}

void pico_render_thread::run()
{
	init_egl();
	if (ctx == EGL_NO_CONTEXT)
	{
		spdlog::error("render thread: EGL init failed, exiting");
		return;
	}

	load_egl_procs();

	if (client)
	{
		client->blit_pipeline.init(eye_width, eye_height);
		client->lobby.init(eye_width, eye_height);
	}

	init_sdk();
	if (!sdk_inited)
	{
		spdlog::error("render thread: SDK init failed, exiting");
		return;
	}

	init_swapchain();

	pico_sched::pin_current_thread("render thread", 2, -8);

	bool warp_pinned = false;
	int frame = 0;

	constexpr int64_t target_frame_ns = 13888888LL; // 72Hz

	while (running.load())
	{
		int64_t t_start = now_ns();

		if (window_dirty.load())
			on_surface_changed();

		if (win_surface == EGL_NO_SURFACE)
		{
			usleep(5000);
			continue;
		}

		if (rt_inited && !warp_pinned && (frame % 8) == 0)
		{
			warp_pinned = pico_sched::try_pin_warp_thread();
			if (warp_pinned)
				pico_sched::pin_current_thread("render thread", 2, -8);
			else if (frame > 150)
				warp_pinned = true;
		}

		bool streaming = client && client->streaming.load() && !client->stream_ui_visible.load();

		if (streaming)
		{
			controller_sample cs[2];
			client->tracker.get_controllers(cs);

			bool both_down = cs[0].connected && cs[1].connected &&
				cs[0].touch[1] < 64 && cs[1].touch[1] < 64;
			bool prev_both = client->prev_thumbstick_down[0] && client->prev_thumbstick_down[1];

			if (both_down && !prev_both)
			{
				client->stream_ui_visible = !client->stream_ui_visible.load();
				spdlog::info("Stream UI toggled: {}", client->stream_ui_visible.load());

				if (client->vm && client->activity)
				{
					JNIEnv * env2 = nullptr;
					bool attached = false;
					if (client->vm->GetEnv((void **)&env2, JNI_VERSION_1_6) != JNI_OK)
					{
						if (client->vm->AttachCurrentThread(&env2, nullptr) == JNI_OK)
							attached = true;
					}
					if (env2 && client->activity)
					{
						jclass clazz = env2->GetObjectClass(client->activity);
						jmethodID method = env2->GetMethodID(clazz, "onLobbyTouch", "(FFZZF)V");
						if (method)
							env2->CallVoidMethod(client->activity, method, -1.0f, -1.0f, false, false, 0.0f);
						env2->DeleteLocalRef(clazz);
					}
					if (attached)
						client->vm->DetachCurrentThread();
				}
			}
			client->prev_thumbstick_down[0] = cs[0].touch[1] < 64;
			client->prev_thumbstick_down[1] = cs[1].touch[1] < 64;

			std::shared_ptr<pico_decoded_frame> frames[2];
			{
				std::lock_guard lock(client->decoded_frame_mutex);
				frames[0] = client->latest_decoded_frames[0];
				frames[1] = client->latest_decoded_frames[1];
			}

			if (frames[0] && frames[0]->valid && frames[1] && frames[1]->valid)
			{
				int64_t gap = (int64_t)frames[0]->frame_index - (int64_t)frames[1]->frame_index;
				if (gap > 15)
				{
					spdlog::warn("Stream 1 behind by {} frames, flushing decoder", gap);
					client->decoders[1]->flush();
				}
			}

			bool has_new_frame = false;
			for (int e = 0; e < 2; e++)
			{
				if (frames[e] && frames[e]->valid && frames[e]->hardware_buffer
				    && frames[e]->frame_index != last_frame_idx[e])
					has_new_frame = true;
			}

			if (has_new_frame || !prev_swap_valid)
			{
				bool first_frame = !prev_swap_valid;
				if (!first_frame)
					submit_to_warp(prev_swap_idx, 5000000ULL);

				blit_decoded_to_swap(frames);

				int newer_eye = 0;
				if (frames[0] && frames[0]->valid && frames[1] && frames[1]->valid)
					newer_eye = (frames[0]->frame_index >= frames[1]->frame_index) ? 0 : 1;

				for (int e = 0; e < 2; e++)
				{
					if (frames[e] && frames[e]->valid)
						g_stutter.on_pose_update(e, frames[e]->frame_index, frames[e]->server_pose[e]);
				}

				if (frames[newer_eye] && frames[newer_eye]->valid)
				{
					for (int e = 0; e < 2; e++)
						slots[swap_idx].pose[e] = frames[newer_eye]->server_pose[e];
				}
				else
				{
					for (int e = 0; e < 2; e++)
					{
						if (frames[e] && frames[e]->valid)
							slots[swap_idx].pose[e] = frames[e]->server_pose[e];
						else
							slots[swap_idx].pose[e] = {};
					}
				}

				if (slots[swap_idx].fence) glDeleteSync(slots[swap_idx].fence);
				slots[swap_idx].fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
				glFlush();

				if (first_frame)
					submit_to_warp(swap_idx, 50000000ULL);

				prev_swap_idx = swap_idx;
				prev_swap_valid = true;
				swap_idx = (swap_idx + 1) % kSwapLen;

				g_stutter.on_frame_begin(
					frames[0] ? frames[0]->frame_index : 0,
					frames[1] ? frames[1]->frame_index : 0);
				g_stutter.on_frame_end();
				g_stutter.log_summary();

				client->stats_frame_count++;
				int64_t now_ns_val = now_ns();
				if (client->stats_last_time == 0)
					client->stats_last_time = now_ns_val;
				int64_t elapsed = now_ns_val - client->stats_last_time;
				if (elapsed >= 500000000LL)
				{
					float dt = elapsed * 1e-9f;
					int fps = (int)(client->stats_frame_count / dt);
					client->stats_frame_count = 0;
					client->stats_last_time = now_ns_val;

					uint64_t rx = client->session ? client->session->bytes_received() : 0;
					uint64_t tx = client->session ? client->session->bytes_sent() : 0;
					float bw_rx = (float)(rx - client->stats_bytes_rx) / dt;
					float bw_tx = (float)(tx - client->stats_bytes_tx) / dt;
					client->stats_bytes_rx = rx;
					client->stats_bytes_tx = tx;

					client->stats_bandwidth_rx = 0.8f * client->stats_bandwidth_rx + 0.2f * bw_rx;
					client->stats_bandwidth_tx = 0.8f * client->stats_bandwidth_tx + 0.2f * bw_tx;

					int latency_ms = 0;
					if (client->stats_last_encode_begin > 0)
					{
						int64_t latency = now_ns_val - client->stats_last_encode_begin;
						if (latency > 0 && latency < 1000000000LL)
							latency_ms = (int)(latency / 1000000LL);
					}

					client->notify_stream_stats(fps, latency_ms,
						client->stats_bandwidth_rx * 8, client->stats_bandwidth_tx * 8, 50);
				}
			}
		}
		else
		{
			render_lobby();
		}

		frame++;

		if ((frame % 3600) == 0)
		{
			int64_t elapsed = now_ns() - t_start;
			spdlog::info("frame {} elapsed={:.1f}ms", frame, elapsed / 1e6f);
		}

		struct timespec deadline;
		clock_gettime(CLOCK_MONOTONIC, &deadline);
		int64_t deadline_ns = (int64_t)deadline.tv_sec * 1000000000LL + deadline.tv_nsec;
		deadline_ns += target_frame_ns;
		deadline.tv_sec = deadline_ns / 1000000000LL;
		deadline.tv_nsec = deadline_ns % 1000000000LL;
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr);
	}

	if (atw_enabled)
		Pvr_SetAsyncTimeWarp(0);

	if (win_surface != EGL_NO_SURFACE)
	{
		eglMakeCurrent(dpy, pbuf, pbuf, ctx);
		eglDestroySurface(dpy, win_surface);
		win_surface = EGL_NO_SURFACE;
	}
	if (cur_win)
	{
		ANativeWindow_release(cur_win);
		cur_win = nullptr;
	}

	for (int e = 0; e < 2; e++)
	{
		for (int i = 0; i < kSwapLen; i++)
		{
			if (swap_tex[e][i]) glDeleteTextures(1, &swap_tex[e][i]);
		}
		if (eye_textures[e]) glDeleteTextures(1, &eye_textures[e]);
		if (eye_egl_images[e] != EGL_NO_IMAGE_KHR) g_eglDestroyImageKHR(dpy, eye_egl_images[e]);
		if (eye_prev_egl_images[e] != EGL_NO_IMAGE_KHR) g_eglDestroyImageKHR(dpy, eye_prev_egl_images[e]);
	}
	for (int i = 0; i < kSwapLen; i++)
	{
		if (slots[i].fence) glDeleteSync(slots[i].fence);
	}
	if (stream_fbo) glDeleteFramebuffers(1, &stream_fbo);

	if (warp_ctx != EGL_NO_CONTEXT) eglDestroyContext(dpy, warp_ctx);
	if (ctx != EGL_NO_CONTEXT)
	{
		eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		eglDestroyContext(dpy, ctx);
	}
	if (pbuf != EGL_NO_SURFACE) eglDestroySurface(dpy, pbuf);
	eglTerminate(dpy);

	spdlog::info("render thread exited");
}
