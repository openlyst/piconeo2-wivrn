#include "lobby.h"
#include <android/log.h>
#include <cstring>
#include <cmath>
#include <vector>

#define LOG_TAG "WiVRn-OXR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static const char * vert_src = R"(#version 310 es
layout(location = 0) in vec3 a_pos;
uniform mat4 u_mvp;
void main()
{
    gl_Position = u_mvp * vec4(a_pos, 1.0);
}
)";

static const char * frag_src = R"(#version 310 es
precision mediump float;
uniform vec4 u_color;
out vec4 frag_color;
void main()
{
    frag_color = u_color;
}
)";

static const char * tex_vert_src = R"(#version 310 es
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
uniform mat4 u_mvp;
out vec2 v_uv;
void main()
{
    gl_Position = u_mvp * vec4(a_pos, 1.0);
    v_uv = a_uv;
}
)";

static const char * tex_frag_src = R"(#version 310 es
precision mediump float;
in vec2 v_uv;
uniform sampler2D u_tex;
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
		LOGE("Lobby shader compile error: %s", log);
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

static Mat4 mat4_rotate_x(float angle)
{
	float c = cosf(angle), s = sinf(angle);
	Mat4 r = mat4_identity();
	r.m[5] = c;  r.m[6] = -s;
	r.m[9] = s;  r.m[10] = c;
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
	Mat4 inv_rot = mat4_identity();
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			inv_rot.m[i * 4 + j] = rot.m[j * 4 + i];

	Mat4 trans = mat4_translate(-pos[0], -pos[1], -pos[2]);
	return mat4_mul(inv_rot, trans);
}

pico_lobby::~pico_lobby()
{
	if (controller_vao) glDeleteVertexArrays(1, &controller_vao);
	if (ray_vao) glDeleteVertexArrays(1, &ray_vao);
	if (quad_vao) glDeleteVertexArrays(1, &quad_vao);
	if (program) glDeleteProgram(program);
	if (tex_program) glDeleteProgram(tex_program);
	if (controller_vbo) glDeleteBuffers(1, &controller_vbo);
	if (ray_vbo) glDeleteBuffers(1, &ray_vbo);
	if (quad_vbo) glDeleteBuffers(1, &quad_vbo);
	if (ui_texture) glDeleteTextures(1, &ui_texture);
}

static void generate_placeholder_texture(GLuint tex, int w, int h)
{
	std::vector<uint8_t> data(w * h * 4);
	for (int y = 0; y < h; y++)
	{
		for (int x = 0; x < w; x++)
		{
			int idx = (y * w + x) * 4;
			// DEBUG bright red
			data[idx] = 255;
			data[idx + 1] = 0;
			data[idx + 2] = 0;
			data[idx + 3] = 255;
		}
	}
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data.data());
	glBindTexture(GL_TEXTURE_2D, 0);
}

