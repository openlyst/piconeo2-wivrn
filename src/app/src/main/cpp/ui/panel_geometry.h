#pragma once
// Layout constants extracted from the old settings_panel.h.
// The 3D settings panel rendering is gone, but server_list.cpp still
// references these for its (unused) 3D geometry builders.
#include "ui_kit.h"  // kUiText

constexpr float kCtX0 = -0.48f, kCtX1 = 0.60f;
constexpr float kCtTop = 0.48f, kCtBot = -0.48f;
constexpr float kSbX = 0.36f;
constexpr float kCtX1Content = kSbX - 0.015f;
