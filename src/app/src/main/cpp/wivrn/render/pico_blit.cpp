#include "pico_blit.h"

#include <spdlog/spdlog.h>
#include <cstring>
#include <cmath>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

static const char * vert_src = R"(#version 310 es
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
out vec2 v_uv;
uniform vec2 u_tex_size;
void main()
{
    gl_Position = vec4(a_pos, 0.0, 1.0);
    v_uv = a_uv / u_tex_size;
}
)";

static const char * frag_src = R"(#version 310 es
#extension GL_OES_EGL_image_external_essl3 : require
precision mediump float;
in vec2 v_uv;
uniform samplerExternalOES u_tex;
out vec4 frag_color;
void main()
{
    frag_color = texture(u_tex, v_uv);
}
)";

static GLuint compile_shader(GLenum type, const char * src)
{
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &src, nullptr);
	glCompileShader(shader);
	GLint status = GL_FALSE;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (status != GL_TRUE)
	{
		char log[1024];
		glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
		spdlog::error("Shader compile error: {}", log);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

pico_blit_pipeline::~pico_blit_pipeline()
{
	if (vao)
		glDeleteVertexArrays(1, &vao);
	if (program)
		glDeleteProgram(program);
	if (vertex_buffer)
		glDeleteBuffers(1, &vertex_buffer);
}

void pico_blit_pipeline::init(int w, int h)
{
	eye_width.store(w);
	eye_height.store(h);

	GLuint vert = compile_shader(GL_VERTEX_SHADER, vert_src);
	GLuint frag = compile_shader(GL_FRAGMENT_SHADER, frag_src);
	if (!vert || !frag)
		throw std::runtime_error("Failed to compile blit shaders");

	program = glCreateProgram();
	glAttachShader(program, vert);
	glAttachShader(program, frag);
	glLinkProgram(program);
	GLint status = GL_FALSE;
	glGetProgramiv(program, GL_LINK_STATUS, &status);
	if (status != GL_TRUE)
	{
		char log[1024];
		glGetProgramInfoLog(program, sizeof(log), nullptr, log);
		spdlog::error("Program link error: {}", log);
		throw std::runtime_error("Failed to link blit program");
	}
	glDeleteShader(vert);
	glDeleteShader(frag);

	pos_attrib = 0;
	uv_attrib = 1;
	tex_uniform = glGetUniformLocation(program, "u_tex");
	tex_size_uniform = glGetUniformLocation(program, "u_tex_size");

	glGenVertexArrays(1, &vao);
	glGenBuffers(1, &vertex_buffer);

	glBindVertexArray(vao);
	glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
	glEnableVertexAttribArray(pos_attrib);
	glVertexAttribPointer(pos_attrib, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
	glEnableVertexAttribArray(uv_attrib);
	glVertexAttribPointer(uv_attrib, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
	glBindVertexArray(0);

	initialized = true;
	spdlog::info("GLES 3.1 de-foveation blit pipeline initialized ({}x{})", w, h);
}

size_t pico_blit_pipeline::required_vertices(const wivrn::to_headset::foveation_parameter & p)
{
	// Triangle strip with degenerate vertices to break rows.
	// Each y row produces (2 * (x_count + 1) + 1) vertices.
	return (2 * (p.x.size() + 1) + 1) * p.y.size();
}

static int count_pixels(const std::vector<uint16_t> & param)
{
	const int n_ratio = (int)(param.size() - 1) / 2;
	int res = 0;
	for (size_t i = 0; i < param.size(); ++i)
	{
		const int ratio = std::abs(n_ratio - (int)i) + 1;
		res += ratio * param[i];
	}
	return res;
}

void pico_blit_pipeline::draw(int eye, GLuint src_texture,
                               const wivrn::to_headset::foveation_parameter & foveation,
                               int src_width, int src_height)
{
	if (!initialized || src_texture == 0)
	{
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		return;
	}

	// Trivial foveation (no compression): fall back to a fullscreen quad.
	const bool trivial = foveation.x.size() <= 1 && foveation.y.size() <= 1;
	const size_t needed = trivial ? 4 : required_vertices(foveation);

	bool foveation_changed = vertex_dirty ||
		foveation.x != last_foveation.x ||
		foveation.y != last_foveation.y ||
		src_width != last_src_width ||
		src_height != last_src_height;

	if (foveation_changed)
	{
		if (vertex_data.size() < needed * 4)
			vertex_data.resize(needed * 4);

		if (trivial)
		{
			float * v = vertex_data.data();
			v[0] = -1.0f; v[1] = -1.0f; v[2] = 0.0f;          v[3] = (float)src_height;
			v[4] =  1.0f; v[5] = -1.0f; v[6] = (float)src_width; v[7] = (float)src_height;
			v[8] = -1.0f; v[9] =  1.0f; v[10] = 0.0f;          v[11] = 0.0f;
			v[12] = 1.0f; v[13] = 1.0f; v[14] = (float)src_width; v[15] = 0.0f;
		}
		else
		{
			const int n_ratio_x = (int)(foveation.x.size() - 1) / 2;
			const int n_ratio_y = (int)(foveation.y.size() - 1) / 2;
			const int out_w = count_pixels(foveation.x);
			const int out_h = count_pixels(foveation.y);

			const float out_pixel_w = 2.0f / std::max(1, out_w);
			const float out_pixel_h = 2.0f / std::max(1, out_h);

			float * v = vertex_data.data();
			uint32_t in_y = 0;
			float out_y = 0.5f * out_h;
			for (size_t iy = 0; iy < foveation.y.size(); ++iy)
			{
				const int ratio_y = std::abs(n_ratio_y - (int)iy) + 1;
				const uint16_t n_out_y = foveation.y[iy];

				uint32_t in_x = 0;
				float out_x = -0.5f * out_w;
				for (size_t ix = 0; ix < foveation.x.size(); ++ix)
				{
					const int ratio_x = std::abs(n_ratio_x - (int)ix) + 1;
					const uint16_t n_out_x = foveation.x[ix];

					*v++ = out_x * out_pixel_w;
					*v++ = out_y * out_pixel_h;
					*v++ = (float)in_x;
					*v++ = (float)in_y;

					*v++ = out_x * out_pixel_w;
					*v++ = (out_y - n_out_y * ratio_y) * out_pixel_h;
					*v++ = (float)in_x;
					*v++ = (float)(in_y + n_out_y);

					in_x += n_out_x;
					out_x += n_out_x * ratio_x;
				}
				*v++ = out_x * out_pixel_w;
				*v++ = out_y * out_pixel_h;
				*v++ = (float)in_x;
				*v++ = (float)in_y;

				in_y += n_out_y;
				out_y -= n_out_y * ratio_y;

				*v++ = out_x * out_pixel_w;
				*v++ = out_y * out_pixel_h;
				*v++ = (float)in_x;
				*v++ = (float)in_y;
				*v++ = out_x * out_pixel_w;
				*v++ = out_y * out_pixel_h;
				*v++ = (float)in_x;
				*v++ = (float)in_y;
			}
		}

		last_foveation = foveation;
		last_src_width = src_width;
		last_src_height = src_height;
		last_vertex_count = needed;
		vertex_dirty = false;
	}

	glViewport(0, 0, eye_width.load(), eye_height.load());
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);

	glUseProgram(program);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, src_texture);
	glUniform1i(tex_uniform, 0);
	glUniform2f(tex_size_uniform, (float)src_width, (float)src_height);

	glBindVertexArray(vao);
	if (foveation_changed)
		glBufferData(GL_ARRAY_BUFFER, needed * 4 * sizeof(float), vertex_data.data(), GL_DYNAMIC_DRAW);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, (GLsizei)needed);

	glBindVertexArray(0);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
	glUseProgram(0);
}

void pico_blit_pipeline::set_resolution(int w, int h)
{
	eye_width.store(w);
	eye_height.store(h);
}