void pico_lobby::init(int w, int h)
{
	eye_width.store(w);
	eye_height.store(h);

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
		LOGE("Lobby program link error: %s", log);
		glDeleteShader(vert);
		glDeleteShader(frag);
		throw std::runtime_error("Failed to link lobby program");
	}
	glDeleteShader(vert);
	glDeleteShader(frag);

	pos_attrib = 0;
	mvp_uniform = glGetUniformLocation(program, "u_mvp");
	color_uniform = glGetUniformLocation(program, "u_color");

	float s = 0.04f;
	float box[] = {
		-s,-s, s,  s,-s, s,  s, s, s,  -s,-s, s,  s, s, s,  -s, s, s,
		-s,-s,-s,  -s, s,-s,  s, s,-s,  -s,-s,-s,  s, s,-s,  s,-s,-s,
		-s, s, s,  s, s, s,  s, s,-s,  -s, s, s,  s, s,-s,  -s, s,-s,
		-s,-s, s,  -s,-s,-s,  s,-s,-s,  -s,-s, s,  s,-s,-s,  s,-s, s,
		 s,-s, s,  s,-s,-s,  s, s,-s,   s,-s, s,  s, s,-s,  s, s, s,
		-s,-s, s,  -s, s, s,  -s, s,-s,  -s,-s, s,  -s, s,-s, -s,-s,-s,
	};
	glGenVertexArrays(1, &controller_vao);
	glGenBuffers(1, &controller_vbo);
	glBindVertexArray(controller_vao);
	glBindBuffer(GL_ARRAY_BUFFER, controller_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(box), box, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 12, (void *)0);
	glBindVertexArray(0);

	float ray[] = {
		0, 0, 0,  0, 0, -1.0f,
	};
	glGenVertexArrays(1, &ray_vao);
	glGenBuffers(1, &ray_vbo);
	glBindVertexArray(ray_vao);
	glBindBuffer(GL_ARRAY_BUFFER, ray_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(ray), ray, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 12, (void *)0);
	glBindVertexArray(0);

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
		LOGE("Lobby tex program link error: %s", log);
	}
	glDeleteShader(tvert);
	glDeleteShader(tfrag);

	tex_pos_attrib = 0;
	tex_uv_attrib = 1;
	tex_mvp_uniform = glGetUniformLocation(tex_program, "u_mvp");
	tex_sampler_uniform = glGetUniformLocation(tex_program, "u_tex");

	glGenTextures(1, &ui_texture);
	glBindTexture(GL_TEXTURE_2D, ui_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);

	float quad[] = {
		-1.0f, -1.0f, 0.0f,  0.0f, 1.0f,
		 1.0f, -1.0f, 0.0f,  1.0f, 1.0f,
		 1.0f,  1.0f, 0.0f,  1.0f, 0.0f,
		-1.0f, -1.0f, 0.0f,  0.0f, 1.0f,
		 1.0f,  1.0f, 0.0f,  1.0f, 0.0f,
		-1.0f,  1.0f, 0.0f,  0.0f, 0.0f,
	};
	glGenVertexArrays(1, &quad_vao);
	glGenBuffers(1, &quad_vbo);
	glBindVertexArray(quad_vao);
	glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20, (void *)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 20, (void *)12);
	glBindVertexArray(0);

	// Fixed world location so the panel does not follow the HMD. App/Menu recenter
	// can move it in front of the user when desired.
	panel_pos[0] = 0.0f;
	panel_pos[1] = 1.6f;
	panel_pos[2] = -2.0f;
	panel_yaw = 0.0f;

	initialized = true;
	LOGI("Lobby initialized (%dx%d) panel=(%.2f,%.2f,%.2f) yaw=%.2f", w, h,
	     panel_pos[0], panel_pos[1], panel_pos[2], panel_yaw);
}

