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

#include "wivrn_client_pico.h"
#include "pico_decoder.h"
#include "pico_audio.h"
#include "pico_blit.h"
#include "pico_tracking.h"
#include "crypto.h"
#include "protocol_version.h"
#include "wivrn_packets.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/android_sink.h>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <jni.h>
#include <android/native_window.h>
#include <android/hardware_buffer.h>

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <optional>
#include <sys/stat.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <cmath>
#include <unistd.h>
#include <vector>

using namespace std::chrono_literals;
using namespace wivrn;

extern "C" {
bool Pvr_SetTrackingOriginType(int trackingOriginType);
void PVR_CameraEndFrame(unsigned int eye, unsigned int texId);
struct PvrPoseBlk { float v[22]; };
void PVR_ChangeRenderPose(unsigned int eye, unsigned int pad, struct PvrPoseBlk blk);
void Pvr_SetAsyncTimeWarp(unsigned char enable);
void PVR_TimeWarpEvent(unsigned int eye);
void *GetRenderEventFunc();
int   Pvr_Init(int index);
int   Pvr_StartSensor(int index);
int   Pvr_Enable6DofModule(bool enable);
int   InitSensor();
void  Pvr_SetInitActivity(void *activity, void *vrActivityClass);
void  Pvr_DisableBoundary();
void  Pvr_ShutdownSDKBoundary();
bool  Pvr_SetSinglePassDepthBufferWidthHeight(int width, int height);
float Pvr_GetIPD();
}

enum { EV_InitRenderThread = 1024, EV_Pause = 1025, EV_Resume = 1026 };
typedef void (*RenderEventFunc)(int);

namespace
{
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

struct pico_client
{
	JavaVM * vm = nullptr;
	JNIEnv * jni_env = nullptr;
	jobject activity = nullptr;

	std::atomic<bool> running{false};
	std::atomic<bool> shutdown{false};
	std::atomic<bool> pvr_initialized{false};

	std::unique_ptr<wivrn_session_pico> session;

	// GLES
	pico_blit_pipeline blit_pipeline;
	int eye_width = 2048;
	int eye_height = 2048;
	bool gl_initialized = false;

	// Pico Neo 2: 101 deg FOV, 50.50 deg per side (https://vr-compare.com/headset/piconeo2)
	XrFovf eye_fov[2]{
		XrFovf{-0.8814f, 0.8814f, 0.8814f, -0.8814f},
		XrFovf{-0.8814f, 0.8814f, 0.8814f, -0.8814f},
	};

	// Tracking control
	std::mutex tracking_mutex;
	wivrn::to_headset::tracking_control tracking_control;
	bool tracking_control_received = false;

	// Video stream info
	std::mutex video_mutex;
	std::optional<wivrn::to_headset::video_stream_description> video_desc;
	bool video_ready = false;

	// Decoders (left, right, alpha)
	std::unique_ptr<pico_video_decoder> decoders[3];

	// Shard accumulation
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

	// Decoded frames ready for display
	std::mutex decoded_frame_mutex;
	std::shared_ptr<pico_decoded_frame> latest_decoded_frames[3];
	std::atomic<uint64_t> latest_decoded_frame_index{0};

	// Per-stream GL textures (managed on render thread)
	GLuint eye_textures[3]{0, 0, 0};
	EGLImageKHR eye_egl_images[3]{EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR};
	AHardwareBuffer * last_hb[3]{nullptr, nullptr, nullptr};
	std::shared_ptr<pico_decoded_frame> eye_current_frames[3];

	// DIATW warp swapchain (GL_TEXTURE_2D textures submitted to PVR warp)
	static constexpr int kSwapLen = 3;
	GLuint swap_tex[2][kSwapLen]{{0}};
	int swap_idx = 0;
	GLuint stream_fbo = 0;
	bool atw_enabled = false;

	// Audio
	std::mutex audio_mutex;
	std::optional<wivrn::to_headset::audio_stream_description> audio_desc;
	std::unique_ptr<pico_audio> audio_handle;

	// Haptics
	std::mutex haptics_mutex;
	std::vector<wivrn::to_headset::haptics> pending_haptics;

	// Feature control
	std::atomic<bool> microphone_enabled{false};

	// Time offset between headset clock and server clock
	std::atomic<int64_t> time_offset_ns{0};

	std::string server_host;
	int server_port = 0;
	bool tcp_only = false;
	std::string pairing_pin;

	crypto::key headset_keypair;

	std::thread network_thread;
	std::thread connect_thread;
	std::mutex connect_mutex;

	pico_native_tracker tracker;

	~pico_client()
	{
		shutdown = true;
		running = false;

		tracker.stop();

		if (connect_thread.joinable())
			connect_thread.join();
		if (network_thread.joinable())
			network_thread.join();
	}

