#pragma once

#include "wivrn_client_pico.h"
#include "pico_blit.h"
#include "pico_lobby.h"
#include "pico_tracking.h"
#include "pico_decoder.h"
#include "pico_audio.h"
#include "wivrn_packets.h"
#include "crypto.h"
#include "pico_sdk.h"

#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <android/hardware_buffer.h>
#include <jni.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

struct pico_client
{
	JavaVM * vm = nullptr;
	jobject activity = nullptr;

	std::atomic<bool> running{false};
	std::atomic<bool> shutdown{false};
	std::atomic<bool> pvr_initialized{false};

	std::unique_ptr<wivrn_session_pico> session;

	pico_blit_pipeline blit_pipeline;
	pico_lobby lobby;
	int eye_width = 1664;
	int eye_height = 1664;
	bool gl_initialized = false;
	std::atomic<bool> streaming{false};
	std::atomic<bool> stream_ui_visible{false};
	bool prev_thumbstick_click[2] = {false, false};
	bool prev_thumbstick_down[2] = {false, false};

	uint64_t stats_bytes_rx = 0;
	uint64_t stats_bytes_tx = 0;
	int64_t stats_last_time = 0;
	int stats_frame_count = 0;
	int stats_fps = 0;
	float stats_bandwidth_rx = 0;
	float stats_bandwidth_tx = 0;
	int64_t stats_last_latency = 0;
	int64_t stats_last_encode_begin = 0;

	XrFovf eye_fov[2]{
		XrFovf{-0.8814f, 0.8814f, 0.8814f, -0.8814f},
		XrFovf{-0.8814f, 0.8814f, 0.8814f, -0.8814f},
	};

	std::mutex tracking_mutex;
	wivrn::to_headset::tracking_control tracking_control;
	bool tracking_control_received = false;

	std::mutex video_mutex;
	std::optional<wivrn::to_headset::video_stream_description> video_desc;
	bool video_ready = false;

	std::unique_ptr<pico_video_decoder> decoders[3];

	struct shard_set
	{
		uint64_t frame_index = 0;
		std::vector<std::optional<wivrn::to_headset::video_stream_data_shard>> shards;
		size_t min_for_reconstruction = 0;
		wivrn::from_headset::feedback feedback{};
		wivrn::to_headset::video_stream_data_shard::view_info_t view_info{};
		bool has_view_info = false;

		void reset(uint64_t idx)
		{
			frame_index = idx;
			shards.clear();
			min_for_reconstruction = 0;
			has_view_info = false;
		}
	};
	shard_set current_shards[3];
	shard_set next_shards[3];

	std::mutex decoded_frame_mutex;
	std::shared_ptr<pico_decoded_frame> latest_decoded_frames[3];
	std::atomic<uint64_t> latest_decoded_frame_index{0};

	std::shared_ptr<pico_decoded_frame> render_frames[2];

	GLuint eye_textures[3]{0, 0, 0};
	EGLImageKHR eye_egl_images[3]{EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR};
	EGLImageKHR eye_prev_egl_images[3]{EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR};
	AHardwareBuffer * last_hb[3]{nullptr, nullptr, nullptr};
	std::shared_ptr<pico_decoded_frame> eye_current_frames[3];

	static constexpr int kSwapLen = 5;
	GLuint swap_tex[2][kSwapLen]{{0}};
	int swap_idx = 0;
	GLuint stream_fbo = 0;
	bool atw_enabled = false;

	std::mutex audio_mutex;
	std::optional<wivrn::to_headset::audio_stream_description> audio_desc;
	std::unique_ptr<pico_audio> audio_handle;

	struct rumble_slot
	{
		float amplitude = 0.f;
		int duration_ms = 0;
		bool active = false;
	};
	std::mutex haptics_mutex;
	rumble_slot rumble[2];

	std::atomic<bool> microphone_enabled{false};
	std::atomic<int64_t> time_offset_ns{0};

	std::string server_host;
	int server_port = 0;
	bool tcp_only = false;
	std::string pairing_pin;

	crypto::key headset_keypair;

	std::promise<std::string> pin_promise;

	std::thread network_thread;
	std::thread connect_thread;
	std::mutex connect_mutex;

	pico_native_tracker tracker;

	~pico_client();

	void setup_decoders();
	void setup_audio();
	bool connect_to_server();
	void try_connect();
	void send_headset_info();
	void network_loop();

	void notify_connection_state(int state, const std::string & msg);
	void notify_application_list(const std::vector<std::pair<std::string, std::string>> & apps);
	void notify_application_icon(const std::string & app_id, const std::vector<std::byte> & png_data);
	void notify_running_applications(const std::vector<to_headset::running_applications::application> & apps);
	void notify_stream_stats(int fps, int latency_ms, float bandwidth_rx, float bandwidth_tx, int bitrate_mbps);

	void handle_packet(to_headset::packets & packet);
	void handle_video_shard(to_headset::video_stream_data_shard && shard);
	void send_feedback(uint64_t frame_index);

	int64_t get_timestamp_ns()
	{
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
	}

	XrTime to_xr_time(int64_t headset_ns)
	{
		return headset_ns + time_offset_ns.load();
	}
};

extern pico_client * g_client;

extern PFNEGLCREATEIMAGEKHRPROC g_eglCreateImageKHR;
extern PFNEGLDESTROYIMAGEKHRPROC g_eglDestroyImageKHR;
extern PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC g_eglGetNativeClientBufferANDROID;
extern PFNGLEGLIMAGETARGETTEXTURE2DOESPROC g_glEGLImageTargetTexture2DOES;

void load_egl_procs();

std::optional<std::string> get_server_uri_from_intent(JNIEnv * env, jobject intent);
std::pair<std::string, int> parse_uri(const std::string & uri);
std::optional<std::string> parse_pin_from_uri(const std::string & uri);
