#pragma once

/*
 * Declarations for the prebuilt Pico Neo 2 native SDK (libPvr_UnitySDK.so).
 * These are reverse-engineered ABI contracts from the legacy PvrSDK.
 * Wrapped in a namespace to keep our call sites clean and distinct.
 */

namespace neo2_sdk
{
// Sensor initialization
int   init_sdk(int index);
int   start_sensor(int index);
int   enable_6dof(bool enable);
int   init_sensor_module();
void  set_init_activity(void * activity, void * vr_class);

// HMD tracking
int   get_hmd_state(float * x, float * y, float * z, float * w,
                    float * px, float * py, float * pz,
                    float * vfov, float * hfov, int * viewNumber);

// Controller tracking (3DoF + arm model on Neo 2)
void  get_controller_pose(float * out_quat, float * out_pos);
void  get_pointer_pose(float * out_quat, float * out_pos);

// Tracking mode bitmask: ROTATION=0x1, POSITION=0x2
bool  set_tracking_mode(int mode);
int   get_tracking_mode();
void  disable_boundary();
void  shutdown_boundary();

// Tracking origin: 0=EyeLevel, 1=FloorLevel, 2=StageLevel
bool  set_tracking_origin(int origin_type);
float get_floor_height();
int   get_boundary_dimensions(float * x, float * y, float * z, bool is_play_area);
bool  is_boundary_configured();

// Render event infrastructure
void * get_render_event_func();
bool  set_single_pass_depth_buffer_size(int width, int height);
void  get_fov(float * out_a, float * out_b);
float get_ipd();

// HW compositor (DIATW) eye-buffer submit
void  set_current_render_texture(unsigned int tex_id);
void  begin_eye_event();
void  end_eye_event();
void  camera_end_frame(unsigned int eye, unsigned int tex_id);

struct pose_block { float v[22]; };
void  change_render_pose(unsigned int eye, unsigned int pad, pose_block blk);
void  set_async_timewarp(unsigned char enable);
void  timewarp_event(unsigned int eye);

// SDK globals
extern float eye_texture_fov_0;
extern float eye_texture_fov_1;
extern float eye_buffer_fov_x;
extern float eye_buffer_fov_y;

// Performance pinning (Qualcomm VR perf service)
int  set_cpu_level(int level, bool sustained);
int  set_gpu_level(int level, bool sustained);
void get_cpu_level(int & level, bool & sustained);
void get_gpu_level(int & level, bool & sustained);

// Panel backlight
void set_screen_brightness(int brightness);
void get_screen_brightness(int & brightness);
constexpr int brightness_min = 8, brightness_max = 255;

// Vsync phase oracle
double get_fractional_vsync();

// Internal config accessors
float get_float_config(int idx);
int   get_int_config(int idx);

// Render event ids
enum { EV_INIT_RENDER_THREAD = 1024, EV_PAUSE = 1025, EV_RESUME = 1026 };
using render_event_fn = void (*)(int);
} // namespace neo2_sdk

// Thin C++ wrappers around the actual exported symbols
extern "C" {
int   Pvr_Init(int);
int   Pvr_StartSensor(int);
int   Pvr_Enable6DofModule(bool);
int   InitSensor();
void  Pvr_SetInitActivity(void *, void *);
int   Pvr_GetMainSensorState(float *, float *, float *, float *,
                             float *, float *, float *,
                             float *, float *, int *);
void  Pvr_GetControllerTrackingData(float *, float *);
void  Pvr_GetPointerPose(float *, float *);
bool  Pvr_SetTrackingMode(int);
int   Pvr_GetTrackingMode();
void  Pvr_DisableBoundary();
void  Pvr_ShutdownSDKBoundary();
bool  Pvr_SetTrackingOriginType(int);
float Pvr_GetFloorHeight();
int   Pvr_BoundaryGetDimensions(float *, float *, float *, bool);
bool  Pvr_BoundaryGetConfigured();
void *GetRenderEventFunc();
bool  Pvr_SetSinglePassDepthBufferWidthHeight(int, int);
void  Pvr_GetFOV(float *, float *);
float Pvr_GetIPD();
void  Pvr_SetCurrentRenderTexture(unsigned int);
void  PvrBeginEyeEvent();
void  PvrEndEyeEvent();
void  PVR_CameraEndFrame(unsigned int, unsigned int);
struct PvrPoseBlk { float v[22]; };
void  PVR_ChangeRenderPose(unsigned int, unsigned int, struct PvrPoseBlk);
void  Pvr_SetAsyncTimeWarp(unsigned char);
void  PVR_TimeWarpEvent(unsigned int);
extern float fEyeTextureFov0;
extern float fEyeTextureFov1;
extern float gEyeBufferFovX;
extern float gEyeBufferFovY;
}

int  SetCpuLevel(int, bool);
int  SetGpuLevel(int, bool);
void GetCpuLevel(int &, bool &);
void GetGpuLevel(int &, bool &);
void SetHmdScreenBrightness(int);
void GetHmdScreenBrightness(int &);
namespace PVR { double GetFractionalVsync(); }

enum GlobalFloatConfigs : int {};
enum GlobalIntConfigs : int {};
float psmvr_GetFloatConfig(GlobalFloatConfigs);
int   psmvr_GetIntConfig(GlobalIntConfigs);
