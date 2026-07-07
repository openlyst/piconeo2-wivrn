#include "pico_tracking.h"
#include "wivrn_client_pico.h"
#include "pico_sdk.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstring>

using namespace wivrn;

namespace
{

constexpr long k_period_ns = 1000000000L / 300;
constexpr int k_uplink_div = 4;

constexpr float k_yaw_offset_deg = 35.0f;
constexpr float k_rot_offset_x_deg = 10.0f;
constexpr float k_rot_offset_y_deg = -34.0f;
constexpr float k_grip_side_mm = 0.0f;
constexpr float k_grip_up_mm = 12.5f;
constexpr float k_grip_back_mm = 40.0f;
constexpr float k_rot_swing = 1.0f;

constexpr float k_head_vel_tau = 0.66f;
constexpr float k_ctrl_vel_tau = 0.05f;

neo2::quat apply_controller_orientation(const float raw_orient[4], int hand)
{
	neo2::quat oq = {-raw_orient[0], -raw_orient[1], raw_orient[2], raw_orient[3]};

	float yaw_deg = (hand == 0 ? +k_yaw_offset_deg : -k_yaw_offset_deg);
	float yaw_rad = yaw_deg * 0.01745329f;
	neo2::quat q_yaw = {0.0f, std::sin(yaw_rad * 0.5f), 0.0f, std::cos(yaw_rad * 0.5f)};
	oq = neo2::normalize_quat(neo2::multiply_quat(oq, q_yaw));

	float rx = k_rot_offset_x_deg * 0.01745329f;
	float ry = (hand == 0 ? k_rot_offset_y_deg : -k_rot_offset_y_deg) * 0.01745329f;
	float rz = 0.0f;

	neo2::quat qrx = {std::sin(rx * 0.5f), 0.0f, 0.0f, std::cos(rx * 0.5f)};
	neo2::quat qry = {0.0f, std::sin(ry * 0.5f), 0.0f, std::cos(ry * 0.5f)};
	neo2::quat qrz = {0.0f, 0.0f, std::sin(rz * 0.5f), std::cos(rz * 0.5f)};
	neo2::quat q_off = neo2::multiply_quat(neo2::multiply_quat(qrx, qry), qrz);
	oq = neo2::normalize_quat(neo2::multiply_quat(oq, q_off));

	return oq;
}

void compute_grip_offset(const neo2::quat & orient, int hand, float out_world[3])
{
	float side = k_grip_side_mm * 0.001f;
	float up = k_grip_up_mm * 0.001f;
	float back = k_grip_back_mm * 0.001f;
	float side_sign = (hand == 0) ? +1.0f : -1.0f;

	float local[3] = {side * side_sign, up, back};

	float swing = k_rot_swing;
	if (swing < 0.0f) swing = 0.0f; else if (swing > 1.0f) swing = 1.0f;

	float yn = std::sqrt(orient.y * orient.y + orient.w * orient.w);
	neo2::quat orient_yaw = (yn > 1e-6f)
		? neo2::quat{0.0f, orient.y / yn, 0.0f, orient.w / yn}
		: neo2::quat{0.0f, 0.0f, 0.0f, 1.0f};

	float orbit[3] = {local[0] * swing, local[1] * swing, local[2] * swing};
	float level[3] = {local[0] * (1.0f - swing), local[1] * (1.0f - swing), local[2] * (1.0f - swing)};

	float w_orbit[3], w_level[3];
	neo2::rotate_vector(orient, orbit, w_orbit);
	neo2::rotate_vector(orient_yaw, level, w_level);

	out_world[0] = w_orbit[0] + w_level[0];
	out_world[1] = w_orbit[1] + w_level[1];
	out_world[2] = w_orbit[2] + w_level[2];
}

} // namespace

void pico_native_tracker::start()
{
	if (running.load())
		return;
	running = true;
	height_calibrated = false;
	thread = std::thread([this] { run(); });
	spdlog::info("Native tracker started");
}

