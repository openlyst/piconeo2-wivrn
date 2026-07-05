#pragma once

// Stubs for PVR SDK sensor functions not available in OpenXR build
// Head pose is set from OpenXR via pico_native_tracker::set_head_pose

static inline int Pvr_GetMainSensorState(float* qx, float* qy, float* qz, float* qw,
                                          float* px, float* py, float* pz,
                                          float* vfov, float* hfov, int* viewNum)
{
    return -1;
}

static inline int Pvr_ResetSensor(int type)
{
    return 0;
}
