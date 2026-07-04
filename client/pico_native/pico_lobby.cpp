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

	// Texture shader program
	GLuint tvert = compile_shader(GL_VERTEX_SHADER, tex_vert_src);
	GLuint tfrag = compile_shader(GL_FRAGMENT_SHADER, tex_frag_src);
	tex_program = glCreateProgram();
	glAttachShader(tex_program, tvert);
	glAttachShader(tex_program, tfrag);
	glLinkProgram(tex_program);
	GLint tstatus = GL_FALSE;
	glGetProgramiv(tex_program, GL_LINK_STATUS, &tstatus);
	if (tstatus != GL_TRUE)
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

	// UI texture (will be updated from Java Bitmap)
	glGenTextures(1, &ui_texture);
	glBindTexture(GL_TEXTURE_2D, ui_texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1400, 900, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);

	// Quad VBO for UI panel (position xyz + uv)
	// V is flipped because Android Bitmap data starts top-left but OpenGL V=0 is bottom
	float quad[] = {
		-1.0f, -1.0f, 0.0f,  0.0f, 1.0f,
		 1.0f, -1.0f, 0.0f,  1.0f, 1.0f,
		 1.0f,  1.0f, 0.0f,  1.0f, 0.0f,
		-1.0f, -1.0f, 0.0f,  0.0f, 1.0f,
		 1.0f,  1.0f, 0.0f,  1.0f, 0.0f,
		-1.0f,  1.0f, 0.0f,  0.0f, 0.0f,
	};
	glGenBuffers(1, &quad_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
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
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);
	glDisable(GL_CULL_FACE);

	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

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

	// Draw floor grid (no depth write so it doesn't occlude anything)
	glDepthMask(GL_FALSE);
	glUniform4f(color_uniform, 0.5f, 0.5f, 0.8f, 1.0f);
	glBindBuffer(GL_ARRAY_BUFFER, grid_vbo);
	glEnableVertexAttribArray(pos_attrib);
	glVertexAttribPointer(pos_attrib, 3, GL_FLOAT, GL_FALSE, 12, (void *)0);
	int grid_vert_count = 0;
	glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &grid_vert_count);
	grid_vert_count /= (3 * sizeof(float));
	glUniformMatrix4fv(mvp_uniform, 1, GL_FALSE, vp.m);
	glDrawArrays(GL_LINES, 0, grid_vert_count);

	// Draw controllers as small boxes (with depth write so they occlude the panel)
	glDepthMask(GL_TRUE);
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

	draw_quad(head_orient, head_pos, fov, ipd, eye);
}

