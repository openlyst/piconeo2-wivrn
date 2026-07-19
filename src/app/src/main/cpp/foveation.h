#pragma once
// Read server foveation params from the ALVR settings JSON. Used at
// STREAMING_STARTED and REAL_TIME_CONFIG to detect mid-session changes.
// Fills out[6] = {center_size_x, center_size_y, center_shift_x,
//                 center_shift_y, edge_ratio_x, edge_ratio_y}.
void readFoveationParams(float out[6]);