void pico_native_tracker::stop()
{
	running = false;
	if (thread.joinable())
		thread.join();
	spdlog::info("Native tracker stopped");
}

void pico_native_tracker::set_head_pose(const float orient[4], const float pos[3])
{
	uint64_t ts = (uint64_t)now_ns();
	neo2::quat hq = neo2::normalize_quat({orient[0], orient[1], orient[2], orient[3]});

	std::lock_guard lock(state_mutex);
	std::memcpy(head_orient, orient, sizeof(head_orient));
	std::memcpy(head_pos, pos, sizeof(head_pos));
	head_valid = true;

	if (!height_calibrated && !floor_relative.load())
	{
		constexpr float k_standing_eye_height = 1.5f;
		float offset = k_standing_eye_height - pos[1];
		height_offset.store(offset);
		height_calibrated = true;
		spdlog::info("Height calibrated: initial_Y={:.3f} offset={:.3f}", pos[1], offset);
	}

	step_head_filter(pos, hq, ts);
}

void pico_native_tracker::set_prediction_ns(int64_t ns)
{
	prediction_ns.store(ns);
}

void pico_native_tracker::get_head_pose(float out_orient[4], float out_pos[3])
{
	std::lock_guard lock(state_mutex);
	std::memcpy(out_orient, head_orient, sizeof(head_orient));
	std::memcpy(out_pos, head_pos, sizeof(head_pos));
}

void pico_native_tracker::get_controllers(controller_sample out[2])
{
	std::lock_guard lock(state_mutex);
	out[0] = controllers[0];
	out[1] = controllers[1];
}

void pico_native_tracker::update_controller(int hand, const float orient[4], const float pos[3],
                                            int trigger, const int touch[2], int battery,
                                            bool a, bool b, bool grip, bool click, bool menu)
{
	if (hand < 0 || hand > 1)
		return;
	std::lock_guard lock(state_mutex);
	auto & c = controllers[hand];
	std::memcpy(c.orientation, orient, sizeof(c.orientation));
	std::memcpy(c.position, pos, sizeof(c.position));
	c.trigger = trigger;
	c.battery = battery;
	c.button_a = a;
	c.button_b = b;
	c.grip = grip;
	c.thumbstick_click = click;
	c.menu = menu;
	c.connected = true;
	if (touch)
	{
		c.touch[0] = touch[0];
		c.touch[1] = touch[1];
	}
}

void pico_native_tracker::clear_controller(int hand)
{
	if (hand < 0 || hand > 1)
		return;
	std::lock_guard lock(state_mutex);
	controllers[hand].connected = false;
}

void pico_native_tracker::update_controller_from_jni(int hand, int conn, const float * sensor,
                                                     const float * ang_vel, const int * keys)
{
	if (hand < 0 || hand > 1)
		return;

	std::lock_guard lock(state_mutex);
	auto & c = controllers[hand];

	if (conn != 1)
	{
		c.connected = false;
		return;
	}

	c.connected = true;

	if (sensor)
	{
		c.orientation[0] = sensor[0];
		c.orientation[1] = sensor[1];
		c.orientation[2] = sensor[2];
		c.orientation[3] = sensor[3];
		c.position[0] = sensor[4];
		c.position[1] = sensor[5];
		c.position[2] = sensor[6];
	}

	if (keys)
	{
		c.trigger = keys[2];
		c.touch[0] = keys[0];
		c.touch[1] = keys[1];
		c.battery = keys[10];
		c.button_a = keys[6] != 0;
		c.button_b = keys[7] != 0;
		c.grip = keys[3] != 0;
		c.thumbstick_click = keys[4] != 0;
		c.menu = keys[5] != 0;
		c.home = keys[11] != 0;
	}
}

