#pragma once

#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

// Returns true if the WiVRn stream client exists and is actively streaming.
bool wivrn_streaming();

// Returns true if the stream is active AND the decoders are ready to produce
// frames. When this becomes true the render loop should create/resize the
// stream swapchain.
bool wivrn_stream_ready();

// Writes the current stream resolution into *w and *h. Returns true when the
// video description has been received and a resolution is known.
bool wivrn_stream_resolution(int *w, int *h);

// Blit the latest decoded WiVRn frame for one eye into the currently bound
// FBO. The caller must have bound the destination framebuffer and viewport.
// Returns true if a frame was available and blit.
bool wivrn_blit_eye(int eye, int viewport_w, int viewport_h);
