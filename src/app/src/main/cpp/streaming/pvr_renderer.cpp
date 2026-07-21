#include "pvr_renderer.h"
#include "streaming_client.h"
#include "pico_sdk.h"
#include "pico_sched.h"
#include "pico_stutter.h"
#include "pico_decoder.h"
#include "pico_blit.h"
#include "pico_tracking.h"
#include "latency_tracker.h"

#include <spdlog/spdlog.h>
#include <android/hardware_buffer.h>
#include <cstring>
#include <cmath>
#include <unistd.h>
#include <sys/system_properties.h>

#ifndef EGL_CONTEXT_PRIORITY_LEVEL_IMG
#define EGL_CONTEXT_PRIORITY_LEVEL_IMG 0x3100
#define EGL_CONTEXT_PRIORITY_HIGH_IMG  0x3101
#define EGL_CONTEXT_PRIORITY_MEDIUM_IMG 0x3102
#define EGL_CONTEXT_PRIORITY_LOW_IMG   0x3103
#endif

extern PFNEGLCREATEIMAGEKHRPROC g_eglCreateImageKHR;
extern PFNEGLDESTROYIMAGEKHRPROC g_eglDestroyImageKHR;
extern PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC g_eglGetNativeClientBufferANDROID;
extern PFNGLEGLIMAGETARGETTEXTURE2DOESPROC g_glEGLImageTargetTexture2DOES;
extern stutter_detector g_stutter;

pvr_renderer::~pvr_renderer()
{
	deactivate();
}

void pvr_renderer::set_window(ANativeWindow * window)
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

void pvr_renderer::init_egl()
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
		spdlog::error("pvr_renderer: eglChooseConfig failed");
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
		const EGLint fb[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
		ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, fb);
	}

	const EGLint pbufAttribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
	pbuf = eglCreatePbufferSurface(dpy, cfg, pbufAttribs);
	eglMakeCurrent(dpy, pbuf, pbuf, ctx);

	spdlog::info("pvr_renderer: EGL initialized");
}

bool pvr_renderer::init_pvr_sdk()
{
	if (!vm || !activity)
		return false;

	JNIEnv * env = nullptr;
	bool attached = false;
	if (vm->GetEnv((void **)&env, JNI_VERSION_1_6) != JNI_OK)
	{
		if (vm->AttachCurrentThread(&env, nullptr) == JNI_OK)
			attached = true;
	}

	jclass vrClass = nullptr;
	if (env && activity)
	{
		jclass actClass = env->GetObjectClass(activity);
		vrClass = (jclass)env->NewGlobalRef(actClass);
		env->DeleteLocalRef(actClass);
	}

	Pvr_SetInitActivity((void *)activity, (void *)vrClass);
	Pvr_Enable6DofModule(true);
	int initRc = Pvr_Init(0);
	int sensRc = InitSensor();
	int startRc = Pvr_StartSensor(0);
	spdlog::info("pvr_renderer: Pvr_Init={} InitSensor={} Pvr_StartSensor={}",
		initRc, sensRc, startRc);

	bool guardian = Pvr_BoundaryGetConfigured();
	int originType = guardian ? 2 : 1;
	bool originRc = Pvr_SetTrackingOriginType(originType);
	spdlog::info("pvr_renderer: Pvr_SetTrackingOriginType({})={} floorHeight={:.3f} guardian={}",
		guardian ? "StageLevel" : "FloorLevel", originRc, Pvr_GetFloorHeight(), guardian);

	Pvr_DisableBoundary();
	Pvr_ShutdownSDKBoundary();

	float fov = 101.0f;
	Pvr_SetProjectionFov(fov, fov);
	spdlog::info("pvr_renderer: FOV set to {:.1f}", fov);

	sdk_inited = true;

	if (attached)
		vm->DetachCurrentThread();

	return initRc == 0;
}

