#pragma once
// Passthrough camera background for the lobby. Uses the native Pico SDK
// (libPvr_UnitySDK.so) see-through camera API instead of Android Camera2 NDK.
// The SDK manages the camera frame loop; we pull per-eye raw frames via
// Pvr_BoundaryGetSeeThroughData and render them through a fisheye undistortion
// mesh from the Pico system's seethroughsetting app (grid_point_coord.txt).
#include "pico_sdk.h"
#include <GLES3/gl3.h>

class pico_passthrough
{
	GLuint program = 0;
	GLint  sampler_loc = -1;

	// Per-eye mesh: VAO + VBO (positions+UVs) + shared IBO
	GLuint eye_vao[2] = {0, 0};
	GLuint eye_vbo[2] = {0, 0};
	GLuint ibo = 0;
	int    index_count = 0;

	// One texture per eye: the SDK delivers separate left/right camera frames.
	GLuint eye_tex[2] = {0, 0};
	int    eye_tex_w[2] = {0, 0};
	int    eye_tex_h[2] = {0, 0};

	bool gl_ready = false;
	bool camera_on = false;

	// Cached frame dimensions from the SDK so we can reallocate textures
	// when the resolution changes.
	int frame_w = 0;
	int frame_h = 0;

	void build_shaders();
	void build_mesh();
	void upload_eye(int eye, int w, int h, const uint8_t *data);

public:
	pico_passthrough() = default;
	~pico_passthrough();

	void init();
	void start();
	void stop();
	void draw(int eye);

	bool is_camera_on() const { return camera_on; }
};
