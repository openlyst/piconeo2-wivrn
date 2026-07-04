#pragma once

#include <GLES2/gl2.h>
#include <openxr/openxr.h>
#include "wivrn_packets.h"

#include <array>
#include <cstdint>
#include <vector>

class pico_blit_pipeline
{
	GLuint program = 0;
	GLuint vertex_buffer = 0;

	GLint pos_attrib = -1;
	GLint uv_attrib = -1;
	GLint tex_uniform = -1;
	GLint tex_size_uniform = -1;

	int eye_width = 0;
	int eye_height = 0;

	std::vector<float> vertex_data;
	bool initialized = false;

	static size_t required_vertices(const wivrn::to_headset::foveation_parameter & p);

public:
	pico_blit_pipeline() = default;
	~pico_blit_pipeline();

	void init(int w, int h);
	void draw(int eye, GLuint src_texture,
	          const wivrn::to_headset::foveation_parameter & foveation,
	          int src_width, int src_height);
	bool is_initialized() const { return initialized; }
};
