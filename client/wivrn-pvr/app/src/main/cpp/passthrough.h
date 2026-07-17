#pragma once
// Passthrough camera background for the lobby. Replaces the old dark-void +
// grid-floor environment with a live feed from the Neo 2 tracking cameras,
// so the user sees their surroundings while interacting with the lobby UI
// panels (pairing, settings, EQ). The panels themselves are still drawn by
// pico_lobby on top of this background.
//
// Uses the Android Camera2 NDK API directly because the Pico SDK's camera
// API is broken on this device (libtrackingclient.pxr.so is missing).
#include <GLES3/gl3.h>
#include <media/NdkImageReader.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraCaptureSession.h>

class pico_passthrough
{
	GLuint program = 0;
	GLuint vao = 0, vbo = 0;
	GLint sampler_loc = -1;

	// One texture per eye — the SBS camera frame is split into left/right
	// halves and uploaded to separate textures so both eyes can render in
	// the same frame without overwriting each other.
	GLuint eye_tex[2] = {0, 0};
	int   tex_w = 0, tex_h = 0;

	// Frame counter to detect when we're on the second eye of a pair.
	// acquireLatestImage is called once per frame (first eye), and the
	// image is held until the second eye finishes, then deleted.
	int  frame_seq = 0;
	AImage *pending_image = nullptr;
	int  pending_w = 0, pending_h = 0;
	uint8_t *pending_y = nullptr;
	int  pending_stride = 0;
	bool got_new_this_frame = false;   // set on eye 0, read on eye 1

	bool gl_ready = false;
	bool camera_on = false;

	// Camera2 state
	ACameraManager *cam_mgr = nullptr;
	ACameraDevice  *cam_dev = nullptr;
	ACameraCaptureSession *cam_session = nullptr;
	ACaptureRequest *cam_request = nullptr;
	ANativeWindow   *img_reader_win = nullptr;
	AImageReader    *img_reader = nullptr;

	void build_shaders();
	void build_geometry();
	void upload_eye(int eye, int w, int h, const uint8_t *data, int row_stride);

public:
	pico_passthrough() = default;
	~pico_passthrough();

	void init();
	void start();
	void stop();
	void draw(int eye);

	bool is_camera_on() const { return camera_on; }
};