void pico_native_tracker::step_head_filter(const float pos[3], const neo2::quat & orient, uint64_t ts)
{
	if (head_filter_init)
	{
		double dt = (double)(ts - head_prev_ts) * 1e-9;
		if (dt > 0.002 && dt < 0.05)
		{
			float alpha = 1.0f - std::exp(-(float)dt / k_head_vel_tau);

			float lvx = (float)((pos[0] - head_prev_pos[0]) / dt);
			float lvy = (float)((pos[1] - head_prev_pos[1]) / dt);
			float lvz = (float)((pos[2] - head_prev_pos[2]) / dt);

			neo2::quat delta = neo2::normalize_quat(
				neo2::multiply_quat(orient, neo2::conjugate_quat(head_prev_orient)));
			if (delta.w < 0)
			{
				delta.x = -delta.x; delta.y = -delta.y;
				delta.z = -delta.z; delta.w = -delta.w;
			}
			float w = delta.w > 1.0f ? 1.0f : delta.w;
			float angle = 2.0f * std::acos(w);
			float s = std::sqrt(1.0f - w * w);
			float avx = 0, avy = 0, avz = 0;
			if (s > 1e-6f)
			{
				float k = (angle / s) / (float)dt;
				avx = delta.x * k; avy = delta.y * k; avz = delta.z * k;
			}

			head_lin_vel[0] += (lvx - head_lin_vel[0]) * alpha;
			head_lin_vel[1] += (lvy - head_lin_vel[1]) * alpha;
			head_lin_vel[2] += (lvz - head_lin_vel[2]) * alpha;
			head_ang_vel[0] += (avx - head_ang_vel[0]) * alpha;
			head_ang_vel[1] += (avy - head_ang_vel[1]) * alpha;
			head_ang_vel[2] += (avz - head_ang_vel[2]) * alpha;
		}
	}
	head_prev_pos[0] = pos[0];
	head_prev_pos[1] = pos[1];
	head_prev_pos[2] = pos[2];
	head_prev_orient = orient;
	head_prev_ts = ts;
	head_filter_init = true;
}

void pico_native_tracker::step_ctrl_filter(int hand, const float pos_m[3], uint64_t ts)
{
	if (ctrl_filter_init[hand])
	{
		double dt = (double)(ts - ctrl_prev_ts[hand]) * 1e-9;
		if (dt > 0.002 && dt < 0.05)
		{
			float alpha = 1.0f - std::exp(-(float)dt / k_ctrl_vel_tau);
			float lvx = (float)((pos_m[0] - ctrl_prev_pos[hand][0]) / dt);
			float lvy = (float)((pos_m[1] - ctrl_prev_pos[hand][1]) / dt);
			float lvz = (float)((pos_m[2] - ctrl_prev_pos[hand][2]) / dt);
			ctrl_lin_vel[hand][0] += (lvx - ctrl_lin_vel[hand][0]) * alpha;
			ctrl_lin_vel[hand][1] += (lvy - ctrl_lin_vel[hand][1]) * alpha;
			ctrl_lin_vel[hand][2] += (lvz - ctrl_lin_vel[hand][2]) * alpha;
		}
	}
	ctrl_prev_pos[hand][0] = pos_m[0];
	ctrl_prev_pos[hand][1] = pos_m[1];
	ctrl_prev_pos[hand][2] = pos_m[2];
	ctrl_prev_ts[hand] = ts;
	ctrl_filter_init[hand] = true;
}

