#pragma once

#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <openxr/openxr.h>
#include "wivrn_packets.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

class pico_blit_pipeline
{
	GLuint program = 0;
	GLuint vertex_buffer = 0;
	GLuint vao = 0;

	GLint pos_attrib = -1;
	GLint uv_attrib = -1;
	GLint tex_uniform = -1;
	GLint tex_size_uniform = -1;

	std::atomic<int> eye_width{0};
	std::atomic<int> eye_height{0};

	std::vector<float> vertex_data;
	bool initialized = false;

	wivrn::to_headset::foveation_parameter last_foveation;
	int last_src_width = 0;
	int last_src_height = 0;
	size_t last_vertex_count = 0;
	bool vertex_dirty = true;

	static size_t required_vertices(const wivrn::to_headset::foveation_parameter & p);

public:
	pico_blit_pipeline() = default;
	~pico_blit_pipeline();

	GLuint get_vao() const { return vao; }

	void init(int w, int h);
	void set_resolution(int w, int h);
	void draw(int eye, GLuint src_texture,
	          const wivrn::to_headset::foveation_parameter & foveation,
	          int src_width, int src_height);
	bool is_initialized() const { return initialized; }
};
