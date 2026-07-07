#pragma once

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <time.h>

#include <openxr/openxr.h>

#include "wivrn_packets.h"

struct wivrn_session_pico;

namespace neo2
{
struct quat
{
	float x, y, z, w;
};

inline quat normalize_quat(const quat & q)
{
	float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
	if (n < 1e-9f)
		return {0, 0, 0, 1};
	return {q.x / n, q.y / n, q.z / n, q.w / n};
}

inline quat conjugate_quat(const quat & q)
{
	return {-q.x, -q.y, -q.z, q.w};
}

inline quat multiply_quat(const quat & a, const quat & b)
{
	return {
		a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
		a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
		a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
		a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

inline void rotate_vector(const quat & q, const float v[3], float out[3])
{
	float tx = 2.0f * (q.y * v[2] - q.z * v[1]);
	float ty = 2.0f * (q.z * v[0] - q.x * v[2]);
	float tz = 2.0f * (q.x * v[1] - q.y * v[0]);
	out[0] = v[0] + q.w * tx + (q.y * tz - q.z * ty);
	out[1] = v[1] + q.w * ty + (q.z * tx - q.x * tz);
	out[2] = v[2] + q.w * tz + (q.x * ty - q.y * tx);
}

inline XrQuaternionf to_xr_quat(const quat & q)
{
	return {q.x, q.y, q.z, q.w};
}
} // namespace neo2

struct controller_sample
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
	bool home = false;
};

class pico_native_tracker
{
public:
	wivrn_session_pico * session = nullptr;
	std::atomic<int64_t> time_offset_ns{0};

	XrFovf eye_fov[2]{
		XrFovf{-0.8814f, 0.8814f, 0.8814f, -0.8814f},
		XrFovf{-0.8814f, 0.8814f, 0.8814f, -0.8814f},
	};

	void start();
	void stop();

	void set_head_pose(const float orient[4], const float pos[3]);
	void get_head_pose(float out_orient[4], float out_pos[3]);
	void get_controllers(controller_sample out[2]);
	void set_prediction_ns(int64_t ns);
	std::atomic<bool> floor_relative{false};
	std::atomic<float> height_offset{1.5f};
	std::atomic<float> soft_ipd{0.064f};
	void update_controller(int hand, const float orient[4], const float pos[3],
	                       int trigger, const int touch[2], int battery,
	                       bool a, bool b, bool grip, bool click, bool menu);
	void update_controller_from_jni(int hand, int conn, const float * sensor,
	                                const float * ang_vel, const int * keys);
	void clear_controller(int hand);

private:
	std::thread thread;
	std::atomic<bool> running{false};

	std::mutex state_mutex;

	float head_orient[4]{0, 0, 0, 1};
	float head_pos[3]{0, 0, 0};
	bool head_valid = false;

	controller_sample controllers[2];

	// Velocity filter state (updated in set_head_pose on new data)
	float head_lin_vel[3]{0, 0, 0};
	float head_ang_vel[3]{0, 0, 0};
	float head_prev_pos[3]{0, 0, 0};
	neo2::quat head_prev_orient{0, 0, 0, 1};
	uint64_t head_prev_ts = 0;
	bool head_filter_init = false;
	bool height_calibrated = false;

	// Prediction offset from server's tracking_control
	std::atomic<int64_t> prediction_ns{0};
	std::atomic<bool> recenter_requested{false};

	float ctrl_lin_vel[2][3]{{0, 0, 0}, {0, 0, 0}};
	float ctrl_ang_vel[2][3]{{0, 0, 0}, {0, 0, 0}};
	float ctrl_prev_pos[2][3]{{0, 0, 0}, {0, 0, 0}};
	neo2::quat ctrl_prev_orient[2]{{0, 0, 0, 1}, {0, 0, 0, 1}};
	uint64_t ctrl_prev_ts[2]{0, 0};
	bool ctrl_filter_init[2]{false, false};

	struct input_state
	{
		float value = 0;
		XrTime last_change_time = 0;
	};
	input_state prev_inputs[2][10];

	void step_head_filter(const float pos[3], const neo2::quat & orient, uint64_t ts);
	void step_ctrl_filter(int hand, const float pos_m[3], const neo2::quat & orient, uint64_t ts);

	void run();
	void transmit_tracking(int64_t headset_ns);
	void transmit_inputs(int64_t headset_ns);

	static int64_t now_ns()
	{
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
	}

	int64_t to_xr_time(int64_t ns)
	{
		return ns + time_offset_ns.load();
	}
};
