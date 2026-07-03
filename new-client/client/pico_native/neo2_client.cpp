/*
 * Main client implementation for Pico Neo 2 WiVRn.
 * Handles connection, network I/O, tracking/input uplink,
 * video shard reassembly, and EGL frame display.
 */

#include "neo2_client.h"
#include "protocol_version.h"
#include "wivrn_packets.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/android_sink.h>

#include <arpa/inet.h>
#include <cstring>
#include <fstream>
#include <netdb.h>
#include <unistd.h>

using namespace std::chrono_literals;
using namespace wivrn;

neo2_client * g_neo2_client = nullptr;

void neo2_client::load_egl_procs()
{
	if (egl_create_image)
		return;
	egl_create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
	egl_destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
	egl_get_native_buffer = (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress("eglGetNativeClientBufferANDROID");
	gl_egl_image_target = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
	spdlog::info("EGL procs loaded: create={} destroy={} getBuffer={} target={}",
		!!egl_create_image, !!egl_destroy_image, !!egl_get_native_buffer, !!gl_egl_image_target);
}

void neo2_client::setup_decoders()
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

		decoders[i] = std::make_unique<neo2_video_decoder>(
			*video_desc, i,
			[this, i](std::shared_ptr<neo2_decoded_frame> frame) {
				std::lock_guard lock(decoded_frame_mutex);
				latest_decoded_frames[i] = std::move(frame);
				latest_decoded_frame_index.store(latest_decoded_frames[i]->frame_index);
			});
		spdlog::info("Created decoder for stream {}", i);
	}

	for (int i = 0; i < 3; i++)
	{
		current_shards[i].reset(0);
		next_shards[i].reset(1);
	}
}

void neo2_client::setup_audio()
{
	if (!audio_desc || audio_handle)
		return;

	audio_handle = std::make_unique<neo2_audio>(*audio_desc, *session);
	spdlog::info("Audio initialized");
}

void neo2_client::handle_video_shard(to_headset::video_stream_data_shard && shard)
{
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
		current_shards[idx] = std::move(next_shards[idx]);
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

	target->shards[shard.shard_idx] = std::move(shard);

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

	from_headset::feedback feedback{};
	feedback.frame_index = frame_idx;
	feedback.stream_index = idx;

	int64_t now = get_timestamp_ns();
	feedback.received_first_packet = to_xr_time(now);
	feedback.received_last_packet = to_xr_time(now);
	feedback.sent_to_decoder = to_xr_time(now);

	decoders[idx]->submit_data(data, frame_idx, false);

	feedback.received_from_decoder = to_xr_time(get_timestamp_ns());

	if (target->has_view_info)
		decoders[idx]->mark_frame_complete(feedback, target->view_info);

	if (session)
		session->send_stream(feedback);
}

void neo2_client::handle_packet(to_headset::packets & packet)
{
	std::visit([this](auto && p) {
		using T = std::decay_t<decltype(p)>;

		if constexpr (std::is_same_v<T, to_headset::video_stream_description>)
		{
			std::lock_guard lock(video_mutex);
			video_desc = p;
			video_ready = true;
			spdlog::info("Video stream: {}x{}, fps={}", p.width, p.height, p.frame_rate);
			setup_decoders();
		}
		else if constexpr (std::is_same_v<T, to_headset::audio_stream_description>)
		{
			std::lock_guard lock(audio_mutex);
			audio_desc = p;
			spdlog::info("Audio stream description received");
			setup_audio();
		}
		else if constexpr (std::is_same_v<T, to_headset::tracking_control>)
		{
			std::lock_guard lock(tracking_mutex);
			tracking_control = p;
			tracking_control_received = true;
			spdlog::info("Tracking control: {} samples, m2p={}ns",
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
					audio_handle->set_microphone_active(p.state);
			}
		}
		else if constexpr (std::is_same_v<T, to_headset::refresh_rate_change>)
		{
			spdlog::info("Refresh rate change: {} Hz (not supported)", p.hz);
		}
		else if constexpr (std::is_same_v<T, to_headset::server_message>)
		{
			spdlog::info("Server: {}", p.msg);
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
			// Handled during handshake
		}
		else
		{
			// Unhandled packet type
		}
	}, packet);
}

