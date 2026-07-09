#pragma once

#include "wivrn_client_pico.h"
#include "pico_blit.h"
#include "pico_decoder.h"
#include "pico_audio.h"
#include "wivrn_packets.h"
#include "crypto.h"
#include "oxr_tracking.h"

#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <android/hardware_buffer.h>
#include <jni.h>
#include <openxr/openxr.h>

#include <atomic>
#include <array>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

struct streaming_client
{
	JavaVM * vm = nullptr;
	jobject activity = nullptr;

	std::atomic<bool> shutdown{false};

	std::unique_ptr<wivrn_session_pico> session;

	pico_blit_pipeline blit_pipeline;
	std::atomic<int> eye_width{1664};
	std::atomic<int> eye_height{1756};
	std::atomic<int> stream_eye_width{1664};
	std::atomic<int> stream_eye_height{1756};
	std::atomic<bool> streaming{false};
	std::atomic<bool> stream_ui_visible{false};

	std::atomic<int64_t> last_shard_ns{0};
	std::atomic<int64_t> connected_ns{0};

	uint64_t stats_bytes_rx = 0;
	uint64_t stats_bytes_tx = 0;
	int64_t stats_last_time = 0;
	int stats_frame_count = 0;
	int stats_fps = 0;
	float stats_bandwidth_rx = 0;
	float stats_bandwidth_tx = 0;
	int64_t stats_last_encode_begin = 0;

	static constexpr float k_pico_fov_half = 101.0f * 0.5f * 0.01745329252f;
	XrFovf eye_fov[2]{
		XrFovf{-k_pico_fov_half, k_pico_fov_half, k_pico_fov_half, -k_pico_fov_half},
		XrFovf{-k_pico_fov_half, k_pico_fov_half, k_pico_fov_half, -k_pico_fov_half},
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
		int64_t t_first_shard_ns = 0;
		int64_t t_last_shard_ns = 0;
		bool has_server_timing = false;
		int64_t encode_begin = 0;
		int64_t encode_end = 0;
		int64_t send_begin = 0;
		int64_t send_end = 0;

		void reset(uint64_t idx)
		{
			frame_index = idx;
			shards.clear();
			min_for_reconstruction = 0;
			has_view_info = false;
			t_first_shard_ns = 0;
			t_last_shard_ns = 0;
			has_server_timing = false;
		}
	};
	shard_set current_shards[3];
	shard_set next_shards[3];

	static constexpr size_t FRAME_BUFFER_SIZE = 5;

	std::mutex decoded_frame_mutex;
	std::array<std::shared_ptr<pico_decoded_frame>, FRAME_BUFFER_SIZE> decoded_frame_buffers[3];
	std::atomic<uint64_t> latest_decoded_frame_index{0};
	std::atomic<uint64_t> latest_decoded_frame_index_per_stream[3]{};

	std::shared_ptr<pico_decoded_frame> get_frame(uint64_t frame_index, int stream) const
	{
		auto &buf = decoded_frame_buffers[stream];
		auto &slot = buf[frame_index % buf.size()];
		if (slot && slot->valid && slot->frame_index == frame_index)
			return slot;
		return nullptr;
	}

	std::shared_ptr<pico_decoded_frame> get_latest_frame(int stream) const
	{
		uint64_t idx = latest_decoded_frame_index_per_stream[stream].load(std::memory_order_acquire);
		if (idx == 0)
			return nullptr;
		auto &buf = decoded_frame_buffers[stream];
		auto &slot = buf[idx % buf.size()];
		if (slot && slot->valid && slot->frame_index == idx)
			return slot;
		return nullptr;
	}

	GLuint eye_textures[3]{0, 0, 0};
	EGLImageKHR eye_egl_images[3]{EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR};
	EGLImageKHR eye_prev_egl_images[3]{EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR};
	AHardwareBuffer * last_hb[3]{nullptr, nullptr, nullptr};

	GLuint stream_fbo = 0;

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
	std::atomic<bool> auto_reconnect{false};
	std::atomic<int64_t> time_offset_ns{0};
	std::atomic<int> bitrate_mbps{50};
	std::atomic<bool> dynamic_bitrate_enabled{true};
	std::atomic<int> max_bitrate_mbps{50};
	std::atomic<int> current_bitrate_mbps{50};

	int64_t db_last_check_ns = 0;
	int64_t db_prev_shard_ns = 0;

	std::string server_host;
	int server_port = 0;
	bool tcp_only = false;
	std::string pairing_pin;
	std::string last_error;

	crypto::key headset_keypair;

	std::promise<std::string> pin_promise;

	std::thread network_thread;
	std::thread connect_thread;
	std::mutex connect_mutex;

	oxr_tracker tracker;

	streaming_client();
	~streaming_client();

	void setup_decoders();
	void setup_audio();
	bool connect_to_server();
	void try_connect();
	void run_connect_loop();
	void send_headset_info();
	void network_loop();
	void reset_stream_state();
	void update_dynamic_bitrate();
	void send_bitrate_change(int mbps);

	void notify_connection_state(int state, const std::string & msg);
	void notify_stream_stats(int fps, int latency_ms, float bandwidth_rx, float bandwidth_tx, int bitrate_mbps);
	void notify_application_list(const std::vector<std::pair<std::string, std::string>> & apps);
	void notify_application_icon(const std::string & app_id, const std::vector<std::byte> & png_data);
	void notify_running_applications(const std::vector<to_headset::running_applications::application> & apps);

	void handle_packet(wivrn::to_headset::packets & packet);
	void handle_video_shard(wivrn::to_headset::video_stream_data_shard && shard);

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

extern streaming_client * g_stream;

extern PFNEGLCREATEIMAGEKHRPROC g_eglCreateImageKHR;
extern PFNEGLDESTROYIMAGEKHRPROC g_eglDestroyImageKHR;
extern PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC g_eglGetNativeClientBufferANDROID;
extern PFNGLEGLIMAGETARGETTEXTURE2DOESPROC g_glEGLImageTargetTexture2DOES;
void load_egl_procs();
