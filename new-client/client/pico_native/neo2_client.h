#pragma once

/*
 * Main client for Pico Neo 2 WiVRn.
 * Manages connection lifecycle, network I/O, tracking, input,
 * video shard reassembly, and EGL frame display.
 */

#include "neo2_session.h"
#include "neo2_video_decoder.h"
#include "neo2_audio.h"
#include "neo2_blit.h"
#include "crypto.h"
#include "wivrn_packets.h"

#include <jni.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

struct neo2_controller_state
{
	float orientation[4]{};
	float position[3]{};
	int trigger = 0;
	int touch[2]{};
	int battery = 0;
	bool connected = false;
	bool button_a = false;
	bool button_b = false;
	bool grip = false;
	bool thumbstick_click = false;
	bool menu = false;
};

class neo2_client
{
public:
	JavaVM * vm = nullptr;
	jobject activity = nullptr;

	std::atomic<bool> running{false};
	std::atomic<bool> shutdown{false};

	std::unique_ptr<neo2_session> session;

	// GLES
	neo2_blit_pipeline blit_pipeline;
	int eye_width = 2048;
	int eye_height = 2048;
	bool gl_initialized = false;

	// Pico Neo 2: 101 deg FOV -> ~50.5 deg per side
	XrFovf eye_fov[2]{
		XrFovf{-0.8814f, 0.8814f, 0.8814f, -0.8814f},
		XrFovf{-0.8814f, 0.8814f, 0.8814f, -0.8814f},
	};

	// Tracking
	std::mutex tracking_mutex;
	wivrn::to_headset::tracking_control tracking_control;
	bool tracking_control_received = false;

	// Video
	std::mutex video_mutex;
	std::optional<wivrn::to_headset::video_stream_description> video_desc;
	bool video_ready = false;

	std::unique_ptr<neo2_video_decoder> decoders[3];

	struct shard_set
	{
		uint64_t frame_index = 0;
		std::vector<std::optional<wivrn::to_headset::video_stream_data_shard>> shards;
		size_t min_for_reconstruction = 0;
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

	// Decoded frames
	std::mutex decoded_frame_mutex;
	std::shared_ptr<neo2_decoded_frame> latest_decoded_frames[3];
	std::atomic<uint64_t> latest_decoded_frame_index{0};

	// Per-eye GL textures
	GLuint eye_textures[3]{0, 0, 0};
	EGLImageKHR eye_egl_images[3]{EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR};
	AHardwareBuffer * last_hb[3]{nullptr, nullptr, nullptr};
	std::shared_ptr<neo2_decoded_frame> eye_current_frames[3];

	// Audio
	std::mutex audio_mutex;
	std::optional<wivrn::to_headset::audio_stream_description> audio_desc;
	std::unique_ptr<neo2_audio> audio_handle;

	// Haptics
	std::mutex haptics_mutex;
	std::vector<wivrn::to_headset::haptics> pending_haptics;

	std::atomic<bool> microphone_enabled{false};
	std::atomic<int64_t> time_offset_ns{0};

	// Connection
	std::string server_host;
	int server_port = 0;
	bool tcp_only = false;
	std::string pairing_pin;
	crypto::key headset_keypair;

	std::thread network_thread;
	std::thread tracking_thread;
	std::thread connect_thread;
	std::mutex connect_mutex;

	// Tracking data from Java callbacks
	std::mutex frame_mutex;
	float head_orientation[4]{};
	float head_position[3]{};
	bool has_head_pose = false;
	neo2_controller_state controllers[2];

	// EGL extension procs
	PFNEGLCREATEIMAGEKHRPROC egl_create_image = nullptr;
	PFNEGLDESTROYIMAGEKHRPROC egl_destroy_image = nullptr;
	PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC egl_get_native_buffer = nullptr;
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC gl_egl_image_target = nullptr;

	neo2_client() = default;

	~neo2_client()
	{
		shutdown = true;
		running = false;
		if (connect_thread.joinable()) connect_thread.join();
		if (network_thread.joinable()) network_thread.join();
		if (tracking_thread.joinable()) tracking_thread.join();
	}

	void load_egl_procs();

	void setup_decoders();
	void setup_audio();
	bool connect_to_server();
	void try_connect();
	void send_headset_info();
	void network_loop();
	void tracking_loop();

	void handle_packet(wivrn::to_headset::packets & packet);
	void handle_video_shard(wivrn::to_headset::video_stream_data_shard && shard);
	void send_tracking();
	void send_inputs();

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

extern neo2_client * g_neo2_client;