void pvr_renderer::init_swapchain()
{
	for (int e = 0; e < 2; e++)
	{
		glGenTextures(kSwapLen, swap_tex[e]);
		for (int i = 0; i < kSwapLen; i++)
		{
			glBindTexture(GL_TEXTURE_2D, swap_tex[e][i]);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, eye_width, eye_height,
				0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
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

	spdlog::info("pvr_renderer: swapchain initialized {}x{}", eye_width, eye_height);
}

bool pvr_renderer::init_warp()
{
	if (!cur_win || win_surface == EGL_NO_SURFACE)
		return false;

	Pvr_SetSinglePassDepthBufferWidthHeight(win_w / 2, win_h);

	const EGLint ctxAttribsHigh[] = {
		EGL_CONTEXT_CLIENT_VERSION, 3,
		EGL_CONTEXT_PRIORITY_LEVEL_IMG, EGL_CONTEXT_PRIORITY_HIGH_IMG,
		EGL_NONE
	};
	warp_ctx = eglCreateContext(dpy, cfg, ctx, ctxAttribsHigh);
	if (warp_ctx == EGL_NO_CONTEXT)
	{
		const EGLint fb[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
		warp_ctx = eglCreateContext(dpy, cfg, ctx, fb);
	}

	eglMakeCurrent(dpy, win_surface, win_surface, warp_ctx);

	typedef void (*RenderEventFunc)(int);
	RenderEventFunc re = (RenderEventFunc)GetRenderEventFunc();
	if (re)
	{
		re(EV_InitRenderThread);
		spdlog::info("pvr_renderer: EV_InitRenderThread issued");
	}
	eglMakeCurrent(dpy, pbuf, pbuf, ctx);

	Pvr_SetAsyncTimeWarp(1);
	atw_enabled = true;
	spdlog::info("pvr_renderer: ATW enabled");

	return true;
}

void pvr_renderer::shutdown_warp()
{
	if (atw_enabled)
	{
		Pvr_SetAsyncTimeWarp(0);
		atw_enabled = false;
	}

	if (warp_inited && win_surface != EGL_NO_SURFACE)
	{
		typedef void (*RenderEventFunc)(int);
		RenderEventFunc re = (RenderEventFunc)GetRenderEventFunc();
		if (re)
			re(EV_Pause);
	}

	if (warp_ctx != EGL_NO_CONTEXT)
	{
		eglDestroyContext(dpy, warp_ctx);
		warp_ctx = EGL_NO_CONTEXT;
	}

	warp_inited = false;
}

void pvr_renderer::on_window_changed()
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
			spdlog::error("pvr_renderer: eglCreateWindowSurface failed 0x{:x}", eglGetError());
			return;
		}

		eglQuerySurface(dpy, win_surface, EGL_WIDTH, &win_w);
		eglQuerySurface(dpy, win_surface, EGL_HEIGHT, &win_h);
		spdlog::info("pvr_renderer: window surface ready {}x{}", win_w, win_h);

		if (!warp_inited)
		{
			if (init_warp())
			{
				warp_inited = true;
				spdlog::info("pvr_renderer: warp initialized");
			}
		}
		else
		{
			typedef void (*RenderEventFunc)(int);
			eglMakeCurrent(dpy, win_surface, win_surface, warp_ctx);
			RenderEventFunc re = (RenderEventFunc)GetRenderEventFunc();
			if (re)
			{
				re(EV_InitRenderThread);
				re(EV_Resume);
			}
			eglMakeCurrent(dpy, pbuf, pbuf, ctx);
			spdlog::info("pvr_renderer: warp re-pointed to new surface");
		}
	}
	else
	{
		spdlog::info("pvr_renderer: window surface destroyed");
	}
}

