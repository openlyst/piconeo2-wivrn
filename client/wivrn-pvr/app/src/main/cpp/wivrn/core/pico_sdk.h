#pragma once

extern "C" {
bool Pvr_SetTrackingOriginType(int trackingOriginType);
void PVR_CameraEndFrame(unsigned int eye, unsigned int texId);
struct PvrPoseBlk { float v[22]; };
struct pvrSensorState { uint8_t data[296]; };
pvrSensorState GetPredictedMainSensorState(double predictionTime);
void PVR_ChangeRenderPose(unsigned int eye, unsigned int pad, PvrPoseBlk blk);
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

// Eye tracking (Neo 2 EYE only; returns false on non-Eye units)
bool  Pvr_GetEyeTrackingData(
    int *lStatus, int *rStatus, int *cStatus,
    float *lPx, float *lPy, float *lPz,
    float *rPx, float *rPy, float *rPz,
    float *cPx, float *cPy, float *cPz,
    float *lVx, float *lVy, float *lVz,
    float *rVx, float *rVy, float *rVz,
    float *cVx, float *cVy, float *cVz,
    float *lOpen, float *rOpen,
    float *lPupil, float *rPupil,
    float *lGuideX, float *lGuideY, float *lGuideZ,
    float *rGuideX, float *rGuideY, float *rGuideZ,
    float *fovGazeX, float *fovGazeY, float *fovGazeZ,
    int *fovGazeState);
bool  Pvr_SetTrackingMode(int trackingMode);
int   Pvr_GetTrackingMode();
}

enum { EV_InitRenderThread = 1024, EV_Pause = 1025, EV_Resume = 1026 };
enum { PXR_RESET_POSITION = 0, PXR_RESET_ORIENTATION = 1, PXR_RESET_ORIENTATION_Y_ONLY = 2, PXR_RESET_ALL = 3 };
typedef void (*RenderEventFunc)(int);
