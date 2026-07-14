#pragma once
// Expose the stream swapchain state so the ALVR-to-WiVRn compatibility shim can
// blit decoded frames into the same ring the render thread submits to the SDK warp.
#include <GLES3/gl3.h>
#include <stdint.h>

extern GLuint gSwap[2][5];
extern GLuint gStreamFbo;
extern int    gSwapIdx;
extern uint32_t gStreamW;
extern uint32_t gStreamH;
