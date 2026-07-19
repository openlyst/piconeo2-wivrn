#pragma once
// Passthrough camera background for the lobby. Uses Android Camera2 NDK directly
// because the Pico SDK camera API is broken on this device (libtrackingclient.pxr.so
// is missing). Rendering uses a fisheye undistortion mesh from the Pico system's
// seethroughsetting app (grid_point_coord.txt).
#include <GLES3/gl3.h>
#include <media/NdkImageReader.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraCaptureSession.h>

class pico_passthrough
{
	GLuint program = 0;
	GLint  sampler_loc = -1;

	// Per-eye mesh: VAO + VBO (positions+UVs) + shared IBO
	GLuint eye_vao[2] = {0, 0};
	GLuint eye_vbo[2] = {0, 0};
	GLuint ibo = 0;
	int    index_count = 0;

	// One texture per eye — the SBS camera frame is split into left/right
	// halves and uploaded to separate textures.
	GLuint eye_tex[2] = {0, 0};

	bool gl_ready = false;
	bool camera_on = false;

	// Camera frame state. acquireLatestImage is called once per frame
	// (first eye), the image is held until a new one replaces it.
	AImage *pending_image = nullptr;
	int  pending_w = 0, pending_h = 0;
	uint8_t *pending_y = nullptr;
	int  pending_stride = 0;
	bool got_new_this_frame = false;

	// Camera2 state
	ACameraManager *cam_mgr = nullptr;
	ACameraDevice  *cam_dev = nullptr;
	ACameraCaptureSession *cam_session = nullptr;
	ACaptureRequest *cam_request = nullptr;
	ANativeWindow   *img_reader_win = nullptr;
	AImageReader    *img_reader = nullptr;

	void build_shaders();
	void build_mesh();
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
