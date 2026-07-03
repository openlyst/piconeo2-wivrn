/*
 * GLES blit pipeline implementation for Pico Neo 2 WiVRn client.
 * Simple fullscreen triangle shader for EGL external OES texture display.
 */

#include "neo2_blit.h"

#include <spdlog/spdlog.h>
#include <cstring>
#include <GLES2/gl2ext.h>

static const char * vert_shader_src = R"(
attribute vec2 a_pos;
attribute vec2 a_uv;
varying vec2 v_uv;
void main()
{
    gl_Position = vec4(a_pos, 0.0, 1.0);
    v_uv = vec2(a_uv.x, 1.0 - a_uv.y);
}
)";

static const char * frag_shader_src = R"(
#extension GL_OES_EGL_image_external : require
precision mediump float;
varying vec2 v_uv;
uniform samplerExternalOES u_tex;
void main()
{
    gl_FragColor = texture2D(u_tex, v_uv);
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

neo2_blit_pipeline::~neo2_blit_pipeline()
{
	if (program)
		glDeleteProgram(program);
	if (vbo)
		glDeleteBuffers(1, &vbo);
}

void neo2_blit_pipeline::init(int w, int h)
{
	eye_w = w;
	eye_h = h;

	GLuint vert = compile_shader(GL_VERTEX_SHADER, vert_shader_src);
	GLuint frag = compile_shader(GL_FRAGMENT_SHADER, frag_shader_src);
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

	attrib_pos = glGetAttribLocation(program, "a_pos");
	attrib_uv = glGetAttribLocation(program, "a_uv");
	uniform_tex = glGetUniformLocation(program, "u_tex");

	float verts[] = {
		-1.0f, -1.0f,  0.0f, 0.0f,
		 3.0f, -1.0f,  2.0f, 0.0f,
		-1.0f,  3.0f,  0.0f, 2.0f,
	};

	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	ready = true;
	spdlog::info("GLES blit pipeline initialized ({}x{})", w, h);
}

void neo2_blit_pipeline::draw(int eye, GLuint src_texture)
{
	if (!ready || src_texture == 0)
	{
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		return;
	}

	glViewport(0, 0, eye_w, eye_h);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);

	glUseProgram(program);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, src_texture);
	glUniform1i(uniform_tex, 0);

	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glEnableVertexAttribArray(attrib_pos);
	glVertexAttribPointer(attrib_pos, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
	glEnableVertexAttribArray(attrib_uv);
	glVertexAttribPointer(attrib_uv, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);

	glDrawArrays(GL_TRIANGLES, 0, 3);

	glDisableVertexAttribArray(attrib_pos);
	glDisableVertexAttribArray(attrib_uv);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
	glUseProgram(0);
}
