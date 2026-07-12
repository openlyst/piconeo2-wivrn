#include "pico_client.h"
#include "pico_stutter.h"
#include "pico_sched.h"

#include <spdlog/spdlog.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fstream>
#include <stdexcept>

using namespace std::chrono_literals;
using namespace wivrn;

stutter_detector g_stutter;

pico_client::~pico_client()
{
	shutdown = true;
	running = false;

	tracker.stop();

	if (connect_thread.joinable())
		connect_thread.join();
	if (network_thread.joinable())
		network_thread.join();
}

void pico_client::notify_connection_state(int state, const std::string & msg)
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

void pico_client::notify_application_list(const std::vector<std::pair<std::string, std::string>> & apps)
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

void pico_client::notify_application_icon(const std::string & app_id, const std::vector<std::byte> & png_data)
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

void pico_client::notify_running_applications(const std::vector<to_headset::running_applications::application> & apps)
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
			std::vector<jint> ids(apps.size());
			std::vector<jboolean> overlays(apps.size());
			std::vector<jboolean> actives(apps.size());
			for (size_t i = 0; i < apps.size(); i++)
			{
				jstring jname = env->NewStringUTF(apps[i].name.c_str());
				env->SetObjectArrayElement(jnames, i, jname);
				env->DeleteLocalRef(jname);
				ids[i] = apps[i].id;
				overlays[i] = apps[i].overlay;
				actives[i] = apps[i].active;
			}
			env->SetIntArrayRegion(jids, 0, apps.size(), ids.data());
			env->SetBooleanArrayRegion(joverlays, 0, apps.size(), overlays.data());
			env->SetBooleanArrayRegion(jactives, 0, apps.size(), actives.data());
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

void pico_client::notify_stream_stats(int fps, int latency_ms, float bandwidth_rx, float bandwidth_tx, int bitrate_mbps)
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
				g_stutter.on_frame_decoded(frame->frame_index, i);
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
	if (!streaming.load())
	{
		streaming = true;
		stream_ui_visible = false;
		spdlog::info("Streaming started, hiding lobby UI");
	}
	static int shard_count = 0;
	++shard_count;

	if (shard.timing_info && shard.stream_item_idx == 0)
	{
		stats_frame_count++;
		stats_last_encode_begin = shard.timing_info->encode_begin;
	}

	bool has_timing = shard.timing_info.has_value();
	if (shard_count <= 10 || shard_count % 1000 == 0)
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
	bool is_first_shard = (shard.shard_idx == 0);
	target->shards[shard.shard_idx] = std::move(shard);

	g_stutter.on_shard_arrived(frame_idx, idx, is_first_shard, is_last_shard);

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
	if (recon_count <= 10 || recon_count % 1000 == 0)
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
	g_stutter.on_pushed_to_decoder(frame_idx, idx);

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
			if (head_pred == 0 && p.motions_to_photons > 0)
				head_pred = p.motions_to_photons;
			if (head_pred == 0)
				head_pred = 30000000; // 30ms default prediction
			tracker.set_prediction_ns(head_pred);
			spdlog::info("Head prediction: {}ns", head_pred);
		}
		else if constexpr (std::is_same_v<T, to_headset::haptics>)
		{
			int hand = -1;
			switch (p.id)
			{
				case device_id::LEFT_CONTROLLER_HAPTIC:
				case device_id::LEFT_TRIGGER_HAPTIC:
				case device_id::LEFT_THUMB_HAPTIC:
					hand = 0;
					break;
				case device_id::RIGHT_CONTROLLER_HAPTIC:
				case device_id::RIGHT_TRIGGER_HAPTIC:
				case device_id::RIGHT_THUMB_HAPTIC:
					hand = 1;
					break;
				default:
					break;
			}
			if (hand < 0 || p.amplitude <= 0.f)
				return;

			int ms = static_cast<int>(p.duration.count() / 1000000);
			if (ms < 10)   ms = 10;
			if (ms > 1000) ms = 1000;

			std::lock_guard lock(haptics_mutex);
			auto & slot = rumble[hand];
			if (!slot.active || p.amplitude > slot.amplitude)
				slot.amplitude = p.amplitude;
			if (!slot.active || ms > slot.duration_ms)
				slot.duration_ms = ms;
			slot.active = true;
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
			spdlog::info("Refresh rate change requested: {} Hz (not supported)", p.hz);
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
	pico_sched::pin_current_thread("video recv", 1, -4);

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
			for (int i = 0; i < 1200; ++i)
			{
				if (shutdown)
				{
					spdlog::warn("PIN entry interrupted by shutdown");
					return "000000";
				}
				if (!pairing_pin.empty())
				{
					spdlog::warn("PIN set from URI while waiting: {}", pairing_pin);
					return pairing_pin;
				}
				auto status = future.wait_for(std::chrono::milliseconds(100));
				if (status == std::future_status::ready)
				{
					std::string pin = future.get();
					if (pin.empty())
					{
						spdlog::warn("PIN entry cancelled, using 000000");
						return "000000";
					}
					spdlog::warn("Using PIN from dialog: {}", pin);
					return pin;
				}
			}
			spdlog::warn("PIN entry timed out, using 000000");
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

					try
					{
						session = std::make_unique<wivrn_session_pico>(
							ip, server_port, tcp_only, headset_keypair, model_name, pin_enter, shutdown);
					}
					catch (std::exception & e)
					{
						spdlog::warn("connect: session creation failed: {}", e.what());
					}
					catch (...)
					{
						spdlog::warn("connect: session creation failed (unknown exception)");
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
							ip, server_port, tcp_only, headset_keypair, model_name, pin_enter, shutdown);
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
	info.settings.bitrate_bps = 50000000;

	pico_audio::get_audio_description(info);

	info.fov[0] = eye_fov[0];
	info.fov[1] = eye_fov[1];

	info.hand_tracking = false;
	info.eye_gaze = false;
	info.palm_pose = false;
	info.user_presence = false;
	info.passthrough = false;
	info.face_tracking = from_headset::none;
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
		shutdown = true;
		connect_thread.join();
		shutdown = false;
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
