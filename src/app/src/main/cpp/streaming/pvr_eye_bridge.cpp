#include "eye_tracking.h"
#include "pico_sdk.h"
#include "math3d.h"
#include <atomic>
#include <cmath>

// Bridge the pico_oxr eye-tracking interface expected by pico_native's tracker
// and streaming_client onto wivrn-pvr's existing Pvr_UnitySDK eye state.

std::atomic<float> gGazeQuat[4] = {{0.0f}, {0.0f}, {0.0f}, {1.0f}};
std::atomic<float> gEyeOpenness[2] = {{1.0f}, {1.0f}};
std::atomic<bool>  gEyeOpennessValid{false};
std::atomic<float> gGazePitch{0.0f};
std::atomic<float> gGazeYaw{0.0f};

// Recentering: Pvr_ResetSensor and Pvr_ResetSensorAll are exported by
// libPvr_UnitySDK.so (confirmed via nm -D). The previous stub here shadowed
// the real SDK function with a no-op that only reset the tracking origin.
// We now call the real SDK functions directly via the header declarations.

// Poll the existing wivrn-pvr eye state and mirror it into the pico_oxr
// globals so the WiVRn tracker can forward gaze/openness when supported.
void pollEyeGaze()
{
	if (!gEyeOnline.load())
	{
		gGazeValid.store(false);
		gEyeOpennessValid.store(false);
		return;
	}

	float gx = gGazeLocal[0].load();
	float gy = gGazeLocal[1].load();
	float gz = gGazeLocal[2].load();

	float pitch = std::asin(gy);
	float yaw = std::atan2(-gx, -gz);
	gGazePitch.store(pitch);
	gGazeYaw.store(yaw);

	// Gaze direction -> quaternion q = Rx(pitch) * Ry(yaw) (looking down -Z, head-local).
	float sinp = std::sin(pitch * 0.5f);
	float cosp = std::cos(pitch * 0.5f);
	float siny = std::sin(yaw * 0.5f);
	float cosy = std::cos(yaw * 0.5f);
	gGazeQuat[0].store(sinp * cosy);
	gGazeQuat[1].store(cosp * siny);
	gGazeQuat[2].store(sinp * siny);
	gGazeQuat[3].store(cosp * cosy);
	gGazeValid.store(true);

	if (gEyeHaveOpen)
	{
		gEyeOpenness[0].store(gEyeOpen[0]);
		gEyeOpenness[1].store(gEyeOpen[1]);
		gEyeOpennessValid.store(true);
	}
}
