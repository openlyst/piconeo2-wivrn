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

	// UI texture program (textured quad)
	GLuint tex_program = 0;
	GLuint ui_fbo = 0;
	GLuint ui_texture = 0;
	GLuint quad_vbo = 0;
	GLint tex_pos_attrib = -1;
	GLint tex_uv_attrib = -1;
	GLint tex_mvp_uniform = -1;
	GLint tex_sampler_uniform = -1;

	static constexpr int ui_w = 1024;
	static constexpr int ui_h = 1024;

	// TV panel state (world-locked)
	float tv_pos[3] = {0, 1.5f, -2.0f};
	float tv_yaw = 0.0f;
	bool tv_placed = false;
	bool prev_grip[2] = {false, false};
	bool prev_stick_click[2] = {false, false};

	int eye_width = 0;
	int eye_height = 0;
	bool initialized = false;

	void render_ui();
	void draw_quad(const float head_orient[4], const float head_pos[3],
	               const XrFovf & fov, float ipd, int eye);
	void update_interaction(const float head_orient[4], const float head_pos[3],
	                        const controller_sample controllers[2]);

public:
	pico_lobby() = default;
	~pico_lobby();

	void init(int w, int h);
	void draw(int eye, const float head_orient[4], const float head_pos[3],
	          const controller_sample controllers[2],
	          const XrFovf & fov, float ipd);
	bool is_initialized() const { return initialized; }
};