void neo2_client::network_loop()
{
	while (!shutdown && session)
	{
		try
		{
			session->poll_packets([this](to_headset::packets && packet) {
				handle_packet(packet);
			}, 500ms);
		}
		catch (std::exception & e)
		{
			spdlog::error("Network error: {}", e.what());
			shutdown = true;
			break;
		}
		catch (...)
		{
			spdlog::error("Network error: unknown");
			shutdown = true;
			break;
		}
	}
}

static XrQuaternionf pico_to_openxr_quat(const float q[4])
{
	return XrQuaternionf{q[0], q[1], q[2], q[3]};
}

static XrQuaternionf pico_controller_to_openxr_quat(const float q[4])
{
	XrQuaternionf base = pico_to_openxr_quat(q);
	float ox = -0.29237170f, ow = 0.95630476f;
	XrQuaternionf result;
	result.x = base.w * ox + base.x * ow;
	result.y = base.y * ox + base.z * ow;
	result.z = base.z * ow - base.y * ox;
	result.w = base.w * ow - base.x * ox;
	return result;
}

static XrVector3f pico_to_openxr_pos(const float p[3])
{
	return XrVector3f{p[0], p[1], p[2]};
}

void neo2_client::send_tracking()
{
	if (!session)
		return;

	from_headset::tracking tracking_packet{};

	tracking_packet.production_timestamp = get_timestamp_ns();
	tracking_packet.timestamp = to_xr_time(get_timestamp_ns());
	tracking_packet.view_flags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
	tracking_packet.state_flags = 0;

	tracking_packet.interaction_profiles[0] = interaction_profile::bytedance_pico_neo3_controller;
	tracking_packet.interaction_profiles[1] = interaction_profile::bytedance_pico_neo3_controller;

	float head_orient[4], head_pos[3];
	{
		std::lock_guard lock(frame_mutex);
		if (!has_head_pose)
			return;
		memcpy(head_orient, head_orientation, sizeof(head_orient));
		memcpy(head_pos, head_position, sizeof(head_pos));
	}

	XrPosef head_pose;
	head_pose.orientation = pico_to_openxr_quat(head_orient);
	head_pose.position = pico_to_openxr_pos(head_pos);

	for (int eye = 0; eye < 2; eye++)
	{
		tracking_packet.views[eye].pose = head_pose;
		tracking_packet.views[eye].fov = eye_fov[eye];
	}

	from_headset::tracking::pose head_tracking_pose{};
	head_tracking_pose.pose = head_pose;
	head_tracking_pose.device = device_id::HEAD;
	head_tracking_pose.flags = from_headset::tracking::orientation_valid |
	                           from_headset::tracking::position_valid |
	                           from_headset::tracking::orientation_tracked |
	                           from_headset::tracking::position_tracked;
	tracking_packet.device_poses.push_back(head_tracking_pose);

	for (int c = 0; c < 2; c++)
	{
		neo2_controller_state cs;
		{
			std::lock_guard lock(frame_mutex);
			cs = controllers[c];
		}
		if (!cs.connected)
			continue;

		XrPosef controller_pose;
		controller_pose.orientation = pico_controller_to_openxr_quat(cs.orientation);
		controller_pose.position = pico_to_openxr_pos(cs.position);

		bool is_left = (c == 0);

		from_headset::tracking::pose grip_pose{};
		grip_pose.pose = controller_pose;
		grip_pose.device = is_left ? device_id::LEFT_GRIP : device_id::RIGHT_GRIP;
		grip_pose.flags = from_headset::tracking::orientation_valid |
		                 from_headset::tracking::position_valid |
		                 from_headset::tracking::orientation_tracked |
		                 from_headset::tracking::position_tracked;
		tracking_packet.device_poses.push_back(grip_pose);

		from_headset::tracking::pose aim_pose = grip_pose;
		aim_pose.device = is_left ? device_id::LEFT_AIM : device_id::RIGHT_AIM;
		tracking_packet.device_poses.push_back(aim_pose);
	}

	session->send_stream(tracking_packet);
}

