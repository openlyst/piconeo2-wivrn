#pragma once

// Stubs for OpenXR body tracking extension types not present in the Pico Neo2 SDK.
// Only the subset needed by wivrn_packets.h is defined here.

#include <openxr/openxr.h>

#ifndef XR_BODY_JOINT_COUNT_FB
#define XR_BODY_JOINT_COUNT_FB 70
#endif

#ifndef XR_FULL_BODY_JOINT_COUNT_META
#define XR_FULL_BODY_JOINT_COUNT_META 84
#endif

#ifndef XR_BODY_JOINT_COUNT_BD
#define XR_BODY_JOINT_COUNT_BD 24
#endif

#ifndef XR_TYPE_BODY_SKELETON_FB
#define XR_TYPE_BODY_SKELETON_FB 0
#endif

#pragma pack(push, 8)
typedef struct XrBodySkeletonJointFB {
    int32_t    joint;
    int32_t    parentJoint;
    XrPosef    pose;
} XrBodySkeletonJointFB;
#pragma pack(pop)