	void setup_decoders();
	void setup_audio();
	bool connect_to_server();
	void try_connect();
	void send_headset_info();
	void network_loop();

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

static pico_client * g_client = nullptr;

void pico_client::setup_decoders()
{
	if (!video_desc || decoders[0])
		return;

	for (uint8_t i = 0; i < 3; i++)
	{
		if (video_desc->codec[i] == video_codec::raw)
		{
			decoders[i].reset();
			continue;
		}

		decoders[i] = std::make_unique<pico_video_decoder>(
			*video_desc, i,
			[this, i](std::shared_ptr<pico_decoded_frame> frame) {
				std::lock_guard lock(decoded_frame_mutex);
				latest_decoded_frames[i] = std::move(frame);
				latest_decoded_frame_index.store(latest_decoded_frames[i]->frame_index);
			});
		spdlog::warn("Created decoder for stream {}", i);
	}

	for (int i = 0; i < 3; i++)
	{
		current_shards[i].reset(0);
		next_shards[i].reset(1);
	}
}

void pico_client::setup_audio()
{
	if (!audio_desc || audio_handle)
		return;

	audio_handle = std::make_unique<pico_audio>(*audio_desc, *session);
	spdlog::info("Audio initialized");
}

void pico_client::handle_video_shard(to_headset::video_stream_data_shard && shard)
{
	static int shard_count = 0;
	++shard_count;
	bool has_timing = shard.timing_info.has_value();
	if (shard_count <= 20 || shard_count % 100 == 0)
		spdlog::warn("Video shard #{}: stream={} frame={} shard_idx={} payload={} timing={}",
			shard_count, (int)shard.stream_item_idx, shard.frame_idx,
			shard.shard_idx, (int)shard.payload.size(), has_timing);

	uint8_t idx = shard.stream_item_idx;
	if (idx >= 3 || !decoders[idx])
		return;

	uint64_t frame_idx = shard.frame_idx;
	shard_set * target = nullptr;
	if (frame_idx == current_shards[idx].frame_index)
		target = &current_shards[idx];
	else if (frame_idx == next_shards[idx].frame_index)
		target = &next_shards[idx];
	else if (frame_idx > current_shards[idx].frame_index)
	{
		if (next_shards[idx].frame_index == frame_idx)
		{
			current_shards[idx] = std::move(next_shards[idx]);
		}
		else
		{
			current_shards[idx].reset(frame_idx);
		}
		next_shards[idx].reset(frame_idx + 1);
		target = &current_shards[idx];
	}
	else
	{
		spdlog::warn("Stale shard for frame {} on stream {}", frame_idx, idx);
		return;
	}

	if (shard.view_info)
	{
		target->view_info = *shard.view_info;
		target->has_view_info = true;
	}

	if (shard.shard_idx >= target->shards.size())
		target->shards.resize(shard.shard_idx + 1);

	bool is_last_shard = shard.timing_info.has_value();
	target->shards[shard.shard_idx] = std::move(shard);

	if (!is_last_shard)
	{
		if (shard_count <= 20)
			spdlog::warn("Shard not last (no timing_info), waiting for more shards");
		return;
	}

	size_t filled = 0;
	for (auto & s : target->shards)
		if (s) filled++;

	if (filled < target->min_for_reconstruction)
		return;

	bool complete = true;
	for (auto & s : target->shards)
		if (!s) { complete = false; break; }

	if (!complete)
		return;

	std::vector<std::span<const uint8_t>> data;
	for (auto & s : target->shards)
	{
		if (s)
			data.push_back(s->payload);
	}

	static int recon_count = 0;
	++recon_count;
	if (recon_count <= 20 || recon_count % 100 == 0)
	{
		size_t total_size = 0;
		for (auto & d : data) total_size += d.size();
		spdlog::warn("Frame reconstructed #{}: stream={} frame={} shards={} bytes={}",
			recon_count, idx, frame_idx, data.size(), total_size);
	}

	wivrn::from_headset::feedback feedback{};
	feedback.frame_index = frame_idx;
	feedback.stream_index = idx;

	int64_t now = get_timestamp_ns();
	feedback.received_first_packet = to_xr_time(now);
	feedback.received_last_packet = to_xr_time(now);
	feedback.sent_to_decoder = to_xr_time(now);

	decoders[idx]->push_data(data, frame_idx, false);

	feedback.received_from_decoder = to_xr_time(get_timestamp_ns());

	if (target->has_view_info)
		decoders[idx]->frame_completed(feedback, target->view_info);

	if (session)
		session->send_stream(feedback);
}

void pico_client::handle_packet(to_headset::packets & packet)
{
	std::visit([this](auto && p) {
		using T = std::decay_t<decltype(p)>;

		if constexpr (std::is_same_v<T, to_headset::video_stream_description>)
		{
			std::lock_guard lock(video_mutex);
			video_desc = p;
			video_ready = true;
			spdlog::warn("Received video stream description: {}x{}, fps={}",
				p.width, p.height, p.frame_rate);
			setup_decoders();
		}
		else if constexpr (std::is_same_v<T, to_headset::audio_stream_description>)
		{
			std::lock_guard lock(audio_mutex);
			audio_desc = p;
			spdlog::info("Received audio stream description");
			setup_audio();
		}
		else if constexpr (std::is_same_v<T, to_headset::tracking_control>)
		{
			std::lock_guard lock(tracking_mutex);
			tracking_control = p;
			tracking_control_received = true;
			spdlog::info("Received tracking control: {} samples, m2p={}ns",
				p.pattern.size(), p.motions_to_photons);
		}
		else if constexpr (std::is_same_v<T, to_headset::haptics>)
		{
			std::lock_guard lock(haptics_mutex);
			pending_haptics.push_back(p);
		}
		else if constexpr (std::is_same_v<T, to_headset::timesync_query>)
		{
			int64_t now = get_timestamp_ns();
			static int ts_count = 0;
			if (++ts_count % 50 == 1)
				spdlog::warn("Timesync query received #{}: query={}, response={}", ts_count, p.query, now);
			session->send_control(from_headset::timesync_response{
				.query = p.query,
				.response = now,
			});
		}
		else if constexpr (std::is_same_v<T, to_headset::feature_control>)
		{
			if (p.f == to_headset::feature_control::microphone)
			{
				microphone_enabled = p.state;
				if (audio_handle)
					audio_handle->set_mic_state(p.state);
			}
		}
		else if constexpr (std::is_same_v<T, to_headset::refresh_rate_change>)
		{
			spdlog::info("Refresh rate change requested: {} Hz (not supported on PvrSDK-Native)", p.hz);
		}
		else if constexpr (std::is_same_v<T, to_headset::server_message>)
		{
			spdlog::info("Server message: {}", p.msg);
		}
		else if constexpr (std::is_same_v<T, to_headset::video_stream_data_shard>)
		{
			handle_video_shard(std::move(p));
		}
		else if constexpr (std::is_same_v<T, wivrn::audio_data>)
		{
			if (audio_handle)
				(*audio_handle)(std::move(p));
		}
		else if constexpr (std::is_same_v<T, to_headset::crypto_handshake> ||
			std::is_same_v<T, to_headset::pin_check_2> ||
			std::is_same_v<T, to_headset::pin_check_4> ||
			std::is_same_v<T, to_headset::handshake>)
		{
			// Handled during handshake, ignore here
		}
		else
		{
			// Unhandled packet type - ignore
		}
	}, packet);
}

void pico_client::network_loop()
{
	while (!shutdown && session)
	{
		try
		{
			session->poll([this](to_headset::packets && packet) {
				handle_packet(packet);
			}, 500ms);
		}
		catch (std::exception & e)
		{
			spdlog::error("Network error: {}", e.what());
			break;
		}
		catch (...)
		{
			spdlog::error("Network error: unknown exception");
			break;
		}
	}

	if (!shutdown)
	{
		spdlog::info("Network loop ended, cleaning up session for reconnection");
		tracker.stop();
		tracker.session = nullptr;
		session.reset();
		if (connect_thread.joinable())
			connect_thread.join();
		if (!shutdown && !server_host.empty())
		{
			spdlog::info("Attempting reconnection to {}", server_host);
			try_connect();
		}
	}
}

void pico_client::send_feedback(uint64_t frame_index)
{
	if (!session)
		return;

	from_headset::feedback fb{};
	fb.frame_index = frame_index;
	fb.stream_index = 0;

	int64_t now = get_timestamp_ns();
	fb.encode_begin = to_xr_time(now);
	fb.encode_end = fb.encode_begin;
	fb.send_begin = fb.encode_begin;
	fb.send_end = fb.encode_begin;
	fb.received_first_packet = fb.encode_begin;
	fb.received_last_packet = fb.encode_begin;
	fb.sent_to_decoder = fb.encode_begin;
	fb.received_from_decoder = fb.encode_begin;
	fb.blitted = fb.encode_begin;
	fb.displayed = fb.encode_begin;
	fb.times_displayed = 1;

	session->send_stream(fb);
}

bool pico_client::connect_to_server()
{
	spdlog::info("Connecting to server {}:{}", server_host, server_port);

	try
	{
		const char * key_dir = "/data/data/org.meumeu.wivrn.neo2.local/files";
		const char * key_path = "/data/data/org.meumeu.wivrn.neo2.local/files/private_key.pem";
		spdlog::info("connect: loading keypair from {}", key_path);
		try
		{
			std::ifstream f{key_path};
			std::string key_str{(std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()};
			headset_keypair = crypto::key::from_private_key(key_str);
			spdlog::info("connect: loaded existing keypair");
		}
		catch (...)
		{
			spdlog::info("connect: generating new keypair");
			mkdir(key_dir, 0700);
			headset_keypair = crypto::key::generate_x448_keypair();
			std::ofstream f{key_path};
			f << headset_keypair.private_key();
			f.close();
			if (f.good())
				spdlog::info("connect: generated and saved new keypair");
			else
				spdlog::error("connect: failed to save keypair to {}", key_path);
		}
		std::string model_name = "Pico Neo2";

		auto pin_enter = [this](int fd) -> std::string {
			if (!pairing_pin.empty())
			{
				spdlog::warn("PIN entry requested - using {}", pairing_pin);
				return pairing_pin;
			}
			spdlog::warn("PIN entry requested - no PIN configured, using default 000000");
			return "000000";
		};

		spdlog::info("connect: resolving address");
		struct addrinfo hints{};
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;

		struct addrinfo * result = nullptr;
		int ret = getaddrinfo(server_host.c_str(), std::to_string(server_port).c_str(), &hints, &result);
		if (ret != 0)
		{
			spdlog::error("getaddrinfo failed: {}", gai_strerror(ret));
			return false;
		}
		spdlog::info("connect: address resolved, trying connections");

		for (struct addrinfo * rp = result; rp; rp = rp->ai_next)
		{
			try
			{
				if (rp->ai_family == AF_INET)
				{
					auto * addr = (struct sockaddr_in *)rp->ai_addr;
					in_addr ip = addr->sin_addr;
					spdlog::warn("connect: trying IPv4 {}", inet_ntoa(ip));

					spdlog::warn("connect: creating session object");
					try
					{
						session = std::make_unique<wivrn_session_pico>(
							ip, server_port, tcp_only, headset_keypair, model_name, pin_enter);
					}
					catch (std::exception & e)
					{
						spdlog::warn("connect: session creation failed (std::exception): {}", e.what());
					}
					catch (...)
					{
						spdlog::warn("connect: session creation failed (unknown exception, not rethrowing)");
					}
					if (session && session->is_handshake_ok())
					{
						freeaddrinfo(result);
						spdlog::info("Connected to server (IPv4)");
						return true;
					}
					if (session)
					{
						spdlog::warn("connect: handshake failed, resetting session");
						session.reset();
					}
				}
				else if (rp->ai_family == AF_INET6)
				{
					spdlog::info("connect: trying IPv6");
					auto * addr = (struct sockaddr_in6 *)rp->ai_addr;
					in6_addr ip = addr->sin6_addr;
					try
					{
						session = std::make_unique<wivrn_session_pico>(
							ip, server_port, tcp_only, headset_keypair, model_name, pin_enter);
					}
					catch (...)
					{
						spdlog::warn("connect: IPv6 session creation failed");
					}
					if (session && session->is_handshake_ok())
					{
						freeaddrinfo(result);
						spdlog::info("Connected to server (IPv6)");
						return true;
					}
					if (session)
						session.reset();
				}
			}
			catch (std::exception & e)
			{
				spdlog::warn("Connection attempt failed: {}", e.what());
			}
			catch (...)
			{
				spdlog::warn("Connection attempt failed: unknown exception");
			}
		}

		freeaddrinfo(result);
		spdlog::error("Failed to connect to server");
		return false;
	}
	catch (std::exception & e)
	{
		spdlog::error("connect_to_server error: {}", e.what());
		return false;
	}
	catch (...)
	{
		spdlog::error("connect_to_server error: unknown exception");
		return false;
	}
}

void pico_client::send_headset_info()
{
	from_headset::headset_info_packet info{};

	info.render_eye_width = eye_width;
	info.render_eye_height = eye_height;
	info.stream_eye_width = eye_width;
	info.stream_eye_height = eye_height;

	info.available_refresh_rates.push_back(72.0f);
	info.settings.preferred_refresh_rate = 72.0f;
	info.settings.minimum_refresh_rate = 72.0f;
	info.settings.fps_divider = 1;
	info.settings.bitrate_bps = 20000000;

	pico_audio::get_audio_description(info);

	info.fov[0] = eye_fov[0];
	info.fov[1] = eye_fov[1];

	info.hand_tracking = false;
	info.eye_gaze = false;
	info.palm_pose = false;
	info.user_presence = false;
	info.passthrough = false;
	info.face_tracking = from_headset::face_type::none;
	info.num_generic_trackers = 0;

	{
		std::vector<wivrn::video_codec> codecs;
		pico_video_decoder::supported_codecs(codecs);
		info.supported_codecs = codecs;
	}
	if (info.supported_codecs.empty())
		info.supported_codecs = {h264, h265};

	info.system_name = "Pico Neo2";
	info.language = "en";
	info.country = "US";
	info.variant = "";

	session->send_control(info);
	spdlog::warn("Sent headset info: {}x{}, {} codecs",
		info.render_eye_width, info.render_eye_height,
		info.supported_codecs.size());

	session->send_control(from_headset::session_state_changed{
		.state = XR_SESSION_STATE_FOCUSED,
	});
	spdlog::warn("Sent session_state_changed: FOCUSED");

	session->send_control(from_headset::stream_tab_changed{
		.tab = wivrn::stream_tab::hidden,
	});
	spdlog::warn("Sent stream_tab_changed: hidden");
}

void pico_client::try_connect()
{
	if (server_host.empty() || shutdown)
		return;

	std::lock_guard lock(connect_mutex);
	if (connect_thread.joinable())
	{
		if (session)
			return;
		connect_thread.join();
	}

	connect_thread = std::thread([this] {
		for (int attempt = 1; attempt <= 5 && !shutdown; ++attempt)
		{
			spdlog::info("Connection attempt {} of 5", attempt);

			if (connect_to_server())
			{
				try
				{
					send_headset_info();
					network_thread = std::thread([] { g_client->network_loop(); });
					tracker.session = session.get();
					tracker.start();
				}
				catch (std::exception & e)
				{
					spdlog::error("Failed to start client: {}", e.what());
					session.reset();
				}
				catch (...)
				{
					spdlog::error("Failed to start client: unknown exception");
					session.reset();
				}
				return;
			}

			if (session)
				session.reset();

			if (attempt < 5 && !shutdown)
			{
				int delay_s = 1 << (attempt - 1);
				spdlog::info("Retrying in {} seconds...", delay_s);
				for (int i = 0; i < delay_s * 10 && !shutdown; ++i)
					std::this_thread::sleep_for(100ms);
			}
		}

		spdlog::error("All connection attempts failed");
	});
}

} // anonymous namespace

static std::optional<std::string> get_server_uri_from_intent(JNIEnv * env, jobject intent)
{
	if (!intent)
		return std::nullopt;

	jclass intent_class = env->GetObjectClass(intent);
	jmethodID get_data = env->GetMethodID(intent_class, "getDataString", "()Ljava/lang/String;");
	jstring data_str = (jstring)env->CallObjectMethod(intent, get_data);

	if (data_str)
	{
		const char * data = env->GetStringUTFChars(data_str, nullptr);
		std::string uri(data);
		env->ReleaseStringUTFChars(data_str, data);
		env->DeleteLocalRef(intent_class);

		if (uri.starts_with("wivrn://") || uri.starts_with("wivrn+tcp://"))
			return uri;
	}
	else
	{
		env->DeleteLocalRef(intent_class);
	}

	return std::nullopt;
}

static std::pair<std::string, int> parse_uri(const std::string & uri)
{
	std::string host;
	int port = 5353;

	size_t scheme_end = uri.find("://");
	if (scheme_end == std::string::npos)
		return {host, port};

	size_t host_start = scheme_end + 3;

	size_t at_pos = uri.find('@', host_start);
	if (at_pos != std::string::npos)
		host_start = at_pos + 1;

	size_t path_start = uri.find('/', host_start);
	size_t query_start = uri.find('?', host_start);
	size_t host_end = std::min(
		{path_start == std::string::npos ? uri.size() : path_start,
		 query_start == std::string::npos ? uri.size() : query_start,
		 uri.size()});

	std::string host_port = uri.substr(host_start, host_end - host_start);

	size_t colon = host_port.rfind(':');
	if (colon != std::string::npos)
	{
		host = host_port.substr(0, colon);
		port = std::stoi(host_port.substr(colon + 1));
	}
	else
	{
		host = host_port;
	}

	return {host, port};
}

static std::optional<std::string> parse_pin_from_uri(const std::string & uri)
{
	size_t scheme_end = uri.find("://");
	if (scheme_end != std::string::npos)
	{
		size_t host_start = scheme_end + 3;
		size_t at_pos = uri.find('@', host_start);
		if (at_pos != std::string::npos)
		{
			std::string userinfo = uri.substr(host_start, at_pos - host_start);
			size_t colon = userinfo.find(':');
			if (colon != std::string::npos)
			{
				std::string pin = userinfo.substr(colon + 1);
				if (!pin.empty())
					return pin;
			}
		}
	}

	size_t query_start = uri.find('?');
	if (query_start == std::string::npos)
		return std::nullopt;

	std::string query = uri.substr(query_start + 1);
	size_t pin_pos = query.find("pin=");
	if (pin_pos == std::string::npos)
		return std::nullopt;

	size_t value_start = pin_pos + 4;
	size_t value_end = query.find('&', value_start);
	if (value_end == std::string::npos)
		value_end = query.size();

	return query.substr(value_start, value_end - value_start);
}

extern "C" {

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnInit(JNIEnv * env, jobject thiz, jlong ptr, jobject intent)
{
	static auto logger = spdlog::android_logger_mt("WiVRn-Pico", "WiVRn-Pico");
	spdlog::set_default_logger(logger);
	spdlog::set_level(spdlog::level::debug);

	spdlog::info("WiVRn PvrSDK-Native client starting");

	auto * client = new pico_client();
	g_client = client;

	env->GetJavaVM(&client->vm);
	client->activity = env->NewGlobalRef(thiz);

	auto uri = get_server_uri_from_intent(env, intent);
	if (uri)
	{
		spdlog::info("Server URI: {}", *uri);
		auto [host, port] = parse_uri(*uri);
		client->server_host = host;
		client->server_port = port;
		client->tcp_only = uri->starts_with("wivrn+tcp://");

		auto pin = parse_pin_from_uri(*uri);
		if (pin)
		{
			client->pairing_pin = *pin;
			spdlog::info("PIN from URI: {}", *pin);
		}
	}
	else
	{
		spdlog::warn("No server URI provided, waiting for intent");
		client->server_host = "";
		client->server_port = 5353;
	}
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeGetHeadData(JNIEnv * env, jobject thiz, jlong ptr, jfloatArray out)
{
	if (!g_client || !out)
		return;
	if (env->GetArrayLength(out) < 7)
		return;
	float buf[7];
	g_client->tracker.get_head_pose(buf, buf + 4);
	env->SetFloatArrayRegion(out, 0, 7, buf);
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnDestroy(JNIEnv * env, jobject thiz, jlong ptr)
{
	if (!g_client)
		return;

	g_client->shutdown = true;
	g_client->running = false;

	g_client->tracker.stop();

	if (g_client->network_thread.joinable())
		g_client->network_thread.join();

	if (g_client->activity)
		env->DeleteGlobalRef(g_client->activity);

	delete g_client;
	g_client = nullptr;

	spdlog::info("WiVRn PvrSDK-Native client destroyed");
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnPause(JNIEnv * env, jobject thiz, jlong ptr)
{
	if (g_client)
		g_client->running = false;
	spdlog::info("nativePause");
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnResume(JNIEnv * env, jobject thiz, jlong ptr)
{
	if (!g_client)
		return;

	g_client->try_connect();

	g_client->running = true;
	spdlog::info("nativeResume");
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnNewIntent(JNIEnv * env, jobject thiz, jlong ptr, jobject intent)
{
	if (!g_client)
		return;

	auto uri = get_server_uri_from_intent(env, intent);
	if (uri)
	{
		spdlog::info("New server URI: {}", *uri);
		auto [host, port] = parse_uri(*uri);
		g_client->server_host = host;
		g_client->server_port = port;
		g_client->tcp_only = uri->starts_with("wivrn+tcp://");

		auto pin = parse_pin_from_uri(*uri);
		if (pin)
		{
			g_client->pairing_pin = *pin;
			spdlog::info("PIN from new intent URI: {}", *pin);
		}

		if (g_client->session)
		{
			spdlog::info("Tearing down existing session for reconnection");
			g_client->tracker.stop();
			g_client->tracker.session = nullptr;
			g_client->session.reset();
			if (g_client->network_thread.joinable())
				g_client->network_thread.join();
		}

		g_client->try_connect();
	}
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnOnFrameBegin(
	JNIEnv * env, jobject thiz, jlong ptr,
	jfloatArray headOrient, jfloatArray headPos,
	jfloatArray leftOrient, jfloatArray leftPos, jint leftTrigger, jintArray leftTouch, jint leftBattery,
	jboolean leftA, jboolean leftB, jboolean leftGrip, jboolean leftClick, jboolean leftMenu,
	jfloatArray rightOrient, jfloatArray rightPos, jint rightTrigger, jintArray rightTouch, jint rightBattery,
	jboolean rightA, jboolean rightB, jboolean rightGrip, jboolean rightClick, jboolean rightMenu)
{
	if (!g_client)
		return;

	float head_o[4], head_p[3];
	env->GetFloatArrayRegion(headOrient, 0, 4, head_o);
	env->GetFloatArrayRegion(headPos, 0, 3, head_p);

	static int frame_begin_count = 0;
	bool log_this_frame = (++frame_begin_count % 60 == 1);
	if (log_this_frame)
		spdlog::warn("onFrameBegin #{}: pos=({:.3f},{:.3f},{:.3f})", frame_begin_count, head_p[0], head_p[1], head_p[2]);

	g_client->tracker.set_head_pose(head_o, head_p);

	if (log_this_frame)
	{
		spdlog::warn("TRACKING RAW HMD: orient=({:.4f},{:.4f},{:.4f},{:.4f}) pos=({:.4f},{:.4f},{:.4f})",
			head_o[0], head_o[1], head_o[2], head_o[3],
			head_p[0], head_p[1], head_p[2]);
	}

	if (leftOrient && leftPos)
	{
		float o[4], p[3];
		env->GetFloatArrayRegion(leftOrient, 0, 4, o);
		env->GetFloatArrayRegion(leftPos, 0, 3, p);
		int t[2] = {128, 128};
		if (leftTouch)
			env->GetIntArrayRegion(leftTouch, 0, 2, t);
		g_client->tracker.update_controller(0, o, p, leftTrigger, t, leftBattery,
			leftA, leftB, leftGrip, leftClick, leftMenu);
		if (log_this_frame)
		{
			spdlog::warn("TRACKING RAW LEFT: orient=({:.4f},{:.4f},{:.4f},{:.4f}) pos=({:.4f},{:.4f},{:.4f}) trigger={} batt={}",
				o[0], o[1], o[2], o[3],
				p[0], p[1], p[2], leftTrigger, leftBattery);
		}
	}
	else
	{
		g_client->tracker.clear_controller(0);
	}

	if (rightOrient && rightPos)
	{
		float o[4], p[3];
		env->GetFloatArrayRegion(rightOrient, 0, 4, o);
		env->GetFloatArrayRegion(rightPos, 0, 3, p);
		int t[2] = {128, 128};
		if (rightTouch)
			env->GetIntArrayRegion(rightTouch, 0, 2, t);
		g_client->tracker.update_controller(1, o, p, rightTrigger, t, rightBattery,
			rightA, rightB, rightGrip, rightClick, rightMenu);
		if (log_this_frame)
		{
			spdlog::warn("TRACKING RAW RIGHT: orient=({:.4f},{:.4f},{:.4f},{:.4f}) pos=({:.4f},{:.4f},{:.4f}) trigger={} batt={}",
				o[0], o[1], o[2], o[3],
				p[0], p[1], p[2], rightTrigger, rightBattery);
		}
	}
	else
	{
		g_client->tracker.clear_controller(1);
	}
}

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
	{
		std::lock_guard lock(g_client->decoded_frame_mutex);
		decoded = g_client->latest_decoded_frames[eye];
	}

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
			if (g_client->eye_egl_images[eye] != EGL_NO_IMAGE_KHR)
			{
				g_eglDestroyImageKHR(eglGetDisplay(EGL_DEFAULT_DISPLAY), g_client->eye_egl_images[eye]);
				g_client->eye_egl_images[eye] = EGL_NO_IMAGE_KHR;
			}

			g_client->eye_current_frames[eye].reset();
			g_client->last_hb[eye] = nullptr;

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

	GLuint swap_tex = g_client->swap_tex[eye][g_client->swap_idx];

	glBindFramebuffer(GL_FRAMEBUFFER, g_client->stream_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, swap_tex, 0);

	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE)
	{
		spdlog::warn("FBO incomplete for eye {} slot {} status={}", eye, g_client->swap_idx, status);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		return;
	}

	g_client->blit_pipeline.draw(eye, ext_tex);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);

	glFlush();

	static int draw_count = 0;
	if (++draw_count % 300 == 0)
		spdlog::info("DrawEye {}: ext_tex={} swap_tex={} decoded_valid={}", eye, ext_tex, swap_tex, decoded ? decoded->valid : false);
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnFrameEnd(JNIEnv * env, jobject thiz, jlong ptr)
{
	if (!g_client || !g_client->gl_initialized)
		return;

	int slot = g_client->swap_idx;

	for (int e = 0; e < 2; e++)
	{
		PVR_CameraEndFrame(e, g_client->swap_tex[e][slot]);
	}

	float head_o[4], head_p[3];
	g_client->tracker.get_head_pose(head_o, head_p);

	float n2 = head_o[0]*head_o[0] + head_o[1]*head_o[1] + head_o[2]*head_o[2] + head_o[3]*head_o[3];
	if (n2 > 1e-6f)
	{
		float inv = 1.0f / sqrtf(n2);
		head_o[0] *= inv; head_o[1] *= inv; head_o[2] *= inv; head_o[3] *= inv;
	}

	float ipd = 0.064f;
	for (int e = 0; e < 2; e++)
	{
		float eye_offset = (e == 0 ? -ipd * 0.5f : ipd * 0.5f);
		float v[3] = {eye_offset, 0, 0};
		float rotated[3];
		neo2::rotate_vector({head_o[0], head_o[1], head_o[2], head_o[3]}, v, rotated);

		PvrPoseBlk blk;
		memset(&blk, 0, sizeof(blk));
		blk.v[0] = head_o[0];
		blk.v[1] = head_o[1];
		blk.v[2] = head_o[2];
		blk.v[3] = head_o[3];
		blk.v[4] = head_p[0] + rotated[0];
		blk.v[5] = head_p[1] + rotated[1];
		blk.v[6] = head_p[2] + rotated[2];
		PVR_ChangeRenderPose(e, 0, blk);
	}

	PVR_TimeWarpEvent(0);

	g_client->swap_idx = (g_client->swap_idx + 1) % g_client->kSwapLen;
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnInitGL(JNIEnv * env, jobject thiz, jlong ptr, jint w, jint h)
{
	if (!g_client)
		return;

	spdlog::warn("nativeInitGL: {}x{}", w, h);

	g_client->eye_width = w;
	g_client->eye_height = h;

	spdlog::warn("nativeInitGL: FOV L={:.4f} R={:.4f} U={:.4f} D={:.4f}",
		g_client->eye_fov[0].angleLeft, g_client->eye_fov[0].angleRight,
		g_client->eye_fov[0].angleUp, g_client->eye_fov[0].angleDown);

	g_client->blit_pipeline.init(w, h);
	load_egl_procs();

	for (int e = 0; e < 2; e++)
	{
		glGenTextures(g_client->kSwapLen, g_client->swap_tex[e]);
		for (int i = 0; i < g_client->kSwapLen; i++)
		{
			glBindTexture(GL_TEXTURE_2D, g_client->swap_tex[e][i]);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		}
	}
	glBindTexture(GL_TEXTURE_2D, 0);

	glGenFramebuffers(1, &g_client->stream_fbo);
	spdlog::warn("Created DIATW swapchain {}x{} x{} slots/eye, fbo={}", w, h, g_client->kSwapLen, g_client->stream_fbo);

	g_client->gl_initialized = true;
	g_client->swap_idx = 0;

	jclass localVr = env->FindClass("com/psmart/vrlib/VrActivity");
	jclass vrClass = nullptr;
	if (localVr)
	{
		vrClass = (jclass)env->NewGlobalRef(localVr);
		env->DeleteLocalRef(localVr);
	}
	else
	{
		env->ExceptionClear();
		spdlog::warn("VrActivity class not found; passing null to Pvr_SetInitActivity");
	}

	Pvr_SetInitActivity((void *)g_client->activity, (void *)vrClass);
	Pvr_Enable6DofModule(true);
	int initRc = Pvr_Init(0);
	int sensRc = InitSensor();
	int startRc = Pvr_StartSensor(0);
	spdlog::warn("Pvr_Init={} InitSensor={} Pvr_StartSensor={}", initRc, sensRc, startRc);

	int originRc = Pvr_SetTrackingOriginType(1);
	spdlog::warn("Pvr_SetTrackingOriginType(FloorLevel) = {}", originRc);

	Pvr_DisableBoundary();
	Pvr_ShutdownSDKBoundary();

	g_client->pvr_initialized = true;

	Pvr_SetSinglePassDepthBufferWidthHeight(w, h);

	Pvr_SetAsyncTimeWarp(1);
	g_client->atw_enabled = true;
	spdlog::warn("Async TimeWarp enabled in initGL");

	RenderEventFunc re = (RenderEventFunc)GetRenderEventFunc();
	spdlog::warn("GetRenderEventFunc = {}", (void*)re);
	if (re)
	{
		re(EV_InitRenderThread);
		spdlog::warn("EV_InitRenderThread issued");
	}

	spdlog::warn("GLES initialized for PvrSDK-Native with DIATW warp");
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnDeInitGL(JNIEnv * env, jobject thiz, jlong ptr)
{
	spdlog::info("nativeWivrnDeInitGL");
	if (g_client)
	{
		g_client->gl_initialized = false;

		if (g_client->stream_fbo)
		{
			glDeleteFramebuffers(1, &g_client->stream_fbo);
			g_client->stream_fbo = 0;
		}
		for (int e = 0; e < 2; e++)
		{
			if (g_client->swap_tex[e][0] != 0)
			{
				glDeleteTextures(g_client->kSwapLen, g_client->swap_tex[e]);
				for (int i = 0; i < g_client->kSwapLen; i++)
					g_client->swap_tex[e][i] = 0;
			}
		}
		g_client->atw_enabled = false;
	}
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnSurfaceChanged(JNIEnv * env, jobject thiz, jlong ptr, jint w, jint h)
{
	spdlog::info("nativeSurfaceChanged: {}x{}", w, h);
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnRenderPause(JNIEnv * env, jobject thiz, jlong ptr)
{
	spdlog::info("nativeRenderPause");
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnRenderResume(JNIEnv * env, jobject thiz, jlong ptr)
{
	spdlog::info("nativeRenderResume");

	if (!g_client || !g_client->pvr_initialized.load())
	{
		spdlog::warn("nativeRenderResume: PVR SDK not yet initialized, skipping");
		return;
	}

	int origin_rc = Pvr_SetTrackingOriginType(1);
	spdlog::info("Pvr_SetTrackingOriginType(FloorLevel) = {}", origin_rc);

	Pvr_SetAsyncTimeWarp(1);
	g_client->atw_enabled = true;
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnRendererShutdown(JNIEnv * env, jobject thiz, jlong ptr)
{
	spdlog::info("nativeRendererShutdown");
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnRenderEvent(JNIEnv * env, jobject thiz, jlong ptr, jint event)
{
	spdlog::info("nativeRenderEvent: {}", event);
}

} // extern "C"