void pico_lobby::draw(int eye, const float head_orient[4], const float head_pos[3],
                      const controller_sample controllers[2],
                      const XrFovf & fov, float ipd, bool head_trigger,
                      bool overlay, bool draw_controllers)
{
	if (!initialized)
	{
		// No lobby yet — let the passthrough background show through.
		return;
	}

	int w = eye_width.load();
	int h = eye_height.load();
	if (w <= 0 || h <= 0)
		return;

	glViewport(0, 0, w, h);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);
	glDisable(GL_CULL_FACE);

	if (overlay)
	{
		glClear(GL_DEPTH_BUFFER_BIT);
	}
	else
	{
		// The background (passthrough camera feed or stream video) is now drawn
		// externally before the lobby UI. Only clear depth here so the panels
		// composite on top of whatever was already rendered.
		glClear(GL_DEPTH_BUFFER_BIT);
	}

	debug_frame_count++;

	if (eye == 0)
	{
		// Billboard toward the head. update_interaction's grab logic
		// also recomputes yaw internally after moving the panel.
		if (!overlay)
		{
			float dx = head_pos[0] - panel_pos[0];
			float dz = head_pos[2] - panel_pos[2];
			float dy = head_pos[1] - panel_pos[1];
			float horiz = sqrtf(dx*dx + dz*dz);
			if (horiz > 1e-5f)
			{
				panel_yaw = atan2f(-dx, dz);
				// Tilt toward head: if panel is above head, tilt top
				// forward; if below, tilt top back. Clamp to +-35deg.
				panel_pitch = atan2f(dy, horiz);
				if (panel_pitch > 0.61f) panel_pitch = 0.61f;
				if (panel_pitch < -0.61f) panel_pitch = -0.61f;
			}
		}
		update_interaction(head_orient, head_pos, controllers, head_trigger);
	}

	glUseProgram(program);

	Mat4 proj = mat4_perspective(fov.angleLeft, fov.angleRight, fov.angleUp, fov.angleDown, 0.1f, 100.0f);

	neo2::quat hq = neo2::normalize_quat({head_orient[0], head_orient[1], head_orient[2], head_orient[3]});

	float eye_offset = (eye == 0 ? -ipd * 0.5f : ipd * 0.5f);
	float v[3] = {eye_offset, 0, 0};
	float rotated[3];
	neo2::rotate_vector(hq, v, rotated);

	float eye_pos[3] = {head_pos[0] + rotated[0], head_pos[1] + rotated[1], head_pos[2] + rotated[2]};
	Mat4 view = mat4_view(head_orient, eye_pos);

	Mat4 vp = mat4_mul(proj, view);

	if (draw_controllers)
	{
		glDepthMask(GL_TRUE);
		static int ctrl_log_count = 0;
		bool any_ctrl = false;
		for (int h = 0; h < 2; h++)
		{
			if (!controllers[h].connected)
				continue;
			any_ctrl = true;

			float pos_m[3] = {
				controllers[h].position[0] * 0.001f,
				controllers[h].position[1] * 0.001f,
				controllers[h].position[2] * 0.001f,
			};

			float corrected_orient[4] = {
				-controllers[h].orientation[0],
				-controllers[h].orientation[1],
				controllers[h].orientation[2],
				controllers[h].orientation[3],
			};
			Mat4 model = mat4_mul(mat4_translate(pos_m[0], pos_m[1], pos_m[2]), quat_to_mat4(corrected_orient));
			Mat4 mvp = mat4_mul(vp, model);

			if (h == 0)
				glUniform4f(color_uniform, 0.2f, 0.4f, 1.0f, 1.0f);
			else
				glUniform4f(color_uniform, 1.0f, 0.3f, 0.2f, 1.0f);

			glBindVertexArray(controller_vao);
			glUniformMatrix4fv(mvp_uniform, 1, GL_FALSE, mvp.m);
			glDrawArrays(GL_TRIANGLES, 0, 36);

			float ray_len = 2.0f;
			Mat4 ray_scale = mat4_scale(1.0f, 1.0f, ray_len);
			Mat4 ray_model = mat4_mul(model, ray_scale);
			Mat4 ray_mvp = mat4_mul(vp, ray_model);
			glUniformMatrix4fv(mvp_uniform, 1, GL_FALSE, ray_mvp.m);
			if (h == 0)
				glUniform4f(color_uniform, 0.2f, 0.4f, 1.0f, 0.4f);
			else
				glUniform4f(color_uniform, 1.0f, 0.3f, 0.2f, 0.4f);
			glBindVertexArray(ray_vao);
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			glLineWidth(5.0f);
			glDrawArrays(GL_LINES, 0, 2);
			glDisable(GL_BLEND);
		}

		if (eye == 0 && (ctrl_log_count++ % 300 == 0))
			LOGI("LOBBY controllers: conn=%d/%d pos0=(%.1f,%.1f,%.1f) pos1=(%.1f,%.1f,%.1f)",
			     (int)controllers[0].connected, (int)controllers[1].connected,
			     controllers[0].position[0]*0.001f, controllers[0].position[1]*0.001f, controllers[0].position[2]*0.001f,
			     controllers[1].position[0]*0.001f, controllers[1].position[1]*0.001f, controllers[1].position[2]*0.001f);

		glBindVertexArray(0);
		glUseProgram(0);
	}

	draw_quad(head_orient, head_pos, fov, ipd, eye);
}

GLuint pico_lobby::get_external_texture()
{
	return ui_texture;
}

void pico_lobby::set_surface_texture(jobject st, jmethodID update_method)
{
	surface_texture = st;
	update_tex_image_method = update_method;
}

void pico_lobby::on_frame_available()
{
	frame_available.store(true, std::memory_order_relaxed);
}

void pico_lobby::update_tex_image(JNIEnv* env)
{
	if (!frame_available.load(std::memory_order_relaxed) || !surface_texture || !update_tex_image_method)
		return;

	env->CallVoidMethod(surface_texture, update_tex_image_method);
	frame_available.store(false, std::memory_order_relaxed);

	static int update_count = 0;
	if (++update_count % 100 == 0)
		LOGI("update_tex_image called #%d", update_count);
}

void pico_lobby::update_texture(int width, int height, const void * pixels)
{
	std::lock_guard<std::mutex> lock(tex_mutex);
	pending_tex_w = width;
	pending_tex_h = height;
	pending_tex_data.assign((const uint8_t *)pixels, (const uint8_t *)pixels + width * height * 4);
	tex_pending = true;
}

