#pragma once

#include <GLES2/gl2.h>
#include "pico_tracking.h"

class pico_lobby
{
	GLuint program = 0;
	GLuint grid_vbo = 0;
	GLuint controller_vbo = 0;
	GLint pos_attrib = -1;
	GLint mvp_uniform = -1;
	GLint color_uniform = -1;

	GLuint tex_program = 0;
	GLuint ui_texture = 0;
	GLuint quad_vbo = 0;
	GLint tex_pos_attrib = -1;
	GLint tex_uv_attrib = -1;
	GLint tex_mvp_uniform = -1;
	GLint tex_sampler_uniform = -1;

	int eye_width = 0;
	int eye_height = 0;
	bool initialized = false;

	float panel_pos[3] = {0, 1.5f, -2.0f};
	float panel_yaw = 0.0f;
	bool panel_placed = false;
	bool prev_trigger[2] = {false, false};
	bool prev_grip[2] = {false, false};

	int recentering_controller = -1;
	float recenter_offset_pos[3] = {0, 0, 0};
	float recenter_offset_orient[4] = {0, 0, 0, 1};

	struct ray_hit
	{
		bool valid = false;
		float u = 0.5f;
		float v = 0.5f;
	};
	ray_hit last_hit[2];

	static constexpr float panel_w = 2.0f;
	static constexpr float panel_h = 1.286f;

	void draw_quad(const float head_orient[4], const float head_pos[3],
	               const XrFovf & fov, float ipd, int eye);
	void update_interaction(const float head_orient[4], const float head_pos[3],
	                        const controller_sample controllers[2]);

public:
	float lobby_touch_x[2] = {-1, -1};
	float lobby_touch_y[2] = {-1, -1};
	bool lobby_touch_down[2] = {false, false};
	bool lobby_touch_pressed[2] = {false, false};
	pico_lobby() = default;
	~pico_lobby();

	void init(int w, int h);
	void draw(int eye, const float head_orient[4], const float head_pos[3],
	          const controller_sample controllers[2],
	          const XrFovf & fov, float ipd);
	bool is_initialized() const { return initialized; }

	void update_texture(int width, int height, const void * pixels);
};
