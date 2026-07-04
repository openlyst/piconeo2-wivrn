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

	int eye_width = 0;
	int eye_height = 0;
	bool initialized = false;


public:
	pico_lobby() = default;
	~pico_lobby();

	void init(int w, int h);
	void draw(int eye, const float head_orient[4], const float head_pos[3],
	          const controller_sample controllers[2],
	          const XrFovf & fov, float ipd);
	bool is_initialized() const { return initialized; }
};
