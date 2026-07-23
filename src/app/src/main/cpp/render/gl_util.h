#pragma once
// Generic GL helpers shared by every draw module.
#include <GLES3/gl3.h>

// Compile a shader; logs the info log on failure. Returns the shader name.
GLuint compile(GLenum type, const char *src);
