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
#include "pico_lobby.h"
#include "crypto.h"
#include "protocol_version.h"
#include "wivrn_packets.h"
#include <android/bitmap.h>

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
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <cmath>
#include <unistd.h>
#include <future>
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
void  Pvr_SetProjectionFov(float fovX, float fovY);
float Pvr_GetFOV();
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
	pico_lobby lobby;
	int eye_width = 1664;
	int eye_height = 1664;
	bool gl_initialized = false;
	std::atomic<bool> streaming{false};
	std::atomic<bool> stream_ui_visible{false};
	bool prev_thumbstick_click[2] = {false, false};

	// Stats tracking
	uint64_t stats_bytes_rx = 0;
	uint64_t stats_bytes_tx = 0;
	int64_t stats_last_time = 0;
	int stats_frame_count = 0;
	int stats_fps = 0;
	float stats_bandwidth_rx = 0;
	float stats_bandwidth_tx = 0;
	int64_t stats_last_latency = 0;
	int64_t stats_last_encode_begin = 0;

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

	// Snapshot of decoded frames for the current render frame (both eyes must use same server frame)
	std::shared_ptr<pico_decoded_frame> render_frames[2];

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

	std::promise<std::string> pin_promise;

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

	void notify_connection_state(int state, const std::string & msg)
	{
		if (!vm || !activity)
			return;

		JNIEnv * env = nullptr;
		bool attached = false;
		if (vm->GetEnv((void **)&env, JNI_VERSION_1_6) != JNI_OK)
		{
			if (vm->AttachCurrentThread(&env, nullptr) == JNI_OK)
				attached = true;
		}

		if (env && activity)
		{
			jclass clazz = env->GetObjectClass(activity);
			jmethodID method = env->GetMethodID(clazz, "onConnectionStateChanged", "(ILjava/lang/String;)V");
			if (method)
			{
				jstring jmsg = env->NewStringUTF(msg.c_str());
				env->CallVoidMethod(activity, method, state, jmsg);
				env->DeleteLocalRef(jmsg);
			}
			env->DeleteLocalRef(clazz);
		}

		if (attached)
			vm->DetachCurrentThread();
	}

	void notify_application_list(const std::vector<std::pair<std::string, std::string>> & apps)
	{
		if (!vm || !activity)
			return;

		JNIEnv * env = nullptr;
		bool attached = false;
		if (vm->GetEnv((void **)&env, JNI_VERSION_1_6) != JNI_OK)
		{
			if (vm->AttachCurrentThread(&env, nullptr) == JNI_OK)
				attached = true;
		}

		if (env && activity)
		{
			jclass clazz = env->GetObjectClass(activity);
			jclass string_class = env->FindClass("java/lang/String");
			jmethodID method = env->GetMethodID(clazz, "onApplicationList", "([Ljava/lang/String;[Ljava/lang/String;)V");
			if (method && string_class)
			{
				jobjectArray ids = env->NewObjectArray(apps.size(), string_class, nullptr);
				jobjectArray names = env->NewObjectArray(apps.size(), string_class, nullptr);
				for (size_t i = 0; i < apps.size(); i++)
				{
					jstring jid = env->NewStringUTF(apps[i].first.c_str());
					jstring jname = env->NewStringUTF(apps[i].second.c_str());
					env->SetObjectArrayElement(ids, i, jid);
					env->SetObjectArrayElement(names, i, jname);
					env->DeleteLocalRef(jid);
					env->DeleteLocalRef(jname);
				}
				env->CallVoidMethod(activity, method, ids, names);
				env->DeleteLocalRef(ids);
				env->DeleteLocalRef(names);
			}
			env->DeleteLocalRef(clazz);
			if (string_class)
				env->DeleteLocalRef(string_class);
		}

		if (attached)
			vm->DetachCurrentThread();
	}

	void notify_application_icon(const std::string & app_id, const std::vector<std::byte> & png_data)
	{
		if (!vm || !activity)
			return;

		JNIEnv * env = nullptr;
		bool attached = false;
		if (vm->GetEnv((void **)&env, JNI_VERSION_1_6) != JNI_OK)
		{
			if (vm->AttachCurrentThread(&env, nullptr) == JNI_OK)
				attached = true;
		}

		if (env && activity)
		{
			jclass clazz = env->GetObjectClass(activity);
			jmethodID method = env->GetMethodID(clazz, "onApplicationIcon", "(Ljava/lang/String;[B)V");
			if (method)
			{
				jstring jid = env->NewStringUTF(app_id.c_str());
				jbyteArray jdata = env->NewByteArray(png_data.size());
				env->SetByteArrayRegion(jdata, 0, png_data.size(), reinterpret_cast<const jbyte *>(png_data.data()));
				env->CallVoidMethod(activity, method, jid, jdata);
				env->DeleteLocalRef(jid);
				env->DeleteLocalRef(jdata);
			}
			env->DeleteLocalRef(clazz);
		}

		if (attached)
			vm->DetachCurrentThread();
	}

	void notify_running_applications(const std::vector<to_headset::running_applications::application> & apps)
	{
		if (!vm || !activity)
			return;

		JNIEnv * env = nullptr;
		bool attached = false;
		if (vm->GetEnv((void **)&env, JNI_VERSION_1_6) != JNI_OK)
		{
			if (vm->AttachCurrentThread(&env, nullptr) == JNI_OK)
				attached = true;
		}

		if (env && activity)
		{
			jclass clazz = env->GetObjectClass(activity);
			jclass string_class = env->FindClass("java/lang/String");
			jmethodID method = env->GetMethodID(clazz, "onRunningApplications", "([Ljava/lang/String;[I[Z[Z)V");
			if (method && string_class)
			{
				jobjectArray jnames = env->NewObjectArray(apps.size(), string_class, nullptr);
				jintArray jids = env->NewIntArray(apps.size());
				jbooleanArray joverlays = env->NewBooleanArray(apps.size());
				jbooleanArray jactives = env->NewBooleanArray(apps.size());
				jint ids[apps.size()];
				jboolean overlays[apps.size()];
				jboolean actives[apps.size()];
				for (size_t i = 0; i < apps.size(); i++)
				{
					jstring jname = env->NewStringUTF(apps[i].name.c_str());
					env->SetObjectArrayElement(jnames, i, jname);
					env->DeleteLocalRef(jname);
					ids[i] = apps[i].id;
					overlays[i] = apps[i].overlay;
					actives[i] = apps[i].active;
				}
				env->SetIntArrayRegion(jids, 0, apps.size(), ids);
				env->SetBooleanArrayRegion(joverlays, 0, apps.size(), overlays);
				env->SetBooleanArrayRegion(jactives, 0, apps.size(), actives);
				env->CallVoidMethod(activity, method, jnames, jids, joverlays, jactives);
				env->DeleteLocalRef(jnames);
				env->DeleteLocalRef(jids);
				env->DeleteLocalRef(joverlays);
				env->DeleteLocalRef(jactives);
			}
			env->DeleteLocalRef(clazz);
			if (string_class)
				env->DeleteLocalRef(string_class);
		}

		if (attached)
			vm->DetachCurrentThread();
	}

	void handle_packet(to_headset::packets & packet);
	void handle_video_shard(to_headset::video_stream_data_shard && shard);
	void send_feedback(uint64_t frame_index);

	void notify_stream_stats(int fps, int latency_ms, float bandwidth_rx, float bandwidth_tx, int bitrate_mbps)
	{
		if (!vm || !activity)
			return;

		JNIEnv * env = nullptr;
		bool attached = false;
		if (vm->GetEnv((void **)&env, JNI_VERSION_1_6) != JNI_OK)
		{
			if (vm->AttachCurrentThread(&env, nullptr) == JNI_OK)
				attached = true;
		}

		if (env && activity)
		{
			jclass clazz = env->GetObjectClass(activity);
			jmethodID method = env->GetMethodID(clazz, "onStreamStats", "(IIIII)V");
			if (method)
			{
				env->CallVoidMethod(activity, method, fps, latency_ms, (int)bandwidth_rx, (int)bandwidth_tx, bitrate_mbps);
			}
			env->DeleteLocalRef(clazz);
		}

		if (attached)
			vm->DetachCurrentThread();
	}

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
	streaming = true;
	static int shard_count = 0;
	++shard_count;

	if (shard.timing_info)
	{
		stats_frame_count++;
		stats_last_encode_begin = shard.timing_info->encode_begin;
	}

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

			int64_t head_pred = 0;
			for (const auto & s : p.pattern)
			{
				if (s.device == device_id::HEAD)
				{
					head_pred = std::max(head_pred, s.prediction_ns);
				}
			}
			tracker.set_prediction_ns(head_pred);
			spdlog::info("Head prediction: {}ns", head_pred);
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
		else if constexpr (std::is_same_v<T, to_headset::application_list>)
		{
			spdlog::info("Received application list: {} apps", p.applications.size());
			std::vector<std::pair<std::string, std::string>> apps;
			apps.reserve(p.applications.size());
			for (auto & app : p.applications)
				apps.emplace_back(app.id, app.name);
			notify_application_list(apps);
		}
	else if constexpr (std::is_same_v<T, to_headset::application_icon>)
		{
			spdlog::info("Received application icon for id={} ({} bytes)", p.id, p.image.size());
			notify_application_icon(p.id, p.image);
		}
		else if constexpr (std::is_same_v<T, to_headset::running_applications>)
		{
			spdlog::info("Received running applications: {} apps", p.applications.size());
			notify_running_applications(p.applications);
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

	spdlog::info("Network loop ended, cleaning up session");
	tracker.stop();
	tracker.session = nullptr;
	session.reset();
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
				spdlog::warn("PIN entry requested - using PIN from URI: {}", pairing_pin);
				return pairing_pin;
			}

			spdlog::warn("PIN entry requested - asking user via Java dialog");

			pin_promise = std::promise<std::string>();
			auto future = pin_promise.get_future();

			if (vm && activity)
			{
				JNIEnv * env = nullptr;
				bool attached = false;
				if (vm->GetEnv((void **)&env, JNI_VERSION_1_6) != JNI_OK)
				{
					if (vm->AttachCurrentThread(&env, nullptr) == JNI_OK)
						attached = true;
				}

				if (env && activity)
				{
					jclass clazz = env->GetObjectClass(activity);
					jmethodID method = env->GetMethodID(clazz, "requestPinEntry", "()V");
					if (method)
						env->CallVoidMethod(activity, method);
					else
						spdlog::error("Could not find requestPinEntry method");
					env->DeleteLocalRef(clazz);
				}

				if (attached)
					vm->DetachCurrentThread();
			}

			spdlog::warn("Waiting for PIN from user dialog...");
			auto status = future.wait_for(std::chrono::seconds(120));
			if (status == std::future_status::timeout)
			{
				spdlog::warn("PIN entry timed out, using 000000");
				return "000000";
			}

			std::string pin = future.get();
			if (pin.empty())
			{
				spdlog::warn("PIN entry cancelled, using 000000");
				return "000000";
			}
			spdlog::warn("Using PIN from dialog: {}", pin);
			return pin;
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
					catch (std::exception & e)
					{
						spdlog::warn("connect: IPv6 session creation failed: {}", e.what());
					}
					catch (...)
					{
						spdlog::warn("connect: IPv6 session creation failed (unknown exception)");
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
		int attempt = 0;
		while (!shutdown)
		{
			++attempt;
			spdlog::info("Connection attempt {}", attempt);
			notify_connection_state(1, "Connecting (attempt " + std::to_string(attempt) + ")");

			if (connect_to_server())
			{
				notify_connection_state(1, "Connected, starting stream...");
				try
				{
					int fd = session->get_control_fd();
					int keepalive = 1;
					setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
					int idle = 3;
					setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
					int intvl = 1;
					setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
					int cnt = 3;
					setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));

					send_headset_info();
					network_thread = std::thread([] { g_client->network_loop(); });
					tracker.session = session.get();
					tracker.start();

					notify_connection_state(4, "Streaming");
					network_thread.join();
				}
				catch (std::exception & e)
				{
					spdlog::error("Failed to start client: {}", e.what());
					notify_connection_state(3, e.what());
					session.reset();
				}
				catch (...)
				{
					spdlog::error("Failed to start client: unknown exception");
					notify_connection_state(3, "Unknown error");
					session.reset();
				}
				attempt = 0;
				continue;
			}

			if (session)
				session.reset();

			if (!shutdown)
			{
				int delay_s = std::min(1 << std::min(attempt - 1, 2), 4);
				spdlog::info("Retrying in {} seconds...", delay_s);
				for (int i = 0; i < delay_s * 10 && !shutdown; ++i)
					std::this_thread::sleep_for(100ms);
			}
		}

		notify_connection_state(0, "");
		spdlog::info("Connection thread exiting");
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

	{
		std::lock_guard lock(g_client->decoded_frame_mutex);
		g_client->render_frames[0] = g_client->latest_decoded_frames[0];
		g_client->render_frames[1] = g_client->latest_decoded_frames[1];
	}

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
			spdlog::warn("TRACKING RAW LEFT: orient=({:.4f},{:.4f},{:.4f},{:.4f}) pos=({:.4f},{:.4f},{:.4f}) trigger={} batt={} grip={}",
				o[0], o[1], o[2], o[3],
				p[0], p[1], p[2], leftTrigger, leftBattery, leftGrip);
		}
	}
	else
	{
		if (log_this_frame)
			spdlog::warn("LEFT controller: no orientation/position data, clearing");
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
			spdlog::warn("TRACKING RAW RIGHT: orient=({:.4f},{:.4f},{:.4f},{:.4f}) pos=({:.4f},{:.4f},{:.4f}) trigger={} batt={} grip={}",
				o[0], o[1], o[2], o[3],
				p[0], p[1], p[2], rightTrigger, rightBattery, rightGrip);
		}
	}
	else
	{
		if (log_this_frame)
			spdlog::warn("RIGHT controller: no orientation/position data, clearing");
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

	bool show_lobby = (ext_tex == 0 || !g_client->streaming.load() || g_client->stream_ui_visible.load());

	if (eye == 0 && g_client->streaming.load())
	{
		controller_sample cs[2];
		g_client->tracker.get_controllers(cs);

		bool both_click = cs[0].connected && cs[1].connected &&
			cs[0].thumbstick_click && cs[1].thumbstick_click;
		bool prev_both = g_client->prev_thumbstick_click[0] && g_client->prev_thumbstick_click[1];

		static int ts_log_count = 0;
		if (++ts_log_count % 60 == 0)
		{
			spdlog::info("Thumbstick: L connected={} click={} R connected={} click={} both={} prev_both={}",
				cs[0].connected, cs[0].thumbstick_click,
				cs[1].connected, cs[1].thumbstick_click,
				both_click, prev_both);
		}

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

		// Compute and send stats every 500ms
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

			int bitrate_mbps = 20;
			{
				std::lock_guard lock(g_client->tracking_mutex);
				if (g_client->tracking_control_received)
				{
					// could extract more info from tracking_control if needed
				}
			}

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
			// If no controller is hitting the panel, send a hide event
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

	glFlush();

	static int draw_count = 0;
	if (++draw_count % 300 == 0)
		spdlog::info("DrawEye {}: ext_tex={} decoded_valid={}", eye, ext_tex, decoded ? decoded->valid : false);
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnFrameEnd(JNIEnv * env, jobject thiz, jlong ptr)
{
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
	g_client->lobby.init(w, h);
	load_egl_procs();

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

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnRenderPause(JNIEnv * env, jobject thiz, jlong ptr)
{
	spdlog::info("nativeRenderPause");
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnRenderResume(JNIEnv * env, jobject thiz, jlong ptr)
{
	spdlog::info("nativeRenderResume");
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnRendererShutdown(JNIEnv * env, jobject thiz, jlong ptr)
{
	spdlog::info("nativeRendererShutdown");
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnRenderEvent(JNIEnv * env, jobject thiz, jlong ptr, jint event)
{
	spdlog::info("nativeRenderEvent: {}", event);
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnSubmitPin(JNIEnv * env, jobject thiz, jlong ptr, jstring pin)
{
	if (!g_client)
		return;

	const char * pin_str = env->GetStringUTFChars(pin, nullptr);
	if (pin_str)
	{
		std::string pin_cpp(pin_str);
		env->ReleaseStringUTFChars(pin, pin_str);
		spdlog::warn("Received PIN from Java dialog: \"{}\"", pin_cpp);
		try
		{
			g_client->pin_promise.set_value(pin_cpp);
		}
		catch (const std::future_error & e)
		{
			spdlog::error("Failed to set PIN promise: {}", e.what());
		}
	}
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

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnConnect(JNIEnv * env, jobject thiz, jlong ptr, jstring hostname, jint port, jboolean tcpOnly)
{
	if (!g_client)
		return;

	const char * host_str = env->GetStringUTFChars(hostname, nullptr);
	if (host_str)
	{
		g_client->server_host = host_str;
		g_client->server_port = port;
		g_client->tcp_only = tcpOnly;
		g_client->pairing_pin.clear();
		env->ReleaseStringUTFChars(hostname, host_str);

		spdlog::info("Connect requested: {}:{} tcp={}", g_client->server_host, g_client->server_port, g_client->tcp_only);

		if (g_client->connect_thread.joinable())
			g_client->connect_thread.join();

		g_client->running = true;
		g_client->shutdown = false;
		g_client->connect_thread = std::thread([&]() {
			g_client->try_connect();
		});
	}
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnDisconnect(JNIEnv * env, jobject thiz, jlong ptr)
{
	if (!g_client)
		return;

	spdlog::info("Disconnect requested");
	g_client->running = false;
	g_client->shutdown = true;
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnRequestAppList(JNIEnv * env, jobject thiz, jlong ptr)
{
	if (!g_client || !g_client->session)
		return;

	spdlog::info("Requesting application list from server");
	try
	{
		g_client->session->send_control(from_headset::get_application_list{
			.language = "en",
			.country = "US",
			.variant = "",
		});
	}
	catch (std::exception & e)
	{
		spdlog::warn("Failed to request application list: {}", e.what());
	}
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnStartApp(JNIEnv * env, jobject thiz, jlong ptr, jstring appId)
{
	if (!g_client || !g_client->session)
		return;

	const char * app_id_str = env->GetStringUTFChars(appId, nullptr);
	if (app_id_str)
	{
		spdlog::info("Starting application: {}", app_id_str);
		try
		{
			g_client->session->send_control(from_headset::start_app{
				.app_id = app_id_str,
			});
		}
		catch (std::exception & e)
		{
			spdlog::warn("Failed to start application: {}", e.what());
		}
		env->ReleaseStringUTFChars(appId, app_id_str);
	}
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnRequestRunningApps(JNIEnv * env, jobject thiz, jlong ptr)
{
	if (!g_client || !g_client->session)
		return;

	try
	{
		g_client->session->send_control(from_headset::get_running_applications{});
	}
	catch (std::exception & e)
	{
		spdlog::warn("Failed to request running applications: {}", e.what());
	}
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnSetActiveApp(JNIEnv * env, jobject thiz, jlong ptr, jint appId)
{
	if (!g_client || !g_client->session)
		return;

	spdlog::info("Setting active application: {}", appId);
	try
	{
		g_client->session->send_control(from_headset::set_active_application{
			.id = (uint32_t)appId,
		});
	}
	catch (std::exception & e)
	{
		spdlog::warn("Failed to set active application: {}", e.what());
	}
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnStopApp(JNIEnv * env, jobject thiz, jlong ptr, jint appId)
{
	if (!g_client || !g_client->session)
		return;

	spdlog::info("Stopping application: {}", appId);
	try
	{
		g_client->session->send_control(from_headset::stop_application{
			.id = (uint32_t)appId,
		});
	}
	catch (std::exception & e)
	{
		spdlog::warn("Failed to stop application: {}", e.what());
	}
}

} // extern "C"
