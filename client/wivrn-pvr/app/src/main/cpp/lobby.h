#pragma once

#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <openxr/openxr.h>
#include <jni.h>
#include <vector>
#include <mutex>
#include <atomic>
#include <cmath>
#include <cstring>

// Shared with pico_native tracker (same layout).
#include "pico_tracking.h"

class pico_lobby
{
	GLuint program = 0;
	GLuint controller_vbo = 0;
	GLuint ray_vbo = 0;
	GLuint controller_vao = 0;
	GLuint ray_vao = 0;

	GLint pos_attrib = -1;
	GLint mvp_uniform = -1;
	GLint color_uniform = -1;

	GLuint tex_program = 0;
	GLuint ui_texture = 0;
	GLuint quad_vbo = 0;
	GLuint quad_vao = 0;
	GLint tex_pos_attrib = -1;
	GLint tex_uv_attrib = -1;
	GLint tex_mvp_uniform = -1;
	GLint tex_sampler_uniform = -1;

	std::atomic<int> eye_width{0};
	std::atomic<int> eye_height{0};
	bool initialized = false;

	std::mutex tex_mutex;
	std::atomic<bool> frame_available{false};
	jobject surface_texture = nullptr;
	jmethodID update_tex_image_method = nullptr;
	float tex_matrix[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
	bool tex_matrix_valid = false;

	std::vector<uint8_t> pending_tex_data;
	int pending_tex_w = 0;
	int pending_tex_h = 0;
	bool tex_pending = false;

	float panel_pos[3] = {0, 0.0f, -2.5f};
	float panel_yaw = 0.0f;
	float panel_pitch = 0.0f;
	bool prev_trigger[2] = {false, false};
	bool prev_head_trigger = false;

	// Grip-to-grab state. When the user holds grip while pointing at the
	// panel, the panel follows the controller freely in 3D space.
	bool prev_grip[2] = {false, false};
	bool grabbing = false;
	int  grab_hand = -1;
	// Grab point in panel-local UV (-1..1) and distance from controller
	// to the grab point, both captured at grab start. Each frame we
	// project a point at that distance along the controller ray and
	// offset by the panel-local grab UV to get the new panel center.
	float grab_u = 0;
	float grab_v = 0;
	float grab_dist = 0;

	struct ray_hit
	{
		bool valid = false;
		float u = 0.5f;
		float v = 0.5f;
	};
	ray_hit last_hit[2];
	ray_hit head_hit;

	bool debug_laser_hit = false;
	int debug_hit_hand = -1;
	float debug_hit_u = 0;
	float debug_hit_v = 0;
	float debug_touch_x = -1;
	float debug_touch_y = -1;
	bool debug_trigger_down = false;
	int debug_frame_count = 0;

	static constexpr float panel_w = 1.2f;
	static constexpr float panel_h = 0.77f;

	void draw_quad(const float head_orient[4], const float head_pos[3],
	               const XrFovf & fov, float ipd, int eye);
	void update_interaction(const float head_orient[4], const float head_pos[3],
	                        const controller_sample controllers[2], bool head_trigger);

public:
	float lobby_touch_x[2] = {-1, -1};
	float lobby_touch_y[2] = {-1, -1};
	bool lobby_touch_down[2] = {false, false};
	bool lobby_touch_pressed[2] = {false, false};
	float lobby_thumbstick_y[2] = {0, 0};
	float prev_lobby_touch_x[2] = {-2, -2};
	float prev_lobby_touch_y[2] = {-2, -2};
	bool prev_lobby_touch_down[2] = {false, false};
	bool prev_lobby_touch_pressed[2] = {false, false};

	float head_touch_x = -1;
	float head_touch_y = -1;
	bool head_touch_down = false;
	bool head_touch_pressed = false;
	float prev_head_touch_x = -2;
	float prev_head_touch_y = -2;
	bool prev_head_touch_down = false;
	bool prev_head_touch_pressed = false;
	pico_lobby() = default;
	~pico_lobby();

	void init(int w, int h);
	void set_resolution(int w, int h);
	void draw(int eye, const float head_orient[4], const float head_pos[3],
	          const controller_sample controllers[2],
	          const XrFovf & fov, float ipd, bool head_trigger,
	          bool overlay = false, bool draw_controllers = true);
	bool is_initialized() const { return initialized; }

	void recenter(const float head_pos[3] = nullptr, float head_yaw = 0.0f);
	void recenter_facing(const float head_pos[3], float fwd_x, float fwd_z);

	void update_texture(int width, int height, const void * pixels);
	void update_texture_argb(int width, int height, const uint32_t * pixels);
	void flush_pending_texture();
	GLuint get_external_texture();
	void set_surface_texture(jobject st, jmethodID update_method);
	void update_tex_image(JNIEnv* env);
	void on_frame_available();
};

void push_lobby_touch_to_java(int hand, float x, float y, bool down, bool pressed, float thumbstickY);
