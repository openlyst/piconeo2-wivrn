#pragma once

/*
 * GLES blit pipeline for Pico Neo 2 WiVRn client.
 * Renders decoded video frames (as EGL external OES textures)
 * to the eye buffers via a simple fullscreen triangle.
 */

#include <GLES2/gl2.h>
#include <cstdint>

class neo2_blit_pipeline
{
	GLuint program = 0;
	GLuint vbo = 0;

	GLint attrib_pos = -1;
	GLint attrib_uv = -1;
	GLint uniform_tex = -1;

	int eye_w = 0;
	int eye_h = 0;

	bool ready = false;

public:
	neo2_blit_pipeline() = default;
	~neo2_blit_pipeline();

	void init(int w, int h);
	void draw(int eye, GLuint src_texture);
	bool is_ready() const { return ready; }
};