void pico_native_tracker::run()
{
	int frame_counter = 0;

	auto ts_add_ns = [](struct timespec & t, long ns) {
		t.tv_nsec += ns;
		while (t.tv_nsec >= 1000000000L)
		{
			t.tv_nsec -= 1000000000L;
			t.tv_sec += 1;
		}
	};

	struct timespec next_tick;
	clock_gettime(CLOCK_MONOTONIC, &next_tick);

	while (running.load())
	{
		uint64_t ts = (uint64_t)now_ns();

		float h_orient[4], h_pos[3];
		bool h_valid;

		float qx, qy, qz, qw, px, py, pz, vfov, hfov;
		int viewNum;
		int rc = Pvr_GetMainSensorState(&qx, &qy, &qz, &qw, &px, &py, &pz, &vfov, &hfov, &viewNum);
		h_valid = (rc >= 0);
		if (!h_valid)
		{
			std::lock_guard lock(state_mutex);
			h_valid = head_valid;
			if (h_valid)
			{
				std::memcpy(h_orient, head_orient, sizeof(h_orient));
				std::memcpy(h_pos, head_pos, sizeof(h_pos));
			}
		}
		static int sensor_log_count = 0;
		if (sensor_log_count < 5 || (sensor_log_count % 300) == 0) {
			spdlog::info("sensor rc={} valid={} q=({:.3f},{:.3f},{:.3f},{:.3f}) p=({:.3f},{:.3f},{:.3f})",
				rc, h_valid, h_orient[0], h_orient[1], h_orient[2], h_orient[3], h_pos[0], h_pos[1], h_pos[2]);
			sensor_log_count++;
		}
		if (h_valid && rc >= 0)
		{
			h_orient[0] = qx; h_orient[1] = qy; h_orient[2] = qz; h_orient[3] = qw;
			h_pos[0] = px; h_pos[1] = py; h_pos[2] = pz;

			std::lock_guard lock(state_mutex);
			std::memcpy(head_orient, h_orient, sizeof(head_orient));
			std::memcpy(head_pos, h_pos, sizeof(h_pos));
			head_valid = true;

			neo2::quat hq = neo2::normalize_quat({qx, qy, qz, qw});
			step_head_filter(h_pos, hq, ts);
		}
		else if (h_valid)
		{
			neo2::quat hq = neo2::normalize_quat({h_orient[0], h_orient[1], h_orient[2], h_orient[3]});
			step_head_filter(h_pos, hq, ts);
		}

		if (!h_valid)
		{
			frame_counter++;
			ts_add_ns(next_tick, k_period_ns);
			struct timespec now_t;
			clock_gettime(CLOCK_MONOTONIC, &now_t);
			if (now_t.tv_sec > next_tick.tv_sec ||
			    (now_t.tv_sec == next_tick.tv_sec && now_t.tv_nsec > next_tick.tv_nsec))
			{
				next_tick = now_t;
			}
			else
			{
				clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_tick, nullptr);
			}
			continue;
		}

		controller_sample cs[2];
		{
			std::lock_guard lock(state_mutex);
			cs[0] = controllers[0];
			cs[1] = controllers[1];
		}

		static bool prev_home[2] = {false, false};
		static uint64_t home_press_ts[2] = {0, 0};
		for (int h = 0; h < 2; h++)
		{
			bool home_now = cs[h].connected && cs[h].home;
			if (home_now && !prev_home[h])
			{
				home_press_ts[h] = ts;
			}
			else if (!home_now && prev_home[h])
			{
				if (home_press_ts[h] > 0 && ts - home_press_ts[h] > 800000000ULL)
				{
					spdlog::info("recenter triggered by controller {} home button long press", h);
					Pvr_ResetSensor(PXR_RESET_ALL);
					recenter_requested.store(true);
				}
				home_press_ts[h] = 0;
			}
			prev_home[h] = home_now;
		}

		for (int h = 0; h < 2; h++)
		{
			if (!cs[h].connected)
				continue;

			neo2::quat cq = apply_controller_orientation(cs[h].orientation, h);
			float grip_world[3];
			compute_grip_offset(cq, h, grip_world);

			float pos_m[3] = {
				cs[h].position[0] * 0.001f + grip_world[0],
				cs[h].position[1] * 0.001f + grip_world[1],
				cs[h].position[2] * 0.001f + grip_world[2],
			};
			step_ctrl_filter(h, pos_m, ts);
		}

		bool do_uplink = (frame_counter % k_uplink_div) == 0;
		if (do_uplink && session)
		{
			transmit_tracking((int64_t)ts);
			transmit_inputs((int64_t)ts);
		}

		frame_counter++;

		ts_add_ns(next_tick, k_period_ns);
		struct timespec now_t;
		clock_gettime(CLOCK_MONOTONIC, &now_t);
		if (now_t.tv_sec > next_tick.tv_sec ||
		    (now_t.tv_sec == next_tick.tv_sec && now_t.tv_nsec > next_tick.tv_nsec))
		{
			next_tick = now_t;
		}
		else
		{
			clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_tick, nullptr);
		}
	}
}