void pico_lobby::update_texture(int width, int height, const void * pixels)
{
	if (!ui_texture || !pixels)
		return;

	glBindTexture(GL_TEXTURE_2D, ui_texture);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	glBindTexture(GL_TEXTURE_2D, 0);
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

	Mat4 model = mat4_mul(mat4_translate(panel_pos[0], panel_pos[1], panel_pos[2]),
	                      mat4_mul(mat4_rotate_y(panel_yaw), mat4_rotate_y(M_PI)));

	Mat4 scale = mat4_scale(panel_w * 0.5f, panel_h * 0.5f, 1.0f);
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

static bool ray_plane_intersect(
    const float ray_origin[3], const float ray_dir[3],
    const float plane_center[3], const float plane_normal[3],
    const float plane_u[3], const float plane_v[3],
    float plane_half_w, float plane_half_h,
    float & out_u, float & out_v)
{
	float denom = ray_dir[0]*plane_normal[0] + ray_dir[1]*plane_normal[1] + ray_dir[2]*plane_normal[2];
	if (fabsf(denom) < 1e-6f)
		return false;

	float to_center[3] = {
		plane_center[0] - ray_origin[0],
		plane_center[1] - ray_origin[1],
		plane_center[2] - ray_origin[2],
	};
	float t = (to_center[0]*plane_normal[0] + to_center[1]*plane_normal[1] + to_center[2]*plane_normal[2]) / denom;
	if (t < 0)
		return false;

	float hit[3] = {
		ray_origin[0] + ray_dir[0] * t,
		ray_origin[1] + ray_dir[1] * t,
		ray_origin[2] + ray_dir[2] * t,
	};

	float local[3] = {
		hit[0] - plane_center[0],
		hit[1] - plane_center[1],
		hit[2] - plane_center[2],
	};

	float du = local[0]*plane_u[0] + local[1]*plane_u[1] + local[2]*plane_u[2];
	float dv = local[0]*plane_v[0] + local[1]*plane_v[1] + local[2]*plane_v[2];

	out_u = du / plane_half_w;
	out_v = dv / plane_half_h;

	return (out_u >= -1.0f && out_u <= 1.0f && out_v >= -1.0f && out_v <= 1.0f);
}

void pico_lobby::update_interaction(const float head_orient[4], const float head_pos[3],
                                    const controller_sample controllers[2])
{
	if (!panel_placed)
	{
		neo2::quat hq = neo2::normalize_quat({head_orient[0], head_orient[1], head_orient[2], head_orient[3]});
		float fwd[3] = {0, 0, -1};
		float fwd_world[3];
		neo2::rotate_vector(hq, fwd, fwd_world);
		panel_pos[0] = head_pos[0] + fwd_world[0] * 2.5f;
		panel_pos[1] = head_pos[1];
		panel_pos[2] = head_pos[2] + fwd_world[2] * 2.5f;
		panel_yaw = atan2f(fwd_world[0], fwd_world[2]);
		panel_placed = true;
	}

	// Grip-to-follow: like WiVRn's update_gui_position
	if (recentering_controller >= 0)
	{
		int h = recentering_controller;
		if (controllers[h].connected && controllers[h].grip)
		{
			float cpos[3] = {
				controllers[h].position[0] * 0.001f,
				controllers[h].position[1] * 0.001f,
				controllers[h].position[2] * 0.001f,
			};
			neo2::quat cq = neo2::normalize_quat({
				-controllers[h].orientation[0],
				-controllers[h].orientation[1],
				controllers[h].orientation[2],
				controllers[h].orientation[3],
			});

			// Position offset uses full controller orientation
			float rotated_offset[3];
			neo2::rotate_vector(cq, recenter_offset_pos, rotated_offset);
			panel_pos[0] = cpos[0] + rotated_offset[0];
			panel_pos[1] = cpos[1] + rotated_offset[1];
			panel_pos[2] = cpos[2] + rotated_offset[2];

			// Yaw: extract controller yaw and apply stored yaw offset
			float fwd[3] = {0, 0, -1};
			float fwd_world[3];
			neo2::rotate_vector(cq, fwd, fwd_world);
			float controller_yaw = atan2f(fwd_world[0], fwd_world[2]);
			panel_yaw = controller_yaw + recenter_offset_yaw;
		}
		else
		{
			recentering_controller = -1;
		}
	}
	else
	{
		for (int h = 0; h < 2; h++)
		{
			if (controllers[h].connected && controllers[h].grip && !prev_grip[h])
			{
				float cpos[3] = {
					controllers[h].position[0] * 0.001f,
					controllers[h].position[1] * 0.001f,
					controllers[h].position[2] * 0.001f,
				};
				neo2::quat cq = neo2::normalize_quat({
					-controllers[h].orientation[0],
					-controllers[h].orientation[1],
					controllers[h].orientation[2],
					controllers[h].orientation[3],
				});

				// Ray-cast from controller to panel (like WiVRn)
				float fwd[3] = {0, 0, -1};
				float fwd_world[3];
				neo2::rotate_vector(cq, fwd, fwd_world);

				float cy = cosf(panel_yaw), sy = sinf(panel_yaw);
				float normal[3] = {sy, 0, cy};

				float to_panel[3] = {
					panel_pos[0] - cpos[0],
					panel_pos[1] - cpos[1],
					panel_pos[2] - cpos[2],
				};
				float denom = normal[0] * fwd_world[0] + normal[1] * fwd_world[1] + normal[2] * fwd_world[2];

				bool hit = false;
				if (fabsf(denom) > 1e-6f)
				{
					float lambda = (to_panel[0] * normal[0] + to_panel[1] * normal[1] + to_panel[2] * normal[2]) / denom;
					if (lambda > 0)
					{
						float hit_pt[3] = {
							cpos[0] + lambda * fwd_world[0],
							cpos[1] + lambda * fwd_world[1],
							cpos[2] + lambda * fwd_world[2],
						};
						// Check if hit point is within panel bounds
						float local[3] = {
							hit_pt[0] - panel_pos[0],
							hit_pt[1] - panel_pos[1],
							hit_pt[2] - panel_pos[2],
						};
						float u_axis[3] = {-cy, 0, sy};
						float u = local[0] * u_axis[0] + local[1] * u_axis[1] + local[2] * u_axis[2];
						float v = local[1];
						if (fabsf(u) <= panel_w * 0.5f && fabsf(v) <= panel_h * 0.5f)
							hit = true;
					}
				}

				if (hit)
				{
					// Store panel position relative to controller in controller's local frame
					float delta[3] = {
						panel_pos[0] - cpos[0],
						panel_pos[1] - cpos[1],
						panel_pos[2] - cpos[2],
					};
					neo2::quat inv_cq = {cq.x, cq.y, cq.z, -cq.w};
					neo2::rotate_vector(inv_cq, delta, recenter_offset_pos);

					// Store yaw offset only (panel stays upright)
					float fwd2[3] = {0, 0, -1};
					float fwd_world2[3];
					neo2::rotate_vector(cq, fwd2, fwd_world2);
					float controller_yaw = atan2f(fwd_world2[0], fwd_world2[2]);
					recenter_offset_yaw = panel_yaw - controller_yaw;
				}
				else
				{
					// Ray doesn't intersect panel: teleport to 1m in front of controller
					recenter_offset_pos[0] = 0;
					recenter_offset_pos[1] = 0;
					recenter_offset_pos[2] = -1;
					recenter_offset_yaw = 0;
				}

				recentering_controller = h;
				break;
			}
		}
	}

	// Compute panel coordinate frame
	float cy = cosf(panel_yaw), sy = sinf(panel_yaw);
	// Panel faces -Z after yaw rotation + 180 flip, so normal is +Z rotated by yaw
	float normal[3] = {sy, 0, cy};
	// After the 180 deg Y rotation, U axis is -X rotated by yaw, V is +Y
	float u_axis[3] = {-cy, 0, sy};
	float v_axis[3] = {0, 1, 0};
	float half_w = panel_w * 0.5f;
	float half_h = panel_h * 0.5f;

	for (int h = 0; h < 2; h++)
	{
		last_hit[h].valid = false;

		if (!controllers[h].connected)
		{
			lobby_touch_x[h] = -1;
			lobby_touch_down[h] = false;
			lobby_touch_pressed[h] = false;
			continue;
		}

		// Controller position in meters
		float origin[3] = {
			controllers[h].position[0] * 0.001f,
			controllers[h].position[1] * 0.001f,
			controllers[h].position[2] * 0.001f,
		};

		// Controller forward direction (from orientation quaternion)
		// Pico SDK uses a different coordinate convention; negate x and y (like ALVR)
		neo2::quat cq = neo2::normalize_quat({
			-controllers[h].orientation[0],
			-controllers[h].orientation[1],
			controllers[h].orientation[2],
			controllers[h].orientation[3],
		});
		float dir[3] = {0, 0, -1};
		float ray_dir[3];
		neo2::rotate_vector(cq, dir, ray_dir);

		float u, v;
		if (ray_plane_intersect(origin, ray_dir, panel_pos, normal, u_axis, v_axis, half_w, half_h, u, v))
		{
			last_hit[h].valid = true;
			last_hit[h].u = u;
			last_hit[h].v = v;
		}

		bool trigger_pressed = controllers[h].trigger > 128;
		bool trigger_down = trigger_pressed && !prev_trigger[h];

		lobby_thumbstick_y[h] = (controllers[h].touch[1] - 128) / 128.0f;

		if (last_hit[h].valid)
		{
			float px = (u + 1.0f) * 0.5f * 1400.0f;
			float py = (1.0f - (v + 1.0f) * 0.5f) * 900.0f;

			lobby_touch_x[h] = px;
			lobby_touch_y[h] = py;
			lobby_touch_down[h] = trigger_pressed;
			lobby_touch_pressed[h] = trigger_down;
		}
		else
		{
			lobby_touch_x[h] = -1;
			lobby_touch_down[h] = false;
			lobby_touch_pressed[h] = false;
		}

		prev_trigger[h] = trigger_pressed;
	}

	for (int h = 0; h < 2; h++)
	{
		prev_grip[h] = controllers[h].grip;
	}
}
