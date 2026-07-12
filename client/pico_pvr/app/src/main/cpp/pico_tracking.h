#pragma once

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

struct XrFovf
{
	float angleLeft;
	float angleRight;
	float angleUp;
	float angleDown;
};

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
} // namespace neo2

struct controller_sample
{
	float orientation[4]{};
	float position[3]{};
	float angular_velocity[3]{};
	int trigger = 0;
	int touch[2]{};
	int battery = 0;
	bool connected = false;
	bool has_angular_velocity = false;
	bool button_a = false;
	bool button_b = false;
	bool grip = false;
	bool thumbstick_click = false;
	bool menu = false;
	bool home = false;
};
