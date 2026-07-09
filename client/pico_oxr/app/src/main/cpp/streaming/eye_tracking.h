#pragma once
#include <atomic>

// Eye tracking for Pico Neo 2 EYE via libPvr_NativeSDK.so.
// On non-Eye units Pvr_GetEyeTrackingData returns false and gEyeOnline
// never flips, so all eye-gated behaviour stays disabled.

extern std::atomic<bool> gEyeSupported;  // EYE bit present in Pvr_GetTrackingMode
extern std::atomic<bool> gEyeOnline;     // latches true when first valid gaze arrives

// Latest head-local gaze orientation (quaternion x,y,z,w) + validity.
extern std::atomic<float> gGazeQuat[4];
extern std::atomic<bool>  gGazeValid;

// Eye openness (0=closed, 1=open) and validity, for fb_face2 blendshapes.
extern std::atomic<float> gEyeOpenness[2];
extern std::atomic<bool>  gEyeOpennessValid;

// Pupil dilation in millimeters and validity.
extern std::atomic<float> gPupilDilation[2];
extern std::atomic<bool>  gPupilDilationValid;

// Gaze pitch/yaw in radians (head-local), for eye-look blendshapes.
extern std::atomic<float> gGazePitch;
extern std::atomic<float> gGazeYaw;

// Boot: detect EYE support, leave IR off.
void initEyeTracking();

// (Re)evaluate tracking mode: enable EYE bit only if supported AND streaming.
// Safe to call from any thread; the blocking Pvr_SetTrackingMode runs on a worker.
void setEyeTrackingStreaming(bool streaming);

// Read eye tracking and update gaze/openness state. Call from tracking thread.
void pollEyeGaze();
