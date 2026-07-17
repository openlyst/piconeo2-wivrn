#pragma once
// Passthrough camera background for the lobby. Replaces the old dark-void +
// grid-floor environment with a live feed from the Neo 2 tracking cameras,
// so the user sees their surroundings while interacting with the lobby UI
// panels (pairing, settings, EQ). The panels themselves are still drawn by
// pico_lobby on top of this background.
#include <GLES3/gl3.h>

class pico_passthrough
{
	GLuint program = 0;
	GLuint vao = 0, vbo = 0;
	GLint mvp_loc = -1;
	GLint sampler_loc = -1;

	GLuint eye_tex[2] = {0, 0};
	int   eye_w[2] = {0, 0};
	int   eye_h[2] = {0, 0};

	bool gl_ready = false;
	bool camera_on = false;

	// Camera frame resolution we request from the SDK. The Neo 2 tracking
	// cameras deliver 1280x800 natively; asking for that avoids an internal
	// scale pass.
	static constexpr int kCamW = 1280;
	static constexpr int kCamH = 800;

	void build_shaders();
	void build_geometry();
	void upload_frame(int eye, unsigned char *data, int w, int h);

public:
	pico_passthrough() = default;
	~pico_passthrough();

	// One-time GL setup (shaders + fullscreen quad + textures). Call from the
	// render thread after the GL context is current.
	void init();

	// Start / stop the camera frame loop. start() calls PVR_StartCameraPreview +
	// Pvr_BoundarySetSeeThroughVisible; stop() hides the see-through layer.
	void start();
	void stop();

	// Draw the passthrough background for one eye. Captures the latest camera
	// frame, uploads it, and draws a fullscreen clip-space quad. Call BEFORE
	// drawing the lobby UI panels so they composite on top.
	void draw(int eye);

	bool is_camera_on() const { return camera_on; }
};