void neo2_client::send_inputs()
{
	if (!session)
		return;

	from_headset::inputs inputs_packet{};
	XrTime now = to_xr_time(get_timestamp_ns());

	for (int c = 0; c < 2; c++)
	{
		neo2_controller_state cs;
		{
			std::lock_guard lock(frame_mutex);
			cs = controllers[c];
		}
		if (!cs.connected)
			continue;

		bool is_left = (c == 0);

		auto add_input = [&](device_id id, float value) {
			inputs_packet.values.push_back({
				.id = id,
				.value = value,
				.last_change_time = now,
			});
		};

		float trigger_val = cs.trigger / 255.0f;
		float touch_x = (cs.touch[0] - 128) / 128.0f;
		float touch_y = (128 - cs.touch[1]) / 128.0f;

		if (is_left)
		{
			add_input(device_id::LEFT_TRIGGER_VALUE, trigger_val);
			add_input(device_id::LEFT_TRIGGER_CLICK, trigger_val > 0.9f ? 1.0f : 0.0f);
			add_input(device_id::LEFT_THUMBSTICK_X, touch_x);
			add_input(device_id::LEFT_THUMBSTICK_Y, touch_y);
			add_input(device_id::LEFT_THUMBSTICK_CLICK, cs.thumbstick_click ? 1.0f : 0.0f);
			add_input(device_id::X_CLICK, cs.button_a ? 1.0f : 0.0f);
			add_input(device_id::Y_CLICK, cs.button_b ? 1.0f : 0.0f);
			add_input(device_id::MENU_CLICK, cs.menu ? 1.0f : 0.0f);
			add_input(device_id::LEFT_SQUEEZE_VALUE, cs.grip ? 1.0f : 0.0f);
			add_input(device_id::LEFT_SQUEEZE_CLICK, cs.grip ? 1.0f : 0.0f);
		}
		else
		{
			add_input(device_id::RIGHT_TRIGGER_VALUE, trigger_val);
			add_input(device_id::RIGHT_TRIGGER_CLICK, trigger_val > 0.9f ? 1.0f : 0.0f);
			add_input(device_id::RIGHT_THUMBSTICK_X, touch_x);
			add_input(device_id::RIGHT_THUMBSTICK_Y, touch_y);
			add_input(device_id::RIGHT_THUMBSTICK_CLICK, cs.thumbstick_click ? 1.0f : 0.0f);
			add_input(device_id::A_CLICK, cs.button_a ? 1.0f : 0.0f);
			add_input(device_id::B_CLICK, cs.button_b ? 1.0f : 0.0f);
			add_input(device_id::SYSTEM_CLICK, cs.menu ? 1.0f : 0.0f);
			add_input(device_id::RIGHT_SQUEEZE_VALUE, cs.grip ? 1.0f : 0.0f);
			add_input(device_id::RIGHT_SQUEEZE_CLICK, cs.grip ? 1.0f : 0.0f);
		}
	}

	if (!inputs_packet.values.empty())
		session->send_stream(inputs_packet);
}

void neo2_client::tracking_loop()
{
	while (!shutdown)
	{
		try
		{
			send_tracking();
			send_inputs();
		}
		catch (std::exception & e)
		{
			spdlog::error("Tracking error: {}", e.what());
		}
		catch (...)
		{
			spdlog::error("Tracking error: unknown");
		}

		{
			std::lock_guard lock(haptics_mutex);
			pending_haptics.clear();
		}

		std::this_thread::sleep_for(2ms);
	}
}

