#pragma once

#include <cstdint>

// Stubs for OpenXR face tracking extension types not available in Pico SDK headers
// Use serializable types since wivrn_packets.h serializes these via boost::pfr

#ifndef XR_FACE_PARAMETER_COUNT_ANDROID
#define XR_FACE_PARAMETER_COUNT_ANDROID 18
#endif

#ifndef XR_FACE_REGION_CONFIDENCE_COUNT_ANDROID
#define XR_FACE_REGION_CONFIDENCE_COUNT_ANDROID 6
#endif

#ifndef XR_FACE_EXPRESSION2_COUNT_FB
#define XR_FACE_EXPRESSION2_COUNT_FB 72
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

typedef struct XrFaceTrackingStateANDROID {
    int32_t type;
    uint64_t next;
} XrFaceTrackingStateANDROID;
