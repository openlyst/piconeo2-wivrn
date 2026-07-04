#include "pico_lobby.h"
#include <spdlog/spdlog.h>
#include <cstring>
#include <cmath>
#include <vector>

static const char * vert_src = R"(
attribute vec3 a_pos;
uniform mat4 u_mvp;
void main()
{
    gl_Position = u_mvp * vec4(a_pos, 1.0);
}
)";

static const char * frag_src = R"(
precision mediump float;
uniform vec4 u_color;
void main()
{
    gl_FragColor = u_color;
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
		spdlog::error("Lobby shader compile error: {}", log);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

struct Mat4
{
	float m[16];
};

static Mat4 mat4_identity()
{
	Mat4 r;
	memset(r.m, 0, sizeof(r.m));
	r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
	return r;
}

static Mat4 mat4_mul(const Mat4 & a, const Mat4 & b)
{
	Mat4 r;
	for (int col = 0; col < 4; col++)
		for (int row = 0; row < 4; row++)
		{
			float s = 0;
			for (int k = 0; k < 4; k++)
				s += a.m[k * 4 + row] * b.m[col * 4 + k];
			r.m[col * 4 + row] = s;
		}
	return r;
}

static Mat4 mat4_perspective(float fov_left, float fov_right, float fov_up, float fov_down, float near_z, float far_z)
{
	float l = tanf(fov_left) * near_z;
	float r = tanf(fov_right) * near_z;
	float d = tanf(fov_down) * near_z;
	float u = tanf(fov_up) * near_z;

	Mat4 p;
	memset(p.m, 0, sizeof(p.m));
	p.m[0] = 2.0f * near_z / (r - l);
	p.m[5] = 2.0f * near_z / (u - d);
	p.m[8] = (r + l) / (r - l);
	p.m[9] = (u + d) / (u - d);
	p.m[10] = -(far_z + near_z) / (far_z - near_z);
	p.m[11] = -1.0f;
	p.m[14] = -2.0f * far_z * near_z / (far_z - near_z);
	return p;
}

static Mat4 mat4_translate(float x, float y, float z)
{
	Mat4 r = mat4_identity();
	r.m[12] = x;
	r.m[13] = y;
	r.m[14] = z;
	return r;
}

static Mat4 quat_to_mat4(const float q[4])
{
	float x = q[0], y = q[1], z = q[2], w = q[3];
	float n = sqrtf(x*x + y*y + z*z + w*w);
	if (n < 1e-9f) return mat4_identity();
	x /= n; y /= n; z /= n; w /= n;

	Mat4 r = mat4_identity();
	r.m[0] = 1 - 2*(y*y + z*z);
	r.m[1] = 2*(x*y + w*z);
	r.m[2] = 2*(x*z - w*y);
	r.m[4] = 2*(x*y - w*z);
	r.m[5] = 1 - 2*(x*x + z*z);
	r.m[6] = 2*(y*z + w*x);
	r.m[8] = 2*(x*z + w*y);
	r.m[9] = 2*(y*z - w*x);
	r.m[10] = 1 - 2*(x*x + y*y);
	return r;
}

static Mat4 mat4_view(const float orient[4], const float pos[3])
{
	Mat4 rot = quat_to_mat4(orient);
	// Inverse rotation (transpose for rotation matrix)
	Mat4 inv_rot = mat4_identity();
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			inv_rot.m[i * 4 + j] = rot.m[j * 4 + i];

	Mat4 trans = mat4_translate(-pos[0], -pos[1], -pos[2]);
	return mat4_mul(inv_rot, trans);
}

pico_lobby::~pico_lobby()
{
	if (program) glDeleteProgram(program);
	if (grid_vbo) glDeleteBuffers(1, &grid_vbo);
	if (controller_vbo) glDeleteBuffers(1, &controller_vbo);
}

void pico_lobby::init(int w, int h)
{
	eye_width = w;
	eye_height = h;

	GLuint vert = compile_shader(GL_VERTEX_SHADER, vert_src);
	GLuint frag = compile_shader(GL_FRAGMENT_SHADER, frag_src);
	if (!vert || !frag)
		throw std::runtime_error("Failed to compile lobby shaders");

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
		spdlog::error("Lobby program link error: {}", log);
		glDeleteShader(vert);
		glDeleteShader(frag);
		throw std::runtime_error("Failed to link lobby program");
	}
	glDeleteShader(vert);
	glDeleteShader(frag);

	pos_attrib = glGetAttribLocation(program, "a_pos");
	mvp_uniform = glGetUniformLocation(program, "u_mvp");
	color_uniform = glGetUniformLocation(program, "u_color");

	// Floor grid: lines on the XZ plane at y=0, from -5 to +5, every 0.5m
	std::vector<float> grid_verts;
	float grid_size = 5.0f;
	float grid_step = 0.5f;
	for (float x = -grid_size; x <= grid_size; x += grid_step)
	{
		grid_verts.push_back(x); grid_verts.push_back(0.0f); grid_verts.push_back(-grid_size);
		grid_verts.push_back(x); grid_verts.push_back(0.0f); grid_verts.push_back(grid_size);
	}
	for (float z = -grid_size; z <= grid_size; z += grid_step)
	{
		grid_verts.push_back(-grid_size); grid_verts.push_back(0.0f); grid_verts.push_back(z);
		grid_verts.push_back(grid_size); grid_verts.push_back(0.0f); grid_verts.push_back(z);
	}

	glGenBuffers(1, &grid_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, grid_vbo);
	glBufferData(GL_ARRAY_BUFFER, grid_verts.size() * sizeof(float), grid_verts.data(), GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	// Controller representation: a small box (24 verts = 12 triangles)
	// Simple 0.04m cube
	float s = 0.04f;
	float box[] = {
		// front
		-s,-s, s,  s,-s, s,  s, s, s,  -s,-s, s,  s, s, s,  -s, s, s,
		// back
		-s,-s,-s,  -s, s,-s,  s, s,-s,  -s,-s,-s,  s, s,-s,  s,-s,-s,
		// top
		-s, s, s,  s, s, s,  s, s,-s,  -s, s, s,  s, s,-s,  -s, s,-s,
		// bottom
		-s,-s, s,  -s,-s,-s,  s,-s,-s,  -s,-s, s,  s,-s,-s,  s,-s, s,
		// right
		 s,-s, s,  s,-s,-s,  s, s,-s,   s,-s, s,  s, s,-s,  s, s, s,
		// left
		-s,-s, s,  -s, s, s,  -s, s,-s,  -s,-s, s,  -s, s,-s, -s,-s,-s,
	};
	glGenBuffers(1, &controller_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, controller_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(box), box, GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	initialized = true;
	spdlog::info("Lobby initialized ({}x{})", w, h);
}

void pico_lobby::draw(int eye, const float head_orient[4], const float head_pos[3],
                      const controller_sample controllers[2],
                      const XrFovf & fov, float ipd)
{
	if (!initialized)
	{
		glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		return;
	}

	glViewport(0, 0, eye_width, eye_height);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);

	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	glUseProgram(program);

	Mat4 proj = mat4_perspective(fov.angleLeft, fov.angleRight, fov.angleUp, fov.angleDown, 0.1f, 100.0f);

	// View = inverse(head rotation) * inverse(head translation + IPD offset)
	neo2::quat hq = neo2::normalize_quat({head_orient[0], head_orient[1], head_orient[2], head_orient[3]});

	float eye_offset = (eye == 0 ? -ipd * 0.5f : ipd * 0.5f);
	float v[3] = {eye_offset, 0, 0};
	float rotated[3];
	neo2::rotate_vector(hq, v, rotated);

	float eye_pos[3] = {head_pos[0] + rotated[0], head_pos[1] + rotated[1], head_pos[2] + rotated[2]};
	Mat4 view = mat4_view(head_orient, eye_pos);

	Mat4 vp = mat4_mul(proj, view);

	// Draw floor grid
	glUniform4f(color_uniform, 0.5f, 0.5f, 0.8f, 1.0f);
	glBindBuffer(GL_ARRAY_BUFFER, grid_vbo);
	glEnableVertexAttribArray(pos_attrib);
	glVertexAttribPointer(pos_attrib, 3, GL_FLOAT, GL_FALSE, 12, (void *)0);
	int grid_vert_count = 0;
	glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &grid_vert_count);
	grid_vert_count /= (3 * sizeof(float));
	glUniformMatrix4fv(mvp_uniform, 1, GL_FALSE, vp.m);
	glDrawArrays(GL_LINES, 0, grid_vert_count);

	// Draw controllers as small boxes
	for (int h = 0; h < 2; h++)
	{
		if (!controllers[h].connected)
			continue;

		float pos_m[3] = {
			controllers[h].position[0] * 0.001f,
			controllers[h].position[1] * 0.001f,
			controllers[h].position[2] * 0.001f,
		};

		Mat4 model = mat4_mul(mat4_translate(pos_m[0], pos_m[1], pos_m[2]), quat_to_mat4(controllers[h].orientation));
		Mat4 mvp = mat4_mul(vp, model);

		if (h == 0)
			glUniform4f(color_uniform, 0.2f, 0.4f, 1.0f, 1.0f);
		else
			glUniform4f(color_uniform, 1.0f, 0.3f, 0.2f, 1.0f);

		glBindBuffer(GL_ARRAY_BUFFER, controller_vbo);
		glVertexAttribPointer(pos_attrib, 3, GL_FLOAT, GL_FALSE, 12, (void *)0);
		glUniformMatrix4fv(mvp_uniform, 1, GL_FALSE, mvp.m);
		glDrawArrays(GL_TRIANGLES, 0, 36);
	}

	// Head position marker (green)
	{
		Mat4 model = mat4_translate(head_pos[0], head_pos[1], head_pos[2]);
		Mat4 mvp = mat4_mul(vp, model);
		glUniform4f(color_uniform, 0.2f, 1.0f, 0.2f, 1.0f);
		glBindBuffer(GL_ARRAY_BUFFER, controller_vbo);
		glVertexAttribPointer(pos_attrib, 3, GL_FLOAT, GL_FALSE, 12, (void *)0);
		glUniformMatrix4fv(mvp_uniform, 1, GL_FALSE, mvp.m);
		glDrawArrays(GL_TRIANGLES, 0, 36);
	}

	glDisableVertexAttribArray(pos_attrib);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glUseProgram(0);
}
