#include "eye_tracking.h"
#include "pico_sdk.h"
#include <spdlog/spdlog.h>
#include <cmath>
#include <cstring>
#include <thread>
#include <mutex>
#include <condition_variable>

std::atomic<bool> gEyeSupported{false};
std::atomic<bool> gEyeOnline{false};
std::atomic<float> gGazeQuat[4] = {{0.0f}, {0.0f}, {0.0f}, {1.0f}};
std::atomic<bool>  gGazeValid{false};

static const int MODE_POSITION = 0x2, MODE_EYE = 0x4;
static std::atomic<bool> gEyeIrOn{false};

// ---- worker thread for blocking Pvr_SetTrackingMode ----
static std::mutex              gReqMtx;
static std::condition_variable gReqCv;
static bool                    gReqPending = false;
static bool                    gReqStreaming = false;
static bool                    gWorkerStarted = false;

static void applyModeNow(bool streaming)
{
	bool wantEye = gEyeSupported.load() && streaming;
	int mode = MODE_POSITION | (wantEye ? MODE_EYE : 0);
	bool rc = Pvr_SetTrackingMode(mode);
	gEyeIrOn.store(wantEye);
	if (!wantEye)
	{
		gEyeOnline.store(false);
		gGazeValid.store(false);
	}
	spdlog::info("eye: tracking mode -> {} (streaming={} supported={} set={})",
		wantEye ? "POSITION|EYE" : "POSITION-only",
		streaming ? 1 : 0, gEyeSupported.load() ? 1 : 0, rc ? 1 : 0);
}

static void eyeModeWorker()
{
	for (;;)
	{
		bool streaming;
		{
			std::unique_lock<std::mutex> lk(gReqMtx);
			gReqCv.wait(lk, [] { return gReqPending; });
			gReqPending = false;
			streaming = gReqStreaming;
		}
		applyModeNow(streaming);
	}
}

void initEyeTracking()
{
	int supported = Pvr_GetTrackingMode();
	gEyeSupported.store((supported & MODE_EYE) != 0);
	Pvr_SetTrackingMode(MODE_POSITION);
	gEyeIrOn.store(false);
	gEyeOnline.store(false);
	gGazeValid.store(false);
	spdlog::info("eye: Pvr_GetTrackingMode supported=0x{:x} EYE={}",
		supported, gEyeSupported.load() ? 1 : 0);
}

void setEyeTrackingStreaming(bool streaming)
{
	std::lock_guard<std::mutex> lk(gReqMtx);
	if (!gWorkerStarted)
	{
		gWorkerStarted = true;
		std::thread(eyeModeWorker).detach();
	}
	gReqStreaming = streaming;
	gReqPending = true;
	gReqCv.notify_one();
}

void pollEyeGaze()
{
	if (!gEyeIrOn.load())
	{
		gGazeValid.store(false);
		return;
	}

	int ls = 0, rs = 0, cs = 0;
	float lp[3], rp[3], cp[3];
	float lv[3], rv[3], cv[3];
	float lo, ro, lpd, rpd;
	float lg[3], rg[3], fg[3];
	int fgs;

	bool ok = Pvr_GetEyeTrackingData(
		&ls, &rs, &cs,
		&lp[0], &lp[1], &lp[2], &rp[0], &rp[1], &rp[2], &cp[0], &cp[1], &cp[2],
		&lv[0], &lv[1], &lv[2], &rv[0], &rv[1], &rv[2], &cv[0], &cv[1], &cv[2],
		&lo, &ro, &lpd, &rpd,
		&lg[0], &lg[1], &lg[2], &rg[0], &rg[1], &rg[2],
		&fg[0], &fg[1], &fg[2], &fgs);
	if (!ok)
	{
		gGazeValid.store(false);
		return;
	}

	// Per-eye vectors are not populated on Neo 2 EYE; only combined gaze (cv) works.
	float cmag = sqrtf(cv[0] * cv[0] + cv[1] * cv[1] + cv[2] * cv[2]);
	bool valid = (cs > 0 && cmag > 0.5f && cmag < 2.0f);
	if (!valid)
	{
		gGazeValid.store(false);
		return;
	}

	if (!gEyeOnline.load())
	{
		gEyeOnline.store(true);
		spdlog::info("EYE TRACKING ONLINE: status L={} R={} C={}", ls, rs, cs);
	}

	// Normalize the combined gaze vector.
	float gx = cv[0], gy = cv[1], gz = cv[2];
	float gn = sqrtf(gx * gx + gy * gy + gz * gz);
	if (gn > 1e-6f) { gx /= gn; gy /= gn; gz /= gn; }

	// Decompose into decoupled pitch/yaw. OpenXR forward is -Z.
	// For q = Rx(pitch) * Ry(yaw): dir = (-sinY, cosY*sinP, -cosY*cosP)
	float yaw   = atan2f(-gx, sqrtf(gy * gy + gz * gz));
	float pitch = atan2f(gy, -gz);

	// Build head-local gaze quaternion: q = Rx(pitch) * Ry(yaw)
	float sp = sinf(pitch * 0.5f), cph = cosf(pitch * 0.5f);
	float sy = sinf(yaw * 0.5f), cyh = cosf(yaw * 0.5f);

	// Rx * Ry quaternion product
	float qx = sp * cyh;
	float qy = cph * sy;
	float qz = -sp * sy;
	float qw = cph * cyh;

	// Normalize
	float qn = sqrtf(qx * qx + qy * qy + qz * qz + qw * qw);
	if (qn > 1e-6f) { qx /= qn; qy /= qn; qz /= qn; qw /= qn; }

	gGazeQuat[0].store(qx);
	gGazeQuat[1].store(qy);
	gGazeQuat[2].store(qz);
	gGazeQuat[3].store(qw);
	gGazeValid.store(true);
}
