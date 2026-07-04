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

static const char * tex_vert_src = R"(
attribute vec3 a_pos;
attribute vec2 a_uv;
uniform mat4 u_mvp;
varying vec2 v_uv;
void main()
{
    gl_Position = u_mvp * vec4(a_pos, 1.0);
    v_uv = a_uv;
}
)";

static const char * tex_frag_src = R"(
precision mediump float;
varying vec2 v_uv;
uniform sampler2D u_tex;
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

static Mat4 mat4_scale(float x, float y, float z)
{
	Mat4 r = mat4_identity();
	r.m[0] = x;
	r.m[5] = y;
	r.m[10] = z;
	return r;
}

static Mat4 mat4_rotate_y(float angle)
{
	float c = cosf(angle), s = sinf(angle);
	Mat4 r = mat4_identity();
	r.m[0] = c;  r.m[2] = s;
	r.m[8] = -s; r.m[10] = c;
	return r;
}

static Mat4 mat4_ortho(float left, float right, float bottom, float top, float near_z, float far_z)
{
	Mat4 r;
	memset(r.m, 0, sizeof(r.m));
	r.m[0] = 2.0f / (right - left);
	r.m[5] = 2.0f / (top - bottom);
	r.m[10] = -2.0f / (far_z - near_z);
	r.m[12] = -(right + left) / (right - left);
	r.m[13] = -(top + bottom) / (top - bottom);
	r.m[14] = -(far_z + near_z) / (far_z - near_z);
	r.m[15] = 1.0f;
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
	if (tex_program) glDeleteProgram(tex_program);
	if (grid_vbo) glDeleteBuffers(1, &grid_vbo);
	if (controller_vbo) glDeleteBuffers(1, &controller_vbo);
	if (quad_vbo) glDeleteBuffers(1, &quad_vbo);
	if (ui_texture) glDeleteTextures(1, &ui_texture);
	if (ui_fbo) glDeleteFramebuffers(1, &ui_fbo);
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

	// UI texture + FBO for rendering the WiVRn lobby screen
	glGenTextures(1, &ui_texture);
	glBindTexture(GL_TEXTURE_2D, ui_texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ui_w, ui_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);

	glGenFramebuffers(1, &ui_fbo);

	// Quad VBO for the TV screen (position xyz + uv)
	float quad[] = {
		-1.0f, -1.0f, 0.0f,  0.0f, 0.0f,
		 1.0f, -1.0f, 0.0f,  1.0f, 0.0f,
		 1.0f,  1.0f, 0.0f,  1.0f, 1.0f,
		-1.0f, -1.0f, 0.0f,  0.0f, 0.0f,
		 1.0f,  1.0f, 0.0f,  1.0f, 1.0f,
		-1.0f,  1.0f, 0.0f,  0.0f, 1.0f,
	};
	glGenBuffers(1, &quad_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	// Texture shader program
	GLuint tvert = compile_shader(GL_VERTEX_SHADER, tex_vert_src);
	GLuint tfrag = compile_shader(GL_FRAGMENT_SHADER, tex_frag_src);
	tex_program = glCreateProgram();
	glAttachShader(tex_program, tvert);
	glAttachShader(tex_program, tfrag);
	glLinkProgram(tex_program);
	glGetProgramiv(tex_program, GL_LINK_STATUS, &status);
	if (status != GL_TRUE)
	{
		char log[1024];
		glGetProgramInfoLog(tex_program, sizeof(log), nullptr, log);
		spdlog::error("Lobby tex program link error: {}", log);
	}
	glDeleteShader(tvert);
	glDeleteShader(tfrag);

	tex_pos_attrib = glGetAttribLocation(tex_program, "a_pos");
	tex_uv_attrib = glGetAttribLocation(tex_program, "a_uv");
	tex_mvp_uniform = glGetUniformLocation(tex_program, "u_mvp");
	tex_sampler_uniform = glGetUniformLocation(tex_program, "u_tex");

	// Render the UI once (static content)
	render_ui();

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

	if (eye == 0)
		update_interaction(head_orient, head_pos, controllers);

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

	// Draw the TV screen with WiVRn UI in front of the user
	draw_quad(head_orient, head_pos, fov, ipd, eye);
}

// Helper: draw a filled rectangle in 2D pixel coordinates into the UI FBO
static void ui_fill_rect(GLint pos_loc, GLint color_loc, float x0, float y0, float x1, float y1,
                         float r, float g, float b, float a)
{
	float verts[] = {
		x0, y0, 0,
		x1, y0, 0,
		x1, y1, 0,
		x0, y0, 0,
		x1, y1, 0,
		x0, y1, 0,
	};
	GLuint vbo;
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
	glEnableVertexAttribArray(pos_loc);
	glVertexAttribPointer(pos_loc, 3, GL_FLOAT, GL_FALSE, 12, (void *)0);
	glUniform4f(color_loc, r, g, b, a);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	glDeleteBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void pico_lobby::render_ui()
{
	glBindFramebuffer(GL_FRAMEBUFFER, ui_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ui_texture, 0);

	glViewport(0, 0, ui_w, ui_h);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);

	// Background: dark gradient (top darker, bottom slightly lighter)
	glClearColor(0.05f, 0.06f, 0.10f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	glUseProgram(program);
	Mat4 ortho = mat4_ortho(0, ui_w, 0, ui_h, -1, 1);
	glUniformMatrix4fv(mvp_uniform, 1, GL_FALSE, ortho.m);

	// Title bar
	ui_fill_rect(pos_attrib, color_uniform, 0, ui_h - 120, ui_w, ui_h, 0.08f, 0.10f, 0.16f, 1.0f);

	// WiVRn logo area (simple colored circle proxy)
	float cx = 120, cy = ui_h - 60, rad = 40;
	for (int i = 0; i < 20; i++)
	{
		float a = i * 18.0f * M_PI / 180.0f;
		float a2 = (i + 1) * 18.0f * M_PI / 180.0f;
		float tri[] = {
			cx, cy, 0,
			cx + cosf(a) * rad, cy + sinf(a) * rad, 0,
			cx + cosf(a2) * rad, cy + sinf(a2) * rad, 0,
		};
		GLuint vbo;
		glGenBuffers(1, &vbo);
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);
		glEnableVertexAttribArray(pos_attrib);
		glVertexAttribPointer(pos_attrib, 3, GL_FLOAT, GL_FALSE, 12, (void *)0);
		glUniform4f(color_uniform, 0.15f, 0.55f, 0.85f, 1.0f);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		glDeleteBuffers(1, &vbo);
	}
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	// Title text area (simple white bar as placeholder for "WiVRn")
	ui_fill_rect(pos_attrib, color_uniform, 200, ui_h - 75, 520, ui_h - 45, 0.9f, 0.9f, 0.95f, 1.0f);

	// Connection status card
	float card_x0 = 80, card_y0 = ui_h - 380, card_x1 = ui_w - 80, card_y1 = ui_h - 160;
	ui_fill_rect(pos_attrib, color_uniform, card_x0, card_y0, card_x1, card_y1, 0.10f, 0.12f, 0.18f, 1.0f);

	// Status indicator dot (green = ready)
	float dot_cx = card_x0 + 50, dot_cy = (card_y0 + card_y1) / 2, dot_r = 20;
	for (int i = 0; i < 20; i++)
	{
		float a = i * 18.0f * M_PI / 180.0f;
		float a2 = (i + 1) * 18.0f * M_PI / 180.0f;
		float tri[] = {
			dot_cx, dot_cy, 0,
			dot_cx + cosf(a) * dot_r, dot_cy + sinf(a) * dot_r, 0,
			dot_cx + cosf(a2) * dot_r, dot_cy + sinf(a2) * dot_r, 0,
		};
		GLuint vbo;
		glGenBuffers(1, &vbo);
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);
		glEnableVertexAttribArray(pos_attrib);
		glVertexAttribPointer(pos_attrib, 3, GL_FLOAT, GL_FALSE, 12, (void *)0);
		glUniform4f(color_uniform, 0.2f, 0.8f, 0.3f, 1.0f);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		glDeleteBuffers(1, &vbo);
	}
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	// Status text placeholder bars
	ui_fill_rect(pos_attrib, color_uniform, card_x0 + 100, dot_cy + 15, card_x0 + 500, dot_cy + 30, 0.7f, 0.75f, 0.8f, 1.0f);
	ui_fill_rect(pos_attrib, color_uniform, card_x0 + 100, dot_cy - 20, card_x0 + 350, dot_cy - 5, 0.4f, 0.45f, 0.5f, 1.0f);

	// Server address card
	float srv_y0 = card_y0 - 180, srv_y1 = card_y0 - 40;
	ui_fill_rect(pos_attrib, color_uniform, card_x0, srv_y0, card_x1, srv_y1, 0.10f, 0.12f, 0.18f, 1.0f);
	ui_fill_rect(pos_attrib, color_uniform, card_x0 + 40, (srv_y0 + srv_y1) / 2 - 10, card_x0 + 300, (srv_y0 + srv_y1) / 2 + 10, 0.5f, 0.55f, 0.6f, 1.0f);

	// Connect button
	float btn_x0 = card_x0 + 350, btn_y0 = (srv_y0 + srv_y1) / 2 - 30, btn_x1 = btn_x0 + 200, btn_y1 = btn_y0 + 60;
	ui_fill_rect(pos_attrib, color_uniform, btn_x0, btn_y0, btn_x1, btn_y1, 0.15f, 0.55f, 0.85f, 1.0f);
	// Button text placeholder
	ui_fill_rect(pos_attrib, color_uniform, btn_x0 + 50, (btn_y0 + btn_y1) / 2 - 8, btn_x0 + 150, (btn_y0 + btn_y1) / 2 + 8, 1.0f, 1.0f, 1.0f, 1.0f);

	// Bottom bar with device info
	ui_fill_rect(pos_attrib, color_uniform, 0, 0, ui_w, 60, 0.06f, 0.07f, 0.10f, 1.0f);
	ui_fill_rect(pos_attrib, color_uniform, 40, 20, 400, 40, 0.3f, 0.35f, 0.4f, 1.0f);

	glUseProgram(0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void pico_lobby::draw_quad(const float head_orient[4], const float head_pos[3],
                           const XrFovf & fov, float ipd, int eye)
{
	Mat4 proj = mat4_perspective(fov.angleLeft, fov.angleRight, fov.angleUp, fov.angleDown, 0.1f, 100.0f);

	neo2::quat hq = neo2::normalize_quat({head_orient[0], head_orient[1], head_orient[2], head_orient[3]});
	float eye_offset = (eye == 0 ? -ipd * 0.5f : ipd * 0.5f);
	float v[3] = {eye_offset, 0, 0};
	float rotated[3];
	neo2::rotate_vector(hq, v, rotated);
	float eye_pos[3] = {head_pos[0] + rotated[0], head_pos[1] + rotated[1], head_pos[2] + rotated[2]};
	Mat4 view = mat4_view(head_orient, eye_pos);
	Mat4 vp = mat4_mul(proj, view);

	// World-locked TV panel: use stored position and yaw
	float tv_w = 1.5f;
	float tv_h = 1.5f;

	// Model: translate to TV position, rotate by yaw around Y, then rotate 180 to face -Z
	Mat4 model = mat4_mul(mat4_translate(tv_pos[0], tv_pos[1], tv_pos[2]),
	                      mat4_mul(mat4_rotate_y(tv_yaw), mat4_rotate_y(M_PI)));

	Mat4 scale = mat4_scale(tv_w * 0.5f, tv_h * 0.5f, 1.0f);
	model = mat4_mul(model, scale);

	Mat4 mvp = mat4_mul(vp, model);

	glUseProgram(tex_program);
	glUniformMatrix4fv(tex_mvp_uniform, 1, GL_FALSE, mvp.m);
	glUniform1i(tex_sampler_uniform, 0);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, ui_texture);

	glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
	glEnableVertexAttribArray(tex_pos_attrib);
	glVertexAttribPointer(tex_pos_attrib, 3, GL_FLOAT, GL_FALSE, 20, (void *)0);
	glEnableVertexAttribArray(tex_uv_attrib);
	glVertexAttribPointer(tex_uv_attrib, 2, GL_FLOAT, GL_FALSE, 20, (void *)12);

	glDrawArrays(GL_TRIANGLES, 0, 6);

	glDisableVertexAttribArray(tex_pos_attrib);
	glDisableVertexAttribArray(tex_uv_attrib);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glUseProgram(0);
}

void pico_lobby::update_interaction(const float head_orient[4], const float head_pos[3],
                                    const controller_sample controllers[2])
{
	// On first placement, position TV in front of user
	if (!tv_placed)
	{
		neo2::quat hq = neo2::normalize_quat({head_orient[0], head_orient[1], head_orient[2], head_orient[3]});
		float fwd[3] = {0, 0, -1};
		float fwd_world[3];
		neo2::rotate_vector(hq, fwd, fwd_world);
		tv_pos[0] = head_pos[0] + fwd_world[0] * 2.0f;
		tv_pos[1] = head_pos[1];
		tv_pos[2] = head_pos[2] + fwd_world[2] * 2.0f;

		// Compute yaw from head forward direction
		tv_yaw = atan2f(fwd_world[0], fwd_world[2]);
		tv_placed = true;
	}

	// Both thumbstick clicks: recenter TV in front of user
	bool both_click = controllers[0].thumbstick_click && controllers[1].thumbstick_click;
	bool prev_both = prev_stick_click[0] && prev_stick_click[1];
	if (both_click && !prev_both)
	{
		neo2::quat hq = neo2::normalize_quat({head_orient[0], head_orient[1], head_orient[2], head_orient[3]});
		float fwd[3] = {0, 0, -1};
		float fwd_world[3];
		neo2::rotate_vector(hq, fwd, fwd_world);
		tv_pos[0] = head_pos[0] + fwd_world[0] * 2.0f;
		tv_pos[1] = head_pos[1];
		tv_pos[2] = head_pos[2] + fwd_world[2] * 2.0f;
		tv_yaw = atan2f(fwd_world[0], fwd_world[2]);
	}

	// Grip to drag: when either controller grip is held, move TV to follow that controller
	for (int h = 0; h < 2; h++)
	{
		if (controllers[h].connected && controllers[h].grip)
		{
			// Controller position in meters
			float cx = controllers[h].position[0] * 0.001f;
			float cy = controllers[h].position[1] * 0.001f;
			float cz = controllers[h].position[2] * 0.001f;

			// Move TV to controller position (offset slightly up so it feels natural)
			tv_pos[0] = cx;
			tv_pos[1] = cy;
			tv_pos[2] = cz;

			// Face the TV toward the head
			float dx = head_pos[0] - cx;
			float dz = head_pos[2] - cz;
			tv_yaw = atan2f(dx, dz);
		}
	}

	// Update previous states
	for (int h = 0; h < 2; h++)
	{
		prev_grip[h] = controllers[h].grip;
		prev_stick_click[h] = controllers[h].thumbstick_click;
	}
}
