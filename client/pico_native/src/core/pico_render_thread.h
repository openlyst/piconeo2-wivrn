#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <android/native_window.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <memory>
#include <openxr/openxr.h>

struct pico_client;
struct pico_decoded_frame;

struct swap_slot
{
	GLsync fence = nullptr;
	XrPosef pose[2]{};
};

class pico_render_thread
{
public:
	pico_render_thread();
	~pico_render_thread();

	void start(pico_client * client);
	void stop();

	void set_surface(ANativeWindow * window);
	void clear_surface();

private:
	pico_client * client = nullptr;

	std::thread thread;
	std::atomic<bool> running{false};

	std::mutex win_mutex;
	ANativeWindow * pending_window = nullptr;
	std::atomic<bool> window_dirty{false};

	EGLDisplay dpy = EGL_NO_DISPLAY;
	EGLConfig cfg = nullptr;
	EGLContext ctx = EGL_NO_CONTEXT;
	EGLSurface pbuf = EGL_NO_SURFACE;
	EGLContext warp_ctx = EGL_NO_CONTEXT;

	ANativeWindow * cur_win = nullptr;
	EGLSurface win_surface = EGL_NO_SURFACE;
	int win_w = 0, win_h = 0;

	bool sdk_inited = false;
	bool rt_inited = false;
	bool warp_to_window = false;
	bool atw_enabled = false;

	static constexpr int kSwapLen = 5;
	GLuint swap_tex[2][kSwapLen]{{0}};
	GLuint stream_fbo = 0;
	int swap_idx = 0;
	int prev_swap_idx = -1;
	bool prev_swap_valid = false;
	swap_slot slots[kSwapLen];

	GLuint eye_textures[2]{0, 0};
	EGLImageKHR eye_egl_images[2]{EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR};
	EGLImageKHR eye_prev_egl_images[2]{EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR};
	AHardwareBuffer * last_hb[2]{nullptr, nullptr};
	uint64_t last_frame_idx[2]{0, 0};
	int prev_touch_hand = -1;

	int eye_width = 1664;
	int eye_height = 1664;

	void run();
	void init_egl();
	void init_sdk();
	void init_swapchain();
	void on_surface_changed();
	void blit_decoded_to_swap(std::shared_ptr<pico_decoded_frame> frames[2]);
	void submit_to_warp(int slot_idx, uint64_t fence_wait_ns);
	void render_lobby();

	int64_t now_ns()
	{
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
	}
};
