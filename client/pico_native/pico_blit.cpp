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
uniform vec4 u_delta_q;
uniform vec2 u_tan_fov;

vec3 rotate_quat(vec3 v, vec4 q)
{
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

void main()
{
    // Convert UV to view direction in eye space
    vec2 ndc = v_uv * 2.0 - 1.0;
    vec3 dir_eye = normalize(vec3(ndc.x * u_tan_fov.x, ndc.y * u_tan_fov.y, 1.0));

    // Rotate by delta quaternion (server -> current)
    vec3 dir_reproj = rotate_quat(dir_eye, u_delta_q);

    // Project back to UV
    vec3 dir_norm = normalize(dir_reproj);
    vec2 reproj_ndc = vec2(dir_norm.x / dir_norm.z, dir_norm.y / dir_norm.z);
    vec2 reproj_uv = reproj_ndc / (u_tan_fov * 2.0) + 0.5;

    if (reproj_uv.x < 0.0 || reproj_uv.x > 1.0 || reproj_uv.y < 0.0 || reproj_uv.y > 1.0)
    {
        gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
    }
    else
    {
        gl_FragColor = texture2D(u_tex, vec2(reproj_uv.x, 1.0 - reproj_uv.y));
    }
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
	delta_q_uniform = glGetUniformLocation(program, "u_delta_q");
	tan_fov_uniform = glGetUniformLocation(program, "u_tan_fov");

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
	spdlog::info("GLES blit pipeline initialized ({}x{})", w, h);
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

	glViewport(0, 0, eye_width, eye_height);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);

	glUseProgram(program);

	// Compute delta quaternion: q_delta = inverse(server) * current
	// For unit quaternions, inverse = conjugate
	float sx = server_pose.orientation.x;
	float sy = server_pose.orientation.y;
	float sz = server_pose.orientation.z;
	float sw = server_pose.orientation.w;
	float sn = std::sqrt(sx*sx + sy*sy + sz*sz + sw*sw);
	if (sn > 1e-6f) { sx /= sn; sy /= sn; sz /= sn; sw /= sn; }

	float cx = current_pose.orientation.x;
	float cy = current_pose.orientation.y;
	float cz = current_pose.orientation.z;
	float cw = current_pose.orientation.w;
	float cn = std::sqrt(cx*cx + cy*cy + cz*cz + cw*cw);
	if (cn > 1e-6f) { cx /= cn; cy /= cn; cz /= cn; cw /= cn; }

	// inverse(server) = conjugate(server) = (-sx, -sy, -sz, sw)
	// q_delta = inverse(server) * current
	float dx = -sw*cx + sx*cw - sy*cz + sz*cy;
	float dy = -sw*cy + sx*cz + sy*cw - sz*cx;
	float dz = -sw*cz - sx*cy + sy*cx + sz*cw;
	float dw =  sw*cw + sx*cx + sy*cy + sz*cz;

	glUniform4f(delta_q_uniform, dx, dy, dz, dw);

	float tan_x = (-fov.angleLeft + fov.angleRight) * 0.5f;
	float tan_y = (-fov.angleDown + fov.angleUp) * 0.5f;
	glUniform2f(tan_fov_uniform, tan_x, tan_y);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, src_texture);
	glUniform1i(tex_uniform, 0);

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