void pvr_renderer::blit_decoded_to_swap(std::shared_ptr<pico_decoded_frame> frames[2])
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
				EGLint attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
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
		glClear(GL_COLOR_ATTACHMENT0);

		if (stream && frames[eye] && frames[eye]->valid)
			stream->blit_pipeline.draw(eye, eye_textures[eye],
				frames[eye]->foveation[eye], frames[eye]->width, frames[eye]->height);
	}

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void pvr_renderer::submit_to_warp(int slot_idx, uint64_t fence_wait_ns)
{
	if (slots[slot_idx].fence)
	{
		GLenum w = glClientWaitSync(slots[slot_idx].fence,
			GL_SYNC_FLUSH_COMMANDS_BIT, fence_wait_ns);
		if (w == GL_TIMEOUT_EXPIRED)
		{
			static std::atomic<int> fence_timeout_count{0};
			if (fence_timeout_count.fetch_add(1) % 300 == 0)
				spdlog::warn("pvr_renderer: fence timeout on slot {} (count={})",
					slot_idx, fence_timeout_count.load());
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

		float ipd = stream ? stream->tracker.soft_ipd.load() : 0.064f;
		float eye_offset = (e == 0 ? -ipd * 0.5f : ipd * 0.5f);

		PvrPoseBlk blk;
		memset(&blk, 0, sizeof(blk));
		blk.v[0] = pose.orientation.x;
		blk.v[1] = pose.orientation.y;
		blk.v[2] = pose.orientation.z;
		blk.v[3] = pose.orientation.w;
		blk.v[4] = eye_offset;
		blk.v[5] = 0;
		blk.v[6] = 0;
		PVR_ChangeRenderPose(e, 0, blk);
	}

	PVR_TimeWarpEvent(0);
}

bool pvr_renderer::activate(ANativeWindow * window, streaming_client * s,
                            JavaVM * jvm, jobject act)
{
	if (active.load())
		return true;

	stream = s;
	vm = jvm;
	activity = act;

	if (stream)
	{
		eye_width = stream->eye_width.load();
		eye_height = stream->eye_height.load();
	}

	init_egl();
	if (ctx == EGL_NO_CONTEXT)
	{
		spdlog::error("pvr_renderer: EGL init failed");
		return false;
	}

	if (!init_pvr_sdk())
	{
		spdlog::error("pvr_renderer: PVR SDK init failed, falling back to OpenXR");
		if (pbuf != EGL_NO_SURFACE) eglDestroySurface(dpy, pbuf);
		if (ctx != EGL_NO_CONTEXT) eglDestroyContext(dpy, ctx);
		eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		eglTerminate(dpy);
		dpy = EGL_NO_DISPLAY;
		return false;
	}

	init_swapchain();

	if (window)
		set_window(window);

	running = true;
	active = true;
	thread = std::thread([this] { run(); });

	stream->tracker.pvr_sensor_mode.store(true);
	spdlog::info("pvr_renderer: activated, PVR sensor mode enabled");
	return true;
}

void pvr_renderer::deactivate()
{
	if (!active.load())
		return;

	running = false;
	if (thread.joinable())
		thread.join();

	stream->tracker.pvr_sensor_mode.store(false);

	shutdown_warp();

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
	{
		std::lock_guard lk(win_mutex);
		if (pending_window)
		{
			ANativeWindow_release(pending_window);
			pending_window = nullptr;
		}
	}

	for (int e = 0; e < 2; e++)
	{
		for (int i = 0; i < kSwapLen; i++)
		{
			if (swap_tex[e][i]) glDeleteTextures(1, &swap_tex[e][i]);
		}
		if (eye_textures[e]) glDeleteTextures(1, &eye_textures[e]);
		if (eye_egl_images[e] != EGL_NO_IMAGE_KHR)
			g_eglDestroyImageKHR(dpy, eye_egl_images[e]);
		if (eye_prev_egl_images[e] != EGL_NO_IMAGE_KHR)
			g_eglDestroyImageKHR(dpy, eye_prev_egl_images[e]);
		last_hb[e] = nullptr;
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
	if (dpy != EGL_NO_DISPLAY) eglTerminate(dpy);

	dpy = EGL_NO_DISPLAY;
	ctx = EGL_NO_CONTEXT;
	warp_ctx = EGL_NO_CONTEXT;
	pbuf = EGL_NO_SURFACE;
	sdk_inited = false;
	warp_inited = false;

	active = false;
	spdlog::info("pvr_renderer: deactivated");
}

void pvr_renderer::run()
{
	pico_sched::pin_current_thread("pvr-render", 2, -8);

	bool warp_pinned = false;
	int frame = 0;

	int target_fps = 72;
	char prop_buf[8] = {};
	if (__system_property_get("persist.pvr.config.target_fps", prop_buf) > 0)
	{
		int val = atoi(prop_buf);
		if (val >= 60 && val <= 120)
			target_fps = val;
	}
	int64_t target_frame_ns = 1000000000LL / target_fps;
	spdlog::info("pvr_renderer: target {}Hz ({}ns)", target_fps, target_frame_ns);

	while (running.load())
	{
		int64_t t_start = now_ns();

		if (window_dirty.load())
			on_window_changed();

		if (win_surface == EGL_NO_SURFACE)
		{
			usleep(5000);
			continue;
		}

		if (!warp_inited)
		{
			usleep(5000);
			continue;
		}

		if (!warp_pinned && (frame % 8) == 0)
		{
			warp_pinned = pico_sched::try_pin_warp_thread();
			if (warp_pinned)
				pico_sched::pin_current_thread("pvr-render", 2, -8);
			else if (frame > 150)
				warp_pinned = true;
		}

		if (!stream || !stream->streaming.load() || stream->stream_ui_visible.load())
		{
			usleep(2000);
			frame++;
			continue;
		}

		controller_sample cs[2];
		stream->tracker.get_controllers(cs);

		bool both_down = cs[0].connected && cs[1].connected &&
			cs[0].thumbstick_click && cs[1].thumbstick_click;

		std::shared_ptr<pico_decoded_frame> frames[2];
		{
			uint64_t idx0 = stream->latest_decoded_frame_index_per_stream[0].load(
				std::memory_order_acquire);
			uint64_t idx1 = stream->latest_decoded_frame_index_per_stream[1].load(
				std::memory_order_acquire);

			if (idx0 > 0 && idx1 > 0)
			{
				uint64_t chosen = std::min(idx0, idx1);
				frames[0] = stream->get_frame(chosen, 0);
				frames[1] = stream->get_frame(chosen, 1);
				if (!frames[0] || !frames[1] || !frames[0]->valid || !frames[1]->valid)
				{
					frames[0] = stream->get_latest_frame(0);
					frames[1] = stream->get_latest_frame(1);
				}
			}
			else
			{
				frames[0] = stream->get_latest_frame(0);
				frames[1] = stream->get_latest_frame(1);
			}
		}

		if (frames[0] && frames[0]->valid && frames[1] && frames[1]->valid)
		{
			int64_t gap = (int64_t)frames[0]->frame_index - (int64_t)frames[1]->frame_index;
			if (gap > 15)
			{
				spdlog::warn("pvr_renderer: stream 1 behind by {} frames, flushing", gap);
				stream->decoders[1]->flush();
			}
		}

		int new_count = 0;
		for (int e = 0; e < 2; e++)
		{
			if (frames[e] && frames[e]->valid && frames[e]->hardware_buffer
			    && frames[e]->frame_index != last_frame_idx[e])
				new_count++;
		}

		bool has_new_frame = (new_count >= 1);

		if (has_new_frame || !prev_swap_valid)
		{
			static int render_log_count = 0;
			if (++render_log_count % 100 == 1)
			{
				uint64_t li = frames[0] ? frames[0]->frame_index : 0;
				uint64_t ri = frames[1] ? frames[1]->frame_index : 0;
				spdlog::info("pvr_renderer: RENDER L={} R={} new_count={}",
					li, ri, new_count);
			}

			bool first_frame = !prev_swap_valid;
			if (!first_frame)
				submit_to_warp(prev_swap_idx, 5000000ULL);

			blit_decoded_to_swap(frames);

			for (int e = 0; e < 2; e++)
			{
				if (frames[e] && frames[e]->valid)
				{
					g_stutter.on_pose_update(e, frames[e]->frame_index,
						frames[e]->server_pose[e]);
					slots[swap_idx].pose[e] = frames[e]->server_pose[e];
				}
				else
					slots[swap_idx].pose[e] = {};
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

			stream->stats_frame_count++;
			int64_t now_ns_val = now_ns();
			if (stream->stats_last_time == 0)
				stream->stats_last_time = now_ns_val;
			int64_t elapsed = now_ns_val - stream->stats_last_time;
			if (elapsed >= 500000000LL)
			{
				float dt = elapsed * 1e-9f;
				stream->stats_frame_count = 0;
				stream->stats_last_time = now_ns_val;

				uint64_t rx = stream->session ? stream->session->bytes_received() : 0;
				uint64_t tx = stream->session ? stream->session->bytes_sent() : 0;
				float bw_rx = (float)(rx - stream->stats_bytes_rx) / dt;
				float bw_tx = (float)(tx - stream->stats_bytes_tx) / dt;
				stream->stats_bytes_rx = rx;
				stream->stats_bytes_tx = tx;

				stream->stats_bandwidth_rx = 0.8f * stream->stats_bandwidth_rx + 0.2f * bw_rx;
				stream->stats_bandwidth_tx = 0.8f * stream->stats_bandwidth_tx + 0.2f * bw_tx;
			}
		}

		frame++;

		struct timespec deadline;
		clock_gettime(CLOCK_MONOTONIC, &deadline);
		int64_t deadline_ns = (int64_t)deadline.tv_sec * 1000000000LL + deadline.tv_nsec;
		deadline_ns += target_frame_ns;
		deadline.tv_sec = deadline_ns / 1000000000LL;
		deadline.tv_nsec = deadline_ns % 1000000000LL;
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr);
	}

	spdlog::info("pvr_renderer: render thread exited");
}
