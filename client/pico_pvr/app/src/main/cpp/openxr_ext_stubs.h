#pragma once

#include <cstdint>

// Stubs for OpenXR face tracking extension types not available in Pico SDK headers
// Must match official OpenXR definitions exactly for protocol version hash compatibility

#ifndef XR_FACE_PARAMETER_COUNT_ANDROID
#define XR_FACE_PARAMETER_COUNT_ANDROID   68
#endif

#ifndef XR_FACE_REGION_CONFIDENCE_COUNT_ANDROID
#define XR_FACE_REGION_CONFIDENCE_COUNT_ANDROID 3
#endif

#ifndef XR_FACE_EXPRESSION2_COUNT_FB
#define XR_FACE_EXPRESSION2_COUNT_FB 70
#endif

#ifndef XR_FACE_CONFIDENCE2_COUNT_FB
#define XR_FACE_CONFIDENCE2_COUNT_FB 2
#endif

#ifndef XR_FACIAL_EXPRESSION_EYE_COUNT_HTC
#define XR_FACIAL_EXPRESSION_EYE_COUNT_HTC 14
#endif

#ifndef XR_FACIAL_EXPRESSION_LIP_COUNT_HTC
#define XR_FACIAL_EXPRESSION_LIP_COUNT_HTC 37
#endif

#ifndef XrFaceTrackingStateANDROID
typedef enum XrFaceTrackingStateANDROID {
    XR_FACE_TRACKING_STATE_PAUSED_ANDROID = 0,
    XR_FACE_TRACKING_STATE_STOPPED_ANDROID = 1,
    XR_FACE_TRACKING_STATE_TRACKING_ANDROID = 2,
    XR_FACE_TRACKING_STATE_MAX_ENUM_ANDROID = 0x7FFFFFFF
} XrFaceTrackingStateANDROID;
#endif
