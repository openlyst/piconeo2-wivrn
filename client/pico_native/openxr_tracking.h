#pragma once

#include <jni.h>
#include <EGL/egl.h>
#define XR_USE_PLATFORM_ANDROID 1
#define XR_USE_GRAPHICS_API_OPENGL_ES 1
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "openxr_pico.h"
#include <atomic>
#include <mutex>

struct openxr_tracker
{
	XrInstance instance = XR_NULL_HANDLE;
	XrSystemId systemId = XR_NULL_SYSTEM_ID;
	XrSession session = XR_NULL_HANDLE;
	XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
	XrSpace appSpace = XR_NULL_HANDLE;

	XrActionSet actionSet = XR_NULL_HANDLE;
	XrAction poseAction = XR_NULL_HANDLE;
	XrAction triggerAction = XR_NULL_HANDLE;
	XrAction squeezeAction = XR_NULL_HANDLE;
	XrAction menuAction = XR_NULL_HANDLE;
	XrAction thumbstickAction = XR_NULL_HANDLE;
	XrAction aButtonAction = XR_NULL_HANDLE;
	XrAction bButtonAction = XR_NULL_HANDLE;
	XrAction thumbstickClickAction = XR_NULL_HANDLE;

	XrPath leftHandPath = XR_NULL_PATH;
	XrPath rightHandPath = XR_NULL_PATH;

	XrViewConfigurationView views[2] = {};
	uint32_t viewCount = 2;

	std::atomic<bool> ready{false};

	bool init(JavaVM* vm, jobject activity, EGLDisplay display, EGLConfig config, EGLContext context);
	void shutdown();

	void poll_events();

	struct head_pose
	{
		XrPosef eyePoses[2];
		XrFovf eyeFovs[2];
		bool valid;
	};

	struct controller_pose
	{
		XrPosef pose;
		bool connected;
		bool positionValid;
		bool orientationValid;
	};

	struct controller_input
	{
		float trigger;
		float squeeze;
		float thumbstickX;
		float thumbstickY;
		bool aButton;
		bool bButton;
		bool menuButton;
		bool thumbstickClick;
		bool active;
	};

	head_pose get_head_pose(XrTime displayTime);
	controller_pose get_controller_pose(int hand, XrTime displayTime);
	controller_input get_controller_input(int hand);

	void sync_actions();
};