void pico_native_tracker::transmit_tracking(int64_t headset_ns)
{
	if (!session)
		return;

	float h_orient[4], h_pos[3];
	bool h_valid;
	controller_sample cs[2];
	{
		std::lock_guard lock(state_mutex);
		h_valid = head_valid;
		if (!h_valid)
			return;
		std::memcpy(h_orient, head_orient, sizeof(h_orient));
		std::memcpy(h_pos, head_pos, sizeof(h_pos));
		cs[0] = controllers[0];
		cs[1] = controllers[1];
	}

	from_headset::tracking pkt{};

	pkt.production_timestamp = headset_ns;
	pkt.timestamp = to_xr_time(headset_ns + prediction_ns.load());
	pkt.view_flags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
	pkt.state_flags = 0;
	if (recenter_requested.exchange(false))
		pkt.state_flags = from_headset::tracking::recentered;

	pkt.interaction_profiles[0] = interaction_profile::bytedance_pico_neo3_controller;
	pkt.interaction_profiles[1] = interaction_profile::bytedance_pico_neo3_controller;

	neo2::quat hq = neo2::normalize_quat({h_orient[0], h_orient[1], h_orient[2], h_orient[3]});

	XrPosef head_pose;
	head_pose.orientation = neo2::to_xr_quat(hq);
	float h_offset = floor_relative.load() ? 0.0f : height_offset.load();
	head_pose.position = {h_pos[0], h_pos[1] + h_offset, h_pos[2]};

	// Pico Neo 2 has a square ~101 degree per-eye FOV (same H and V).
	// The OpenXR runtime may report incorrect/wider FOV values that make
	// everything look too small. Use the known-correct value from the Pico SDK.
	constexpr float k_fov_half = 101.0f * 0.5f * 0.01745329252f;
	constexpr float k_ipd = 0.064f;
	for (int eye = 0; eye < 2; eye++)
	{
		float eye_offset = (eye == 0 ? -k_ipd * 0.5f : k_ipd * 0.5f);
		pkt.views[eye].pose.orientation = {0, 0, 0, 1};
		pkt.views[eye].pose.position = {eye_offset, 0, 0};
		pkt.views[eye].fov = {-k_fov_half, k_fov_half, k_fov_half, -k_fov_half};
	}

	from_headset::tracking::pose head_tp{};
	head_tp.pose = head_pose;
	head_tp.device = device_id::HEAD;
	head_tp.flags = from_headset::tracking::orientation_valid |
	                from_headset::tracking::position_valid |
	                from_headset::tracking::orientation_tracked |
	                from_headset::tracking::position_tracked |
	                from_headset::tracking::linear_velocity_valid |
	                from_headset::tracking::angular_velocity_valid;
	head_tp.linear_velocity = {
		head_lin_vel[0],
		head_lin_vel[1],
		head_lin_vel[2]};
	head_tp.angular_velocity = {
		head_ang_vel[0],
		head_ang_vel[1],
		head_ang_vel[2]};
	pkt.device_poses.push_back(head_tp);

	for (int h = 0; h < 2; h++)
	{
		if (!cs[h].connected)
			continue;

		neo2::quat cq = apply_controller_orientation(cs[h].orientation, h);

		float grip_world[3];
		compute_grip_offset(cq, h, grip_world);

		float pos_m[3] = {
			cs[h].position[0] * 0.001f + grip_world[0],
			cs[h].position[1] * 0.001f + grip_world[1] + h_offset,
			cs[h].position[2] * 0.001f + grip_world[2],
		};

		XrPosef ctrl_pose;
		ctrl_pose.orientation = neo2::to_xr_quat(cq);
		ctrl_pose.position = {pos_m[0], pos_m[1], pos_m[2]};

		bool is_left = (h == 0);

		uint8_t pose_flags = from_headset::tracking::orientation_valid |
		                     from_headset::tracking::position_valid |
		                     from_headset::tracking::orientation_tracked |
		                     from_headset::tracking::position_tracked;

		from_headset::tracking::pose grip_p{};
		grip_p.pose = ctrl_pose;
		grip_p.device = is_left ? device_id::LEFT_GRIP : device_id::RIGHT_GRIP;
		grip_p.flags = pose_flags;
		pkt.device_poses.push_back(grip_p);

		from_headset::tracking::pose aim_p{};
		aim_p.pose.orientation = ctrl_pose.orientation;
		aim_p.pose.position = {
			cs[h].position[0] * 0.001f,
			cs[h].position[1] * 0.001f + h_offset,
			cs[h].position[2] * 0.001f,
		};
		aim_p.device = is_left ? device_id::LEFT_AIM : device_id::RIGHT_AIM;
		aim_p.flags = pose_flags;
		pkt.device_poses.push_back(aim_p);
	}

	static int track_count = 0;
	if (++track_count % 120 == 1)
	{
		spdlog::warn("TRACKING: HMD q=({:.3f},{:.3f},{:.3f},{:.3f}) p=({:.3f},{:.3f},{:.3f}) "
		             "lv=({:.2f},{:.2f},{:.2f}) av=({:.2f},{:.2f},{:.2f})",
			head_pose.orientation.x, head_pose.orientation.y,
			head_pose.orientation.z, head_pose.orientation.w,
			head_pose.position.x, head_pose.position.y, head_pose.position.z,
			head_tp.linear_velocity.x, head_tp.linear_velocity.y, head_tp.linear_velocity.z,
			head_tp.angular_velocity.x, head_tp.angular_velocity.y, head_tp.angular_velocity.z);
		for (int h = 0; h < 2; h++)
		{
			if (!cs[h].connected)
			{
				spdlog::warn("TRACKING {} controller: DISCONNECTED", h == 0 ? "LEFT" : "RIGHT");
				continue;
			}

			neo2::quat cq = apply_controller_orientation(cs[h].orientation, h);
			float grip_world[3];
			compute_grip_offset(cq, h, grip_world);

			spdlog::warn("TRACKING {} controller:\n"
			             "  raw:    q=({:.4f},{:.4f},{:.4f},{:.4f}) pos_mm=({:.1f},{:.1f},{:.1f})\n"
			             "  orient: q=({:.4f},{:.4f},{:.4f},{:.4f})\n"
			             "  grip_off: ({:.4f},{:.4f},{:.4f})\n"
			             "  final:  q=({:.4f},{:.4f},{:.4f},{:.4f}) pos_m=({:.4f},{:.4f},{:.4f})",
				h == 0 ? "LEFT" : "RIGHT",
				cs[h].orientation[0], cs[h].orientation[1], cs[h].orientation[2], cs[h].orientation[3],
				cs[h].position[0], cs[h].position[1], cs[h].position[2],
				cq.x, cq.y, cq.z, cq.w,
				grip_world[0], grip_world[1], grip_world[2],
				cq.x, cq.y, cq.z, cq.w,
				cs[h].position[0] * 0.001f + grip_world[0],
				cs[h].position[1] * 0.001f + grip_world[1] + h_offset,
				cs[h].position[2] * 0.001f + grip_world[2]);
		}
	}

	session->send_stream(pkt);
}

