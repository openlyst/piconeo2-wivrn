#include "pico_blit.h"

#include <spdlog/spdlog.h>
#include <cstring>
#include <cmath>
#include <GLES2/gl2ext.h>

static const char * vert_src = R"(
attribute vec2 a_pos;
attribute vec2 a_uv;
varying vec2 v_uv;
void main()
{
    gl_Position = vec4(a_pos, 0.0, 1.0);
    v_uv = a_uv;
}
)";

static const char * frag_src = R"(
#extension GL_OES_EGL_image_external : require
precision highp float;
varying vec2 v_uv;
uniform samplerExternalOES u_tex;
uniform vec4 u_deltaQuat;
uniform float u_tanL;
uniform float u_tanR;
uniform float u_tanU;
uniform float u_tanD;

vec3 quat_rotate(vec4 q, vec3 v)
{
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

void main()
{
    vec3 dir = vec3(
        mix(-u_tanL, u_tanR, v_uv.x),
        mix(-u_tanD, u_tanU, v_uv.y),
        -1.0
    );

    vec3 sd = quat_rotate(u_deltaQuat, dir);

    float sx = sd.x / -sd.z;
    float sy = sd.y / -sd.z;
    vec2 uv = vec2(
        (sx + u_tanL) / (u_tanL + u_tanR),
        (sy + u_tanD) / (u_tanD + u_tanU)
    );

    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
        gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
    else
        gl_FragColor = texture2D(u_tex, vec2(uv.x, 1.0 - uv.y));
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

static void multiply_quat(const XrQuaternionf & a, const XrQuaternionf & b, XrQuaternionf & out)
{
	out.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
	out.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
	out.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
	out.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
}

static void conjugate_quat(const XrQuaternionf & q, XrQuaternionf & out)
{
	out.x = -q.x;
	out.y = -q.y;
	out.z = -q.z;
	out.w = q.w;
}

static void normalize_quat(XrQuaternionf & q)
{
	float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
	if (n > 1e-9f)
	{
		q.x /= n; q.y /= n; q.z /= n; q.w /= n;
	}
	else
	{
		q = {0, 0, 0, 1};
	}
}

pico_blit_pipeline::~pico_blit_pipeline()
{
	if (program)
		glDeleteProgram(program);
	if (vertex_buffer)
		glDeleteBuffers(1, &vertex_buffer);
}

void pico_blit_pipeline::init(int w, int h)
{
	eye_width = w;
	eye_height = h;

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

	pos_attrib = glGetAttribLocation(program, "a_pos");
	uv_attrib = glGetAttribLocation(program, "a_uv");
	tex_uniform = glGetUniformLocation(program, "u_tex");
	delta_quat_uniform = glGetUniformLocation(program, "u_deltaQuat");
	tan_l_uniform = glGetUniformLocation(program, "u_tanL");
	tan_r_uniform = glGetUniformLocation(program, "u_tanR");
	tan_u_uniform = glGetUniformLocation(program, "u_tanU");
	tan_d_uniform = glGetUniformLocation(program, "u_tanD");

	float verts[] = {
		-1.0f, -1.0f,  0.0f, 0.0f,
		 3.0f, -1.0f,  2.0f, 0.0f,
		-1.0f,  3.0f,  0.0f, 2.0f,
	};

	glGenBuffers(1, &vertex_buffer);
	glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	initialized = true;
	spdlog::info("GLES blit pipeline with timewarp initialized ({}x{})", w, h);
}

void pico_blit_pipeline::draw(int eye, GLuint src_texture,
                               const XrPosef & server_pose, const XrPosef & current_pose,
                               const XrFovf & fov)
{
	if (!initialized || src_texture == 0)
	{
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		return;
	}

	XrQuaternionf s = server_pose.orientation;
	XrQuaternionf c = current_pose.orientation;
	normalize_quat(s);
	normalize_quat(c);

	XrQuaternionf s_inv;
	conjugate_quat(s, s_inv);

	XrQuaternionf delta;
	multiply_quat(s_inv, c, delta);
	normalize_quat(delta);

	float tanL = std::tan(-fov.angleLeft);
	float tanR = std::tan(fov.angleRight);
	float tanU = std::tan(fov.angleUp);
	float tanD = std::tan(-fov.angleDown);

	glViewport(0, 0, eye_width, eye_height);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);

	glUseProgram(program);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, src_texture);
	glUniform1i(tex_uniform, 0);

	glUniform4f(delta_quat_uniform, delta.x, delta.y, delta.z, delta.w);
	glUniform1f(tan_l_uniform, tanL);
	glUniform1f(tan_r_uniform, tanR);
	glUniform1f(tan_u_uniform, tanU);
	glUniform1f(tan_d_uniform, tanD);

	glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
	glEnableVertexAttribArray(pos_attrib);
	glVertexAttribPointer(pos_attrib, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
	glEnableVertexAttribArray(uv_attrib);
	glVertexAttribPointer(uv_attrib, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);

	glDrawArrays(GL_TRIANGLES, 0, 3);

	glDisableVertexAttribArray(pos_attrib);
	glDisableVertexAttribArray(uv_attrib);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
	glUseProgram(0);
}

