#pragma once

extern "C" {
bool Pvr_SetTrackingOriginType(int trackingOriginType);
void PVR_CameraEndFrame(unsigned int eye, unsigned int texId);
struct PvrPoseBlk { float v[22]; };
struct pvrSensorState { uint8_t data[296]; };
pvrSensorState GetPredictedMainSensorState(double predictionTime);
void PVR_ChangeRenderPose(unsigned int eye, PvrPoseBlk *blk);
void Pvr_SetAsyncTimeWarp(unsigned char enable);
void PVR_TimeWarpEvent(unsigned int eye);
void *GetRenderEventFunc();
int   Pvr_Init(int index);
int   Pvr_StartSensor(int index);
int   Pvr_Enable6DofModule(bool enable);
int   InitSensor();
void  Pvr_SetInitActivity(void *activity, void *vrActivityClass);
void  Pvr_DisableBoundary();
void  Pvr_ShutdownSDKBoundary();
bool  Pvr_BoundaryGetConfigured();
float Pvr_GetFloorHeight();
bool  Pvr_SetSinglePassDepthBufferWidthHeight(int width, int height);
float Pvr_GetIPD();
void  Pvr_SetProjectionFov(float fovX, float fovY);
float Pvr_GetFOV();
int   Pvr_GetMainSensorState(float *x, float *y, float *z, float *w,
                             float *px, float *py, float *pz,
                             float *vfov, float *hfov, int *viewNumber);
int   Pvr_ResetSensor(int option);

// Eye tracking (Neo 2 EYE)
int   Pvr_GetTrackingMode();
bool  Pvr_SetTrackingMode(int mode);
bool  Pvr_GetEyeTrackingData(int *ls, int *rs, int *cs,
                             float *lpx, float *lpy, float *lpz,
                             float *rpx, float *rpy, float *rpz,
                             float *cpx, float *cpy, float *cpz,
                             float *lvx, float *lvy, float *lvz,
                             float *rvx, float *rvy, float *rvz,
                             float *cvx, float *cvy, float *cvz,
                             float *lo, float *ro, float *lpd, float *rpd,
                             float *lgx, float *lgy, float *lgz,
                             float *rgx, float *rgy, float *rgz,
                             float *fgx, float *fgy, float *fgz, int *fgs);
}

enum { EV_InitRenderThread = 1024, EV_Pause = 1025, EV_Resume = 1026 };
enum { PXR_RESET_POSITION = 0, PXR_RESET_ORIENTATION = 1, PXR_RESET_ORIENTATION_Y_ONLY = 2, PXR_RESET_ALL = 3 };
typedef void (*RenderEventFunc)(int);
