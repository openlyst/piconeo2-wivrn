#pragma once

#include "wivrn_client_pico.h"
#include "pico_blit.h"
#include "pico_decoder.h"
#include "pico_audio.h"
#include "wivrn_packets.h"
#include "crypto.h"
#include "oxr_tracking.h"

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>
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
	int eye_width = 1664;
	int eye_height = 1664;
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

	static constexpr size_t FRAME_BUFFER_SIZE = 3;

	std::mutex decoded_frame_mutex;
	std::array<std::shared_ptr<pico_decoded_frame>, FRAME_BUFFER_SIZE> decoded_frame_buffers[3];
	std::atomic<uint64_t> latest_decoded_frame_index{0};

	std::shared_ptr<pico_decoded_frame> get_frame(uint64_t frame_index, int stream) const
	{
		auto &buf = decoded_frame_buffers[stream];
		auto &slot = buf[frame_index % buf.size()];
		if (slot && slot->valid && slot->frame_index == frame_index)
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

	oxr_tracker tracker;

	streaming_client();
	~streaming_client();

	void setup_decoders();
	void setup_audio();
	bool connect_to_server();
	void try_connect();
	void send_headset_info();
	void network_loop();
	void reset_stream_state();

	void notify_connection_state(int state, const std::string & msg);
	void notify_stream_stats(int fps, int latency_ms, float bandwidth_rx, float bandwidth_tx, int bitrate_mbps);

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