bool neo2_client::connect_to_server()
{
	spdlog::info("Connecting to {}:{}", server_host, server_port);

	try
	{
		const char * key_path = "/data/data/org.meumeu.wivrn.neo2.local/files/private_key.pem";
		try
		{
			std::ifstream f{key_path};
			std::string key_str{(std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()};
			headset_keypair = crypto::key::from_private_key(key_str);
			spdlog::info("Loaded existing keypair");
		}
		catch (...)
		{
			spdlog::info("Generating new keypair");
			headset_keypair = crypto::key::generate_x448_keypair();
			std::ofstream{key_path} << headset_keypair.private_key();
			spdlog::info("Generated and saved new keypair");
		}

		std::string model_name = "Pico Neo2";

		auto pin_enter = [this](int fd) -> std::string {
			if (!pairing_pin.empty())
			{
				spdlog::info("Using PIN: {}", pairing_pin);
				return pairing_pin;
			}
			spdlog::info("No PIN configured, using default 000000");
			return "000000";
		};

		struct addrinfo hints{};
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;

		struct addrinfo * result = nullptr;
		int ret = getaddrinfo(server_host.c_str(), std::to_string(server_port).c_str(), &hints, &result);
		if (ret != 0)
		{
			spdlog::error("getaddrinfo: {}", gai_strerror(ret));
			return false;
		}

		for (struct addrinfo * rp = result; rp; rp = rp->ai_next)
		{
			try
			{
				if (rp->ai_family == AF_INET)
				{
					auto * addr = (struct sockaddr_in *)rp->ai_addr;
					in_addr ip = addr->sin_addr;
					spdlog::info("Trying IPv4 {}", inet_ntoa(ip));

					try
					{
						session = std::make_unique<neo2_session>(
							ip, server_port, tcp_only, headset_keypair, model_name, pin_enter);
					}
					catch (std::exception & e)
					{
						spdlog::warn("IPv4 session failed: {}", e.what());
					}
					catch (...)
					{
						spdlog::warn("IPv4 session failed: unknown");
					}
					if (session && session->is_connected())
					{
						freeaddrinfo(result);
						spdlog::info("Connected (IPv4)");
						return true;
					}
					if (session)
						session.reset();
				}
				else if (rp->ai_family == AF_INET6)
				{
					auto * addr = (struct sockaddr_in6 *)rp->ai_addr;
					in6_addr ip = addr->sin6_addr;
					spdlog::info("Trying IPv6");
					try
					{
						session = std::make_unique<neo2_session>(
							ip, server_port, tcp_only, headset_keypair, model_name, pin_enter);
					}
					catch (...)
					{
						spdlog::warn("IPv6 session failed");
					}
					if (session && session->is_connected())
					{
						freeaddrinfo(result);
						spdlog::info("Connected (IPv6)");
						return true;
					}
					if (session)
						session.reset();
				}
			}
			catch (std::exception & e)
			{
				spdlog::warn("Connection attempt: {}", e.what());
			}
			catch (...)
			{
				spdlog::warn("Connection attempt: unknown");
			}
		}

		freeaddrinfo(result);
		spdlog::error("Failed to connect");
		return false;
	}
	catch (std::exception & e)
	{
		spdlog::error("connect error: {}", e.what());
		return false;
	}
	catch (...)
	{
		spdlog::error("connect error: unknown");
		return false;
	}
}

void neo2_client::send_headset_info()
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

	neo2_audio::fill_audio_info(info);

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
		neo2_video_decoder::enumerate_supported_codecs(codecs);
		info.supported_codecs = codecs;
	}
	if (info.supported_codecs.empty())
		info.supported_codecs = {h264, h265};

	info.system_name = "Pico Neo2";
	info.language = "en";
	info.country = "US";
	info.variant = "";

	session->send_control(info);
	spdlog::info("Sent headset info: {}x{}, {} codecs",
		info.render_eye_width, info.render_eye_height,
		info.supported_codecs.size());
}

void neo2_client::try_connect()
{
	if (session || server_host.empty())
		return;

	std::lock_guard lock(connect_mutex);
	if (connect_thread.joinable())
	{
		if (session)
			return;
		connect_thread.join();
	}

	connect_thread = std::thread([this] {
		if (!connect_to_server())
			return;

		try
		{
			send_headset_info();
			network_thread = std::thread([] { g_neo2_client->network_loop(); });
			tracking_thread = std::thread([] { g_neo2_client->tracking_loop(); });
		}
		catch (std::exception & e)
		{
			spdlog::error("Failed to start: {}", e.what());
		}
		catch (...)
		{
			spdlog::error("Failed to start: unknown");
		}
	});
}
