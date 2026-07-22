#pragma once
// Lobby panel builders: streaming diagnostics overlay + low-battery popup.
// Each builder emits pos.xyz+rgb geometry into a vertex vector (panel-local metres).
#include <vector>
#include "ui_kit.h"

// page 1 = ALVR pipeline metrics; page 2 = system (CPU/GPU/heat) telemetry.
void buildDiagOverlay(std::vector<float> &v, int page);
// Low-battery popup card, tinted by severity (<=5 red, else amber).
void buildBatteryWarn(std::vector<float> &v, int pct);

// CJK test panel: background quad (flat verts) + textured text quads (8-float verts).
// Returns text vertex count; fills bgV with the panel background.
int buildCjkTestPanel(std::vector<float> &bgV, std::vector<float> &textV);