void pico_lobby::update_texture_argb(int width, int height, const uint32_t * pixels)
{
	std::lock_guard<std::mutex> lock(tex_mutex);
	pending_tex_w = width;
	pending_tex_h = height;
	size_t n = (size_t)width * height;
	pending_tex_data.resize(n * 4);
	const uint32_t *src = pixels;
	uint8_t *dst = pending_tex_data.data();
	for (size_t i = 0; i < n; i++) {
		uint32_t px = src[i];
		dst[i * 4]     = (uint8_t)((px >> 16) & 0xFF);
		dst[i * 4 + 1] = (uint8_t)((px >> 8)  & 0xFF);
		dst[i * 4 + 2] = (uint8_t)(px         & 0xFF);
		dst[i * 4 + 3] = (uint8_t)((px >> 24) & 0xFF);
	}
	tex_pending = true;
}

void pico_lobby::flush_pending_texture()
{
	std::lock_guard<std::mutex> lock(tex_mutex);
	if (!tex_pending)
		return;

	glBindTexture(GL_TEXTURE_2D, ui_texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pending_tex_w, pending_tex_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pending_tex_data.data());
	glBindTexture(GL_TEXTURE_2D, 0);

	tex_pending = false;
	pending_tex_data.clear();
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
	                      mat4_mul(mat4_rotate_y(panel_yaw),
	                      mat4_rotate_x(panel_pitch)));

	Mat4 scale = mat4_scale(panel_w * 0.5f, panel_h * 0.5f, 1.0f);
	model = mat4_mul(model, scale);

	Mat4 mvp = mat4_mul(vp, model);

	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);
	glDepthMask(GL_TRUE);

	glUseProgram(tex_program);
	glUniformMatrix4fv(tex_mvp_uniform, 1, GL_FALSE, mvp.m);
	glUniform1i(tex_sampler_uniform, 0);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, ui_texture);

	glBindVertexArray(quad_vao);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	glBindVertexArray(0);

	glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
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

void pico_lobby::set_resolution(int w, int h)
{
	eye_width.store(w);
	eye_height.store(h);
}

void pico_lobby::recenter(const float head_pos[3], float head_yaw)
{
	constexpr float kPanelDist = 1.5f;
	float cy = std::cosf(head_yaw), sy = std::sinf(head_yaw);
	panel_pos[0] = (head_pos ? head_pos[0] : 0.0f) - sy * kPanelDist;
	panel_pos[1] = head_pos ? head_pos[1] : 0.0f;
	panel_pos[2] = (head_pos ? head_pos[2] : 0.0f) - cy * kPanelDist;
	panel_yaw = head_yaw;
	LOGI("Lobby recentered, panel pos=(%.2f,%.2f,%.2f) yaw=%.2f", panel_pos[0], panel_pos[1], panel_pos[2], panel_yaw);
}

void pico_lobby::recenter_facing(const float head_pos[3], float fwd_x, float fwd_z)
{
	constexpr float kPanelDist = 1.5f;
	float n = sqrtf(fwd_x * fwd_x + fwd_z * fwd_z);
	if (n < 1e-5f) { fwd_x = 0; fwd_z = -1; n = 1; }
	fwd_x /= n; fwd_z /= n;
	panel_pos[0] = head_pos[0] + fwd_x * kPanelDist;
	panel_pos[1] = head_pos[1];
	panel_pos[2] = head_pos[2] + fwd_z * kPanelDist;
	panel_yaw = atan2f(fwd_x, -fwd_z);
	LOGI("Lobby recentered (facing), panel pos=(%.2f,%.2f,%.2f) yaw=%.2f fwd=(%.2f,%.2f)",
	     panel_pos[0], panel_pos[1], panel_pos[2], panel_yaw, fwd_x, fwd_z);
}

