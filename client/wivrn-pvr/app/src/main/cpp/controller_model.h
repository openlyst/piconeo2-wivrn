#pragma once
// Textured controller model renderer for the Pico Neo 2 controllers.
// Loads the system OBJ meshes (triangles + UVs) and the button-state
// PNG textures, renders them as solid textured triangles attached to
// the live controller pose. Texture swaps based on which button is
// pressed (idle / trigger / touchpad / app / home / volume).
#include <GLES3/gl3.h>

enum CtrlTexture {
    CTRL_TEX_IDLE = 0,
    CTRL_TEX_TRIGGER,
    CTRL_TEX_TOUCHPAD,
    CTRL_TEX_APP,
    CTRL_TEX_HOME,
    CTRL_TEX_VOLDOWN,
    CTRL_TEX_VOLUP,
    CTRL_TEX_COUNT
};

// Per-hand controller model state.
struct ControllerModel {
    GLuint vao = 0, vbo = 0;
    int vertCount = 0;
    GLuint textures[CTRL_TEX_COUNT] = {};
    bool loaded = false;
};

// Initialize both controller models (loads OBJ + textures from the
// system path). Safe to call once at startup; subsequent calls are
// no-ops unless reload=true.
void initControllerModels(bool reload = false);

// Render a single controller at the given pose. Picks the appropriate
// texture based on button state. Uses the caller's projection/view.
// hand: 0=left, 1=right.
// buttons: trigger, grip, touchpad, app(a/b), home, volume_up, volume_down
void drawControllerModel(int hand, const float quat[4], const float pos[3],
                         const float mvp[16],
                         bool trigger, bool grip, bool touchpad,
                         bool appButton, bool home,
                         bool volUp, bool volDown,
                         bool connected);

// Cleanup GL resources.
void cleanupControllerModels();