void pico_native_tracker::transmit_inputs(int64_t headset_ns)
{
	if (!session)
		return;

	controller_sample cs[2];
	{
		std::lock_guard lock(state_mutex);
		cs[0] = controllers[0];
		cs[1] = controllers[1];
	}

	from_headset::inputs pkt{};
	XrTime now = to_xr_time(headset_ns);

	// Index into prev_inputs[c]: 0=trigger_value, 1=trigger_click, 2=stick_x, 3=stick_y,
	// 4=stick_click, 5=button_a/x, 6=button_b/y, 7=menu/system, 8=squeeze_value, 9=squeeze_click
	auto add = [&](int c, int idx, device_id id, float value) {
		auto & prev = prev_inputs[c][idx];
		bool changed = (prev.last_change_time == 0 || prev.value != value);
		if (changed)
			prev.last_change_time = now;
		prev.value = value;
		pkt.values.push_back({.id = id, .value = value, .last_change_time = prev.last_change_time});
		if (changed && idx >= 5 && idx <= 7)
			spdlog::warn("INPUT_CHG c={} idx={} id={} val={:.1f} t={}", c, idx, (int)id, value, prev.last_change_time);
	};

	for (int c = 0; c < 2; c++)
	{
		if (!cs[c].connected)
			continue;

		bool is_left = (c == 0);

		float trigger_val = cs[c].trigger / 255.0f;
		float touch_x = (cs[c].touch[0] - 128) / 128.0f;
		float touch_y = (128 - cs[c].touch[1]) / 128.0f;
		float trigger_click = trigger_val > 0.5f ? 1.0f : 0.0f;
		float squeeze = cs[c].grip ? 1.0f : 0.0f;

		if (is_left)
		{
			add(c, 0, device_id::LEFT_TRIGGER_VALUE, trigger_val);
			add(c, 1, device_id::LEFT_TRIGGER_CLICK, trigger_click);
			add(c, 2, device_id::LEFT_THUMBSTICK_X, touch_x);
			add(c, 3, device_id::LEFT_THUMBSTICK_Y, touch_y);
			add(c, 4, device_id::LEFT_THUMBSTICK_CLICK, cs[c].thumbstick_click ? 1.0f : 0.0f);
			add(c, 5, device_id::X_CLICK, cs[c].button_a ? 1.0f : 0.0f);
			add(c, 6, device_id::Y_CLICK, cs[c].button_b ? 1.0f : 0.0f);
			add(c, 7, device_id::MENU_CLICK, cs[c].menu ? 1.0f : 0.0f);
			add(c, 8, device_id::LEFT_SQUEEZE_VALUE, squeeze);
			add(c, 9, device_id::LEFT_SQUEEZE_CLICK, squeeze);
		}
		else
		{
			add(c, 0, device_id::RIGHT_TRIGGER_VALUE, trigger_val);
			add(c, 1, device_id::RIGHT_TRIGGER_CLICK, trigger_click);
			add(c, 2, device_id::RIGHT_THUMBSTICK_X, touch_x);
			add(c, 3, device_id::RIGHT_THUMBSTICK_Y, touch_y);
			add(c, 4, device_id::RIGHT_THUMBSTICK_CLICK, cs[c].thumbstick_click ? 1.0f : 0.0f);
			add(c, 5, device_id::A_CLICK, cs[c].button_a ? 1.0f : 0.0f);
			add(c, 6, device_id::B_CLICK, cs[c].button_b ? 1.0f : 0.0f);
			add(c, 7, device_id::SYSTEM_CLICK, cs[c].menu ? 1.0f : 0.0f);
			add(c, 8, device_id::RIGHT_SQUEEZE_VALUE, squeeze);
			add(c, 9, device_id::RIGHT_SQUEEZE_CLICK, squeeze);
		}
	}

	if (!pkt.values.empty())
		session->send_stream(pkt);
}
