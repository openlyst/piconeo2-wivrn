#include "openxr_tracking.h"
#include <spdlog/spdlog.h>
#include <cstring>
#include <vector>

#define CHK_XR(call) \
	do { \
		XrResult _r = (call); \
		if (_r != XR_SUCCESS) { \
			spdlog::warn("OpenXR: {} failed: {}", #call, (int)_r); \
		} \
	} while (0)

bool openxr_tracker::init(JavaVM* vm, jobject activity, EGLDisplay display, EGLConfig config, EGLContext context)
{
	PFN_xrInitializeLoaderKHR initLoader = nullptr;
	CHK_XR(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction*)&initLoader));

	if (initLoader)
	{
		XrLoaderInitInfoAndroidKHR loaderInfo = {};
		loaderInfo.type = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR;
		loaderInfo.applicationVM = vm;
		loaderInfo.applicationContext = activity;
		CHK_XR(initLoader((XrLoaderInitInfoBaseHeaderKHR*)&loaderInfo));
	}

	XrInstanceCreateInfoAndroidKHR androidInfo = {};
	androidInfo.type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR;
	androidInfo.applicationVM = vm;
	androidInfo.applicationActivity = activity;

	const char* extensions[] = {
		XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
		XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
		XR_PICO_ANDROID_CONTROLLER_FUNCTION_EXT_ENABLE_EXTENSION_NAME,
		XR_PICO_VIEW_STATE_EXT_ENABLE_EXTENSION_NAME,
	};

	XrInstanceCreateInfo createInfo = {};
	createInfo.type = XR_TYPE_INSTANCE_CREATE_INFO;
	createInfo.next = &androidInfo;
	createInfo.enabledExtensionCount = 4;
	createInfo.enabledExtensionNames = extensions;
	createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	strcpy(createInfo.applicationInfo.applicationName, "WiVRn Neo2");
	createInfo.applicationInfo.applicationVersion = 1;
	strcpy(createInfo.applicationInfo.engineName, "WiVRn");
	createInfo.applicationInfo.engineVersion = 1;

	XrResult res = xrCreateInstance(&createInfo, &instance);
	if (res != XR_SUCCESS)
	{
		spdlog::warn("xrCreateInstance failed: {}", (int)res);
		return false;
	}
	spdlog::info("OpenXR instance created for tracking");

	XrSystemGetInfo systemInfo = {};
	systemInfo.type = XR_TYPE_SYSTEM_GET_INFO;
	systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

	res = xrGetSystem(instance, &systemInfo, &systemId);
	if (res != XR_SUCCESS)
	{
		spdlog::warn("xrGetSystem failed: {}", (int)res);
		return false;
	}

	XrSystemProperties sysProps = {};
	sysProps.type = XR_TYPE_SYSTEM_PROPERTIES;
	CHK_XR(xrGetSystemProperties(instance, systemId, &sysProps));
	spdlog::info("OpenXR system: {}, orientationTracking={}, positionTracking={}",
		sysProps.systemName,
		sysProps.trackingProperties.orientationTracking ? "yes" : "no",
		sysProps.trackingProperties.positionTracking ? "yes" : "no");

	for (int i = 0; i < 2; i++)
		views[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
	CHK_XR(xrEnumerateViewConfigurationViews(instance, systemId,
		XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &viewCount, views));
	spdlog::info("OpenXR views: {} views, {}x{}", viewCount,
		views[0].recommendedImageRectWidth, views[0].recommendedImageRectHeight);

	XrGraphicsBindingOpenGLESAndroidKHR graphicsBinding = {};
	graphicsBinding.type = XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR;
	graphicsBinding.display = display;
	graphicsBinding.config = config;
	graphicsBinding.context = context;

	XrSessionCreateInfo sessionInfo = {};
	sessionInfo.type = XR_TYPE_SESSION_CREATE_INFO;
	sessionInfo.next = &graphicsBinding;
	sessionInfo.systemId = systemId;

	res = xrCreateSession(instance, &sessionInfo, &session);
	if (res != XR_SUCCESS)
	{
		spdlog::warn("xrCreateSession failed: {}", (int)res);
		return false;
	}
	spdlog::info("OpenXR session created");

	XrReferenceSpaceCreateInfo spaceInfo = {};
	spaceInfo.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;

	res = xrCreateReferenceSpace(session, &spaceInfo, &appSpace);
	if (res != XR_SUCCESS)
	{
		spdlog::warn("xrCreateReferenceSpace failed: {}", (int)res);
		return false;
	}

	// Actions
	XrActionSetCreateInfo actionSetInfo = {};
	actionSetInfo.type = XR_TYPE_ACTION_SET_CREATE_INFO;
	actionSetInfo.priority = 0;
	strcpy(actionSetInfo.actionSetName, "tracking");
	strcpy(actionSetInfo.localizedActionSetName, "Tracking");
	CHK_XR(xrCreateActionSet(instance, &actionSetInfo, &actionSet));

	CHK_XR(xrStringToPath(instance, "/user/hand/left", &leftHandPath));
	CHK_XR(xrStringToPath(instance, "/user/hand/right", &rightHandPath));

	XrPath subactionPaths[] = {leftHandPath, rightHandPath};

	auto create_action = [&](const char* name, const char* localName, XrActionType type, XrAction& outAction) {
		XrActionCreateInfo info = {};
		info.type = XR_TYPE_ACTION_CREATE_INFO;
		info.actionType = type;
		strcpy(info.actionName, name);
		strcpy(info.localizedActionName, localName);
		info.countSubactionPaths = 2;
		info.subactionPaths = subactionPaths;
		CHK_XR(xrCreateAction(actionSet, &info, &outAction));
	};

	create_action("hand_pose", "Hand Pose", XR_ACTION_TYPE_POSE_INPUT, poseAction);
	create_action("trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT, triggerAction);
	create_action("squeeze", "Squeeze", XR_ACTION_TYPE_FLOAT_INPUT, squeezeAction);
	create_action("menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT, menuAction);
	create_action("thumbstick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT, thumbstickAction);
	create_action("a_button", "A Button", XR_ACTION_TYPE_BOOLEAN_INPUT, aButtonAction);
	create_action("b_button", "B Button", XR_ACTION_TYPE_BOOLEAN_INPUT, bButtonAction);
	create_action("thumbstick_click", "Thumbstick Click", XR_ACTION_TYPE_BOOLEAN_INPUT, thumbstickClickAction);

	// Suggest bindings for khr_simple_controller
	XrPath profilePath;
	CHK_XR(xrStringToPath(instance, "/interaction_profiles/khr/simple_controller", &profilePath));

	std::vector<XrActionSuggestedBinding> bindings;
	auto add_binding = [&](XrAction action, const char* path) {
		XrPath p;
		xrStringToPath(instance, path, &p);
		bindings.push_back({action, p});
	};

	add_binding(poseAction, "/user/hand/left/input/grip/pose");
	add_binding(poseAction, "/user/hand/right/input/grip/pose");
	add_binding(triggerAction, "/user/hand/left/input/select/click");
	add_binding(triggerAction, "/user/hand/right/input/select/click");
	add_binding(menuAction, "/user/hand/left/input/menu/click");
	add_binding(menuAction, "/user/hand/right/input/menu/click");
	add_binding(squeezeAction, "/user/hand/left/input/squeeze/value");
	add_binding(squeezeAction, "/user/hand/right/input/squeeze/value");

	XrInteractionProfileSuggestedBinding suggestedBinding = {};
	suggestedBinding.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
	suggestedBinding.interactionProfile = profilePath;
	suggestedBinding.countSuggestedBindings = (uint32_t)bindings.size();
	suggestedBinding.suggestedBindings = bindings.data();
	CHK_XR(xrSuggestInteractionProfileBindings(instance, &suggestedBinding));

	XrSessionActionSetsAttachInfo attachInfo = {};
	attachInfo.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO;
	attachInfo.countActionSets = 1;
	attachInfo.actionSets = &actionSet;
	CHK_XR(xrAttachSessionActionSets(session, &attachInfo));

	spdlog::info("OpenXR tracking initialized with actions");
	return true;
}

void openxr_tracker::shutdown()
{
	if (actionSet) { xrDestroyActionSet(actionSet); actionSet = XR_NULL_HANDLE; }
	if (appSpace) { xrDestroySpace(appSpace); appSpace = XR_NULL_HANDLE; }
	if (session) { xrDestroySession(session); session = XR_NULL_HANDLE; }
	if (instance) { xrDestroyInstance(instance); instance = XR_NULL_HANDLE; }
	ready = false;
}

void openxr_tracker::poll_events()
{
	XrEventDataBuffer eventBuffer = {};
	eventBuffer.type = XR_TYPE_EVENT_DATA_BUFFER;

	while (xrPollEvent(instance, &eventBuffer) == XR_SUCCESS)
	{
		if (eventBuffer.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
		{
			auto* stateEvent = (XrEventDataSessionStateChanged*)&eventBuffer;
			sessionState = stateEvent->state;

			switch (stateEvent->state)
			{
				case XR_SESSION_STATE_READY:
					CHK_XR(xrBeginSession(session, nullptr));
					ready = true;
					spdlog::info("OpenXR session begun");
					break;
				case XR_SESSION_STATE_STOPPING:
					CHK_XR(xrEndSession(session));
					ready = false;
					spdlog::info("OpenXR session ended");
					break;
				default:
					break;
			}
		}
		eventBuffer.type = XR_TYPE_EVENT_DATA_BUFFER;
	}
}

void openxr_tracker::sync_actions()
{
	if (!ready || !session) return;

	XrActiveActionSet activeActionSet = {};
	activeActionSet.actionSet = actionSet;
	activeActionSet.subactionPath = XR_NULL_PATH;

	XrActionsSyncInfo syncInfo = {};
	syncInfo.type = XR_TYPE_ACTIONS_SYNC_INFO;
	syncInfo.countActiveActionSets = 1;
	syncInfo.activeActionSets = &activeActionSet;
	CHK_XR(xrSyncActions(session, &syncInfo));
}

openxr_tracker::head_pose openxr_tracker::get_head_pose(XrTime displayTime)
{
	head_pose result = {};
	result.valid = false;

	if (!ready || !session) return result;

	XrViewLocateInfo viewLocateInfo = {};
	viewLocateInfo.type = XR_TYPE_VIEW_LOCATE_INFO;
	viewLocateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	viewLocateInfo.displayTime = displayTime;
	viewLocateInfo.space = appSpace;

	XrViewState viewState = {};
	viewState.type = XR_TYPE_VIEW_STATE;

	XrView views[2] = {};
	for (int i = 0; i < 2; i++)
		views[i].type = XR_TYPE_VIEW;

	uint32_t viewCount = 2;
	CHK_XR(xrLocateViews(session, &viewLocateInfo, &viewState, 2, &viewCount, views));

	bool posValid = (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
	bool orientValid = (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;

	if (posValid && orientValid)
	{
		for (uint32_t i = 0; i < viewCount && i < 2; i++)
		{
			result.eyePoses[i] = views[i].pose;
			result.eyeFovs[i] = views[i].fov;
		}
		result.valid = true;
	}

	return result;
}

openxr_tracker::controller_pose openxr_tracker::get_controller_pose(int hand, XrTime displayTime)
{
	controller_pose result = {};
	result.connected = false;

	if (!ready || !session) return result;

	XrPath handPath = (hand == 0) ? leftHandPath : rightHandPath;

	XrActionStateGetInfo getInfo = {};
	getInfo.type = XR_TYPE_ACTION_STATE_GET_INFO;
	getInfo.action = poseAction;
	getInfo.subactionPath = handPath;

	XrActionStatePose poseState = {};
	poseState.type = XR_TYPE_ACTION_STATE_POSE;
	CHK_XR(xrGetActionStatePose(session, &getInfo, &poseState));

	if (!poseState.isActive)
		return result;

	result.connected = true;

	XrActionSpaceCreateInfo spaceInfo = {};
	spaceInfo.type = XR_TYPE_ACTION_SPACE_CREATE_INFO;
	spaceInfo.action = poseAction;
	spaceInfo.subactionPath = handPath;
	spaceInfo.poseInActionSpace.orientation.w = 1.0f;

	XrSpace poseSpace;
	CHK_XR(xrCreateActionSpace(session, &spaceInfo, &poseSpace));

	XrSpaceLocation spaceLoc = {};
	spaceLoc.type = XR_TYPE_SPACE_LOCATION;
	CHK_XR(xrLocateSpace(poseSpace, appSpace, displayTime, &spaceLoc));

	result.positionValid = (spaceLoc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
	result.orientationValid = (spaceLoc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
	result.pose = spaceLoc.pose;

	xrDestroySpace(poseSpace);
	return result;
}

openxr_tracker::controller_input openxr_tracker::get_controller_input(int hand)
{
	controller_input result = {};
	result.active = false;

	if (!ready || !session) return result;

	XrPath handPath = (hand == 0) ? leftHandPath : rightHandPath;

	auto get_bool = [&](XrAction action, bool& out) -> bool {
		XrActionStateGetInfo getInfo = {};
		getInfo.type = XR_TYPE_ACTION_STATE_GET_INFO;
		getInfo.action = action;
		getInfo.subactionPath = handPath;

		XrActionStateBoolean state = {};
		state.type = XR_TYPE_ACTION_STATE_BOOLEAN;
		XrResult r = xrGetActionStateBoolean(session, &getInfo, &state);
		if (r == XR_SUCCESS && state.isActive)
		{
			out = state.currentState;
			return true;
		}
		return false;
	};

	auto get_float = [&](XrAction action, float& out) -> bool {
		XrActionStateGetInfo getInfo = {};
		getInfo.type = XR_TYPE_ACTION_STATE_GET_INFO;
		getInfo.action = action;
		getInfo.subactionPath = handPath;

		XrActionStateFloat state = {};
		state.type = XR_TYPE_ACTION_STATE_FLOAT;
		XrResult r = xrGetActionStateFloat(session, &getInfo, &state);
		if (r == XR_SUCCESS && state.isActive)
		{
			out = state.currentState;
			return true;
		}
		return false;
	};

	auto get_vec2 = [&](XrAction action, float& outX, float& outY) -> bool {
		XrActionStateGetInfo getInfo = {};
		getInfo.type = XR_TYPE_ACTION_STATE_GET_INFO;
		getInfo.action = action;
		getInfo.subactionPath = handPath;

		XrActionStateVector2f state = {};
		state.type = XR_TYPE_ACTION_STATE_VECTOR2F;
		XrResult r = xrGetActionStateVector2f(session, &getInfo, &state);
		if (r == XR_SUCCESS && state.isActive)
		{
			outX = state.currentState.x;
			outY = state.currentState.y;
			return true;
		}
		return false;
	};

	bool anyActive = false;
	anyActive |= get_float(triggerAction, result.trigger);
	anyActive |= get_float(squeezeAction, result.squeeze);
	anyActive |= get_vec2(thumbstickAction, result.thumbstickX, result.thumbstickY);
	anyActive |= get_bool(aButtonAction, result.aButton);
	anyActive |= get_bool(bButtonAction, result.bButton);
	anyActive |= get_bool(menuAction, result.menuButton);
	anyActive |= get_bool(thumbstickClickAction, result.thumbstickClick);
	result.active = anyActive;

	return result;
}