void pico_lobby::update_interaction(const float head_orient[4], const float head_pos[3],
                                    const controller_sample controllers[2], bool head_trigger)
{
	debug_laser_hit = false;

	float cy = cosf(panel_yaw), sy = sinf(panel_yaw);
	float normal[3] = {-sy, 0, cy};
	float u_axis[3] = {cy, 0, sy};
	float v_axis[3] = {0, 1, 0};
	float half_w = panel_w * 0.5f;
	float half_h = panel_h * 0.5f;

	bool any_ctrl = controllers[0].connected || controllers[1].connected;

	// --- Grip-to-grab logic ---
	// When grip is pressed while the ray hits the panel, capture the grab
	// point in panel-local UV. Each frame while held, intersect the
	// controller ray with the panel plane and reposition the panel so the
	// grab UV stays under the ray. The offset is in panel-local space so
	// it stays correct even as billboard rotates the yaw to face the head.

	// First pass: compute ray hits for both hands
	float hit_points[2][3] = {{0}};
	bool  ray_hits[2] = {false, false};
	float hit_uv[2][2] = {{0}};

	for (int h = 0; h < 2; h++)
	{
		if (!controllers[h].connected)
			continue;

		float origin[3] = {
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
		float dir[3] = {0, 0, -1};
		float ray_dir[3];
		neo2::rotate_vector(cq, dir, ray_dir);

		float u, v;
		bool hit = ray_plane_intersect(origin, ray_dir, panel_pos, normal, u_axis, v_axis, half_w, half_h, u, v);
		ray_hits[h] = hit;
		hit_uv[h][0] = u;
		hit_uv[h][1] = v;

		if (hit)
		{
			float denom = ray_dir[0]*normal[0] + ray_dir[1]*normal[1] + ray_dir[2]*normal[2];
			float to_center[3] = {panel_pos[0]-origin[0], panel_pos[1]-origin[1], panel_pos[2]-origin[2]};
			float t_val = (denom != 0) ? (to_center[0]*normal[0] + to_center[1]*normal[1] + to_center[2]*normal[2]) / denom : 0;
			hit_points[h][0] = origin[0] + ray_dir[0] * t_val;
			hit_points[h][1] = origin[1] + ray_dir[1] * t_val;
			hit_points[h][2] = origin[2] + ray_dir[2] * t_val;
		}
	}

	// Start grab on grip rising edge if the ray hits the panel
	for (int h = 0; h < 2; h++)
	{
		bool grip_now = controllers[h].connected && controllers[h].grip;
		bool grip_down = grip_now && !prev_grip[h];

		if (grip_down && ray_hits[h] && !grabbing)
		{
			grabbing = true;
			grab_hand = h;
			grab_u = hit_uv[h][0];
			grab_v = hit_uv[h][1];
			// Distance from controller to the grab point
			float dx = hit_points[h][0] - controllers[h].position[0] * 0.001f;
			float dy = hit_points[h][1] - controllers[h].position[1] * 0.001f;
			float dz = hit_points[h][2] - controllers[h].position[2] * 0.001f;
			grab_dist = sqrtf(dx*dx + dy*dy + dz*dz);
			LOGI("GRAB_START h=%d uv=(%.2f,%.2f) dist=%.2f", h, grab_u, grab_v, grab_dist);
		}
		prev_grip[h] = grip_now;
	}

	// While grabbing, move panel so the grab UV stays under the controller ray
	if (grabbing)
	{
		int h = grab_hand;
		bool grip_held = controllers[h].connected && controllers[h].grip;

		if (!grip_held)
		{
			grabbing = false;
			grab_hand = -1;
			LOGI("GRAB_END panel=(%.2f,%.2f,%.2f) yaw=%.2f", panel_pos[0], panel_pos[1], panel_pos[2], panel_yaw);
		}
		else
		{
			float origin[3] = {
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
			float dir[3] = {0, 0, -1};
			float ray_dir[3];
			neo2::rotate_vector(cq, dir, ray_dir);

			// Project a point at grab_dist along the controller ray.
			float hit[3] = {
				origin[0] + ray_dir[0] * grab_dist,
				origin[1] + ray_dir[1] * grab_dist,
				origin[2] + ray_dir[2] * grab_dist,
			};

			// Temporarily set panel_pos to the hit point so we can
			// compute the billboard yaw from the new position, then
			// use that yaw to compute the panel-local offset.
			float old_pos[3] = {panel_pos[0], panel_pos[1], panel_pos[2]};
			panel_pos[0] = hit[0];
			panel_pos[1] = hit[1];
			panel_pos[2] = hit[2];

			// Compute new yaw and pitch facing the head from this position
			float dx = head_pos[0] - panel_pos[0];
			float dz = head_pos[2] - panel_pos[2];
			float dy = head_pos[1] - panel_pos[1];
			float horiz = sqrtf(dx*dx + dz*dz);
			float new_yaw = panel_yaw;
			float new_pitch = 0;
			if (horiz > 1e-5f)
			{
				new_yaw = atan2f(-dx, dz);
				new_pitch = atan2f(dy, horiz);
				if (new_pitch > 0.61f) new_pitch = 0.61f;
				if (new_pitch < -0.61f) new_pitch = -0.61f;
			}
			panel_yaw = new_yaw;
			panel_pitch = new_pitch;

			// Recompute panel axes with the new yaw
			float ncy = cosf(new_yaw), nsy = sinf(new_yaw);
			float nu_axis[3] = {ncy, 0, nsy};
			float nv_axis[3] = {0, 1, 0};

			// Now offset from the hit point by the grab UV in the
			// new panel-local space to get the final panel center.
			float off_x = grab_u * half_w * nu_axis[0] + grab_v * half_h * nv_axis[0];
			float off_y = grab_u * half_w * nu_axis[1] + grab_v * half_h * nv_axis[1];
			float off_z = grab_u * half_w * nu_axis[2] + grab_v * half_h * nv_axis[2];
			panel_pos[0] = hit[0] - off_x;
			panel_pos[1] = hit[1] - off_y;
			panel_pos[2] = hit[2] - off_z;
		}
	}

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

		// Suppress touch while this hand is grabbing
		if (grabbing && grab_hand == h)
		{
			lobby_touch_x[h] = -1;
			lobby_touch_down[h] = false;
			lobby_touch_pressed[h] = false;
			prev_trigger[h] = false;
			continue;
		}

		float origin[3] = {
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
		float dir[3] = {0, 0, -1};
		float ray_dir[3];
		neo2::rotate_vector(cq, dir, ray_dir);

		float u, v;
		bool hit = ray_plane_intersect(origin, ray_dir, panel_pos, normal, u_axis, v_axis, half_w, half_h, u, v);

		static int interact_log_count = 0;
		bool log_this = (interact_log_count++ % 30 == 0) || hit;
		if (log_this)
		{
			float denom = ray_dir[0]*normal[0] + ray_dir[1]*normal[1] + ray_dir[2]*normal[2];
			float to_center[3] = {panel_pos[0]-origin[0], panel_pos[1]-origin[1], panel_pos[2]-origin[2]};
			float t_val = (denom != 0) ? (to_center[0]*normal[0] + to_center[1]*normal[1] + to_center[2]*normal[2]) / denom : -1;
			float hitpt[3] = {origin[0]+ray_dir[0]*t_val, origin[1]+ray_dir[1]*t_val, origin[2]+ray_dir[2]*t_val};
			LOGI("INTERACT h=%d conn=%d origin=(%.2f,%.2f,%.2f) dir=(%.2f,%.2f,%.2f) t=%.2f hitpt=(%.2f,%.2f,%.2f) panel=(%.2f,%.2f,%.2f) half_w=%.2f half_h=%.2f hit=%d u=%.3f v=%.3f trigger=%d",
			     h, (int)controllers[h].connected, origin[0], origin[1], origin[2], ray_dir[0], ray_dir[1], ray_dir[2],
			     t_val, hitpt[0], hitpt[1], hitpt[2],
			     panel_pos[0], panel_pos[1], panel_pos[2], half_w, half_h, (int)hit, u, v, (int)controllers[h].trigger);
		}

		if (hit)
		{
			last_hit[h].valid = true;
			last_hit[h].u = u;
			last_hit[h].v = v;
			debug_laser_hit = true;
			debug_hit_hand = h;
			debug_hit_u = u;
			debug_hit_v = v;
			debug_trigger_down = controllers[h].trigger > 128;
		}

		bool trigger_pressed = controllers[h].trigger > 128;
		bool trigger_down = trigger_pressed && !prev_trigger[h];

		lobby_thumbstick_y[h] = (controllers[h].touch[1] - 128) / 128.0f;

		if (last_hit[h].valid)
		{
			float px = ((u + 1.0f) * 0.5f) * 1400.0f;
			float py = (1.0f - (v + 1.0f) * 0.5f) * 900.0f;

			lobby_touch_x[h] = px;
			lobby_touch_y[h] = py;
			lobby_touch_down[h] = trigger_pressed;
			lobby_touch_pressed[h] = trigger_down;

			debug_touch_x = px;
			debug_touch_y = py;

			if (trigger_down)
				LOGI("TRIGGER_DOWN h=%d px=%.1f py=%.1f u=%.3f v=%.3f", h, px, py, u, v);
		}
		else
		{
			lobby_touch_x[h] = -1;
			lobby_touch_y[h] = -1;
			lobby_touch_down[h] = false;
			lobby_touch_pressed[h] = false;
		}

		bool touch_changed =
			lobby_touch_x[h] != prev_lobby_touch_x[h] ||
			lobby_touch_y[h] != prev_lobby_touch_y[h] ||
			lobby_touch_down[h] != prev_lobby_touch_down[h] ||
			lobby_touch_pressed[h] != prev_lobby_touch_pressed[h];
		if (touch_changed) {
			push_lobby_touch_to_java(h, lobby_touch_x[h], lobby_touch_y[h],
			                         lobby_touch_down[h], lobby_touch_pressed[h],
			                         lobby_thumbstick_y[h]);
			prev_lobby_touch_x[h] = lobby_touch_x[h];
			prev_lobby_touch_y[h] = lobby_touch_y[h];
			prev_lobby_touch_down[h] = lobby_touch_down[h];
			prev_lobby_touch_pressed[h] = lobby_touch_pressed[h];
		}
		prev_trigger[h] = trigger_pressed;
		}

	// No controllers — use head gaze laser with OK button as trigger
	head_hit.valid = false;
	head_touch_x = -1;
	head_touch_y = -1;
	head_touch_down = false;
	head_touch_pressed = false;

	if (!any_ctrl)
	{
		neo2::quat hq = neo2::normalize_quat({head_orient[0], head_orient[1], head_orient[2], head_orient[3]});
		float dir[3] = {0, 0, -1};
		float ray_dir[3];
		neo2::rotate_vector(hq, dir, ray_dir);

		float u, v;
		bool hit = ray_plane_intersect(head_pos, ray_dir, panel_pos, normal, u_axis, v_axis, half_w, half_h, u, v);

		if (hit)
		{
			head_hit.valid = true;
			head_hit.u = u;
			head_hit.v = v;
			debug_laser_hit = true;
			debug_hit_hand = -1;
			debug_hit_u = u;
			debug_hit_v = v;
			debug_trigger_down = head_trigger;

			float px = ((u + 1.0f) * 0.5f) * 1400.0f;
			float py = (1.0f - (v + 1.0f) * 0.5f) * 900.0f;

			head_touch_x = px;
			head_touch_y = py;
			head_touch_down = head_trigger;
			head_touch_pressed = head_trigger && !prev_head_trigger;

			debug_touch_x = px;
			debug_touch_y = py;

			if (head_touch_pressed)
				LOGI("HEAD_TRIGGER px=%.1f py=%.1f u=%.3f v=%.3f", px, py, u, v);
		}

		prev_head_trigger = head_trigger;

		bool head_touch_changed =
			head_touch_x != prev_head_touch_x ||
			head_touch_y != prev_head_touch_y ||
			head_touch_down != prev_head_touch_down ||
			head_touch_pressed != prev_head_touch_pressed;
		if (head_touch_changed) {
			push_lobby_touch_to_java(-1, head_touch_x, head_touch_y,
			                         head_touch_down, head_touch_pressed, 0.0f);
			prev_head_touch_x = head_touch_x;
			prev_head_touch_y = head_touch_y;
			prev_head_touch_down = head_touch_down;
			prev_head_touch_pressed = head_touch_pressed;
		}
	}
	else
	{
		prev_head_trigger = false;
		// Controllers took over — clear any stale head cursor on the Java side.
		if (prev_head_touch_x >= -1 || prev_head_touch_down || prev_head_touch_pressed) {
			push_lobby_touch_to_java(-1, -1, -1, false, false, 0.0f);
			prev_head_touch_x = -2;
			prev_head_touch_y = -2;
			prev_head_touch_down = false;
			prev_head_touch_pressed = false;
		}
	}

	if (!debug_laser_hit)
	{
		debug_hit_hand = -1;
		debug_touch_x = -1;
		debug_touch_y = -1;
		debug_trigger_down = false;
	}

	if (debug_frame_count % 60 == 0)
	{
		LOGI("LOBBY_DEBUG frame=%d laser_hit=%d hit_hand=%d u=%.3f v=%.3f touch=(%.1f,%.1f) trigger=%d",
		     debug_frame_count, (int)debug_laser_hit, debug_hit_hand,
		     debug_hit_u, debug_hit_v, debug_touch_x, debug_touch_y, (int)debug_trigger_down);
	}
}
