#include <jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <pthread.h>
#include <unistd.h>
#include <cstring>
#include <cmath>
#include <ctime>
#include <mutex>
#include <atomic>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/android_sink.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include "lobby.h"
#include "pico_sdk.h"
#include "pico_tracking.h"

#define LOG_TAG "WiVRn-PVR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#ifndef EGL_CONTEXT_PRIORITY_LEVEL_IMG
#define EGL_CONTEXT_PRIORITY_LEVEL_IMG 0x3100
#define EGL_CONTEXT_PRIORITY_HIGH_IMG  0x3101
#define EGL_CONTEXT_PRIORITY_LOW_IMG   0x3103
#endif

static constexpr int kSwapLen = 3;
static constexpr int eye_width = 1600;
static constexpr int eye_height = 1600;

// ---------------------------------------------------------------------------
// Shared state (JNI <-> render thread)
// ---------------------------------------------------------------------------

static JavaVM* gVM = nullptr;
static jobject gActivity = nullptr;
static jclass gVrClass = nullptr;
static jmethodID g_onLobbyTouchMethod = nullptr;

static pthread_t gThread;
static std::atomic<bool> gRunning{false};

static std::mutex gWinMutex;
static ANativeWindow* gPendingWindow = nullptr;
static std::atomic<bool> gWindowDirty{false};

static std::mutex gStateMutex;
static float gHeadOrient[4] = {0, 0, 0, 1};
static float gHeadPos[3] = {0, 0, 0};
static controller_sample gControllers[2];

static std::atomic<bool> gReady{false};
static std::atomic<float> gIpd{0.064f};
static std::atomic<bool> gRecenterRequested{false};

static int prevTouchHand = -1;

static std::mutex gHapticMutex;
struct haptic_cmd { float amplitude = 0; float duration_ms = 0; };
static haptic_cmd gPendingHaptics[2];

// Lobby instance (shared between JNI and render thread)
static pico_lobby gLobby;

// ---------------------------------------------------------------------------
// Window hand-off
// ---------------------------------------------------------------------------

static void setWindow(ANativeWindow* win)
{
    {
        std::lock_guard<std::mutex> lk(gWinMutex);
        if (gPendingWindow) ANativeWindow_release(gPendingWindow);
        gPendingWindow = win;
        if (gPendingWindow) ANativeWindow_acquire(gPendingWindow);
    }
    gWindowDirty.store(true);
}

// ---------------------------------------------------------------------------
// Render thread state
// ---------------------------------------------------------------------------

struct RenderState {
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLConfig cfg = nullptr;
    EGLContext ctx = EGL_NO_CONTEXT;
    EGLContext warpCtx = EGL_NO_CONTEXT;
    EGLSurface pbuf = EGL_NO_SURFACE;
    EGLSurface winSurface = EGL_NO_SURFACE;
    ANativeWindow* curWin = nullptr;

    GLuint swapTex[2][kSwapLen] = {};
    GLuint streamFbo = 0;
    int swapIdx = 0;
    int prevSwapIdx = 0;
    bool prevSwapValid = false;

    bool rtInited = false;
    bool warpToWindow = false;
    bool atwEnabled = false;
    bool sdkInited = false;

    struct Slot {
        float poseOrient[2][4];
        float posePos[2][3];
        GLsync fence = 0;
    };
    Slot slots[kSwapLen];

    XrFovf eyeFov[2] = {
        {-0.8814f, 0.8814f, 0.8814f, -0.8814f},
        {-0.8814f, 0.8814f, 0.8814f, -0.8814f},
    };
};

// ---------------------------------------------------------------------------
// PVR SDK init
// ---------------------------------------------------------------------------

static void initPvrSdk(RenderState& rs)
{
    JNIEnv* env = nullptr;
    bool attached = false;
    if (gVM->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK)
    {
        if (gVM->AttachCurrentThread(&env, nullptr) == JNI_OK)
            attached = true;
    }

    Pvr_SetInitActivity((void*)gActivity, (void*)gVrClass);
    Pvr_Enable6DofModule(true);
    int initRc = Pvr_Init(0);
    int sensRc = InitSensor();
    int startRc = Pvr_StartSensor(0);
    LOGI("Pvr_Init=%d InitSensor=%d Pvr_StartSensor=%d", initRc, sensRc, startRc);

    bool guardian = Pvr_BoundaryGetConfigured();
    int originType = guardian ? 2 : 1;
    Pvr_SetTrackingOriginType(originType);
    LOGI("Tracking origin: %s, floorHeight=%.3f, guardian=%d",
         guardian ? "StageLevel" : "FloorLevel", Pvr_GetFloorHeight(), guardian);

    Pvr_DisableBoundary();
    Pvr_ShutdownSDKBoundary();

    float fov = 101.0f;
    Pvr_SetProjectionFov(fov, fov);
    LOGI("FOV set to %.1f", fov);

    rs.sdkInited = true;

    if (attached)
        gVM->DetachCurrentThread();
}

// ---------------------------------------------------------------------------
// EGL init
// ---------------------------------------------------------------------------

static bool initEgl(RenderState& rs)
{
    rs.dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(rs.dpy, nullptr, nullptr);

    const EGLint cfgAttribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_NONE
    };
    EGLint n = 0;
    eglChooseConfig(rs.dpy, cfgAttribs, &rs.cfg, 1, &n);
    if (!n)
    {
        LOGE("eglChooseConfig failed");
        return false;
    }

    const EGLint ctxAttribsLow[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_CONTEXT_PRIORITY_LEVEL_IMG, EGL_CONTEXT_PRIORITY_LOW_IMG,
        EGL_NONE
    };
    rs.ctx = eglCreateContext(rs.dpy, rs.cfg, EGL_NO_CONTEXT, ctxAttribsLow);
    if (rs.ctx == EGL_NO_CONTEXT)
    {
        const EGLint fb[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
        rs.ctx = eglCreateContext(rs.dpy, rs.cfg, EGL_NO_CONTEXT, fb);
    }

    const EGLint pbufAttribs[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    rs.pbuf = eglCreatePbufferSurface(rs.dpy, rs.cfg, pbufAttribs);
    eglMakeCurrent(rs.dpy, rs.pbuf, rs.pbuf, rs.ctx);

    LOGI("EGL initialized");
    return true;
}

static void initSwapchain(RenderState& rs)
{
    for (int e = 0; e < 2; e++)
    {
        glGenTextures(kSwapLen, rs.swapTex[e]);
        for (int i = 0; i < kSwapLen; i++)
        {
            glBindTexture(GL_TEXTURE_2D, rs.swapTex[e][i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, eye_width, eye_height, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    if (rs.streamFbo == 0)
        glGenFramebuffers(1, &rs.streamFbo);

    LOGI("swapchain initialized %dx%d", eye_width, eye_height);
}

// ---------------------------------------------------------------------------
// Surface change handling
// ---------------------------------------------------------------------------

static void onSurfaceChanged(RenderState& rs, ANativeWindow* newWin)
{
    if (rs.curWin)
    {
        if (rs.rtInited && rs.warpToWindow)
        {
            RenderEventFunc re = (RenderEventFunc)GetRenderEventFunc();
            if (re) re(EV_Pause);
        }
        eglMakeCurrent(rs.dpy, rs.pbuf, rs.pbuf, rs.ctx);
        if (rs.winSurface != EGL_NO_SURFACE)
        {
            eglDestroySurface(rs.dpy, rs.winSurface);
            rs.winSurface = EGL_NO_SURFACE;
        }
        ANativeWindow_release(rs.curWin);
        rs.curWin = nullptr;
    }

    if (newWin)
    {
        rs.curWin = newWin;
        rs.winSurface = eglCreateWindowSurface(rs.dpy, rs.cfg, rs.curWin, nullptr);
        if (rs.winSurface == EGL_NO_SURFACE)
        {
            LOGE("eglCreateWindowSurface failed 0x%x", eglGetError());
            return;
        }

        int winW, winH;
        eglQuerySurface(rs.dpy, rs.winSurface, EGL_WIDTH, &winW);
        eglQuerySurface(rs.dpy, rs.winSurface, EGL_HEIGHT, &winH);
        LOGI("window surface ready %dx%d", winW, winH);

        if (!rs.rtInited)
        {
            Pvr_SetSinglePassDepthBufferWidthHeight(winW / 2, winH);

            const EGLint ctxAttribsHigh[] = {
                EGL_CONTEXT_CLIENT_VERSION, 3,
                EGL_CONTEXT_PRIORITY_LEVEL_IMG, EGL_CONTEXT_PRIORITY_HIGH_IMG,
                EGL_NONE
            };
            rs.warpCtx = eglCreateContext(rs.dpy, rs.cfg, rs.ctx, ctxAttribsHigh);
            if (rs.warpCtx == EGL_NO_CONTEXT)
            {
                const EGLint fb[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
                rs.warpCtx = eglCreateContext(rs.dpy, rs.cfg, rs.ctx, fb);
            }

            eglMakeCurrent(rs.dpy, rs.winSurface, rs.winSurface, rs.warpCtx);
            RenderEventFunc re = (RenderEventFunc)GetRenderEventFunc();
            if (re)
            {
                re(EV_InitRenderThread);
                LOGI("EV_InitRenderThread issued");
            }
            eglMakeCurrent(rs.dpy, rs.pbuf, rs.pbuf, rs.ctx);

            rs.warpToWindow = true;
            rs.rtInited = true;

            Pvr_SetAsyncTimeWarp(1);
            rs.atwEnabled = true;
            LOGI("ATW enabled");
        }
        else if (rs.rtInited && rs.warpToWindow)
        {
            RenderEventFunc re = (RenderEventFunc)GetRenderEventFunc();
            eglMakeCurrent(rs.dpy, rs.winSurface, rs.winSurface, rs.warpCtx);
            if (re)
            {
                re(EV_InitRenderThread);
                re(EV_Resume);
            }
            eglMakeCurrent(rs.dpy, rs.pbuf, rs.pbuf, rs.ctx);
            LOGI("warp re-pointed to new surface");
        }
    }
    else
    {
        LOGI("window surface destroyed");
    }
}

// ---------------------------------------------------------------------------
// Head pose from PVR SDK
// ---------------------------------------------------------------------------

static void getHeadPose(float outOrient[4], float outPos[3])
{
    float x, y, z, w, px, py, pz, vfov, hfov;
    int viewNumber;
    int rc = Pvr_GetMainSensorState(&x, &y, &z, &w, &px, &py, &pz,
                                    &vfov, &hfov, &viewNumber);
    if (rc >= 0)
    {
        outOrient[0] = x; outOrient[1] = y; outOrient[2] = z; outOrient[3] = w;
        outPos[0] = px; outPos[1] = py; outPos[2] = pz;
    }
}

// ---------------------------------------------------------------------------
// Submit to ATW
// ---------------------------------------------------------------------------

static void submitToWarp(RenderState& rs, int slotIdx, uint64_t fenceWaitNs)
{
    if (rs.slots[slotIdx].fence)
    {
        glClientWaitSync(rs.slots[slotIdx].fence, GL_SYNC_FLUSH_COMMANDS_BIT, fenceWaitNs);
    }

    PVR_CameraEndFrame(0, rs.swapTex[0][slotIdx]);
    PVR_CameraEndFrame(1, rs.swapTex[1][slotIdx]);

    for (int e = 0; e < 2; e++)
    {
        float* q = rs.slots[slotIdx].poseOrient[e];
        float n2 = q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3];
        static float lastGoodQ[2][4] = {{0,0,0,1}, {0,0,0,1}};
        if (n2 > 1e-6f)
        {
            float inv = 1.0f / std::sqrt(n2);
            for (int i = 0; i < 4; i++) lastGoodQ[e][i] = q[i] * inv;
        }
        else
        {
            memcpy(q, lastGoodQ[e], sizeof(float) * 4);
        }

        float ipd = gIpd.load();
        float eyeOffset = (e == 0 ? -ipd * 0.5f : ipd * 0.5f);

        PvrPoseBlk blk;
        memset(&blk, 0, sizeof(blk));
        blk.v[0] = q[0]; blk.v[1] = q[1]; blk.v[2] = q[2]; blk.v[3] = q[3];
        blk.v[4] = eyeOffset;
        PVR_ChangeRenderPose(e, &blk);
    }

    PVR_TimeWarpEvent(0);
}

// ---------------------------------------------------------------------------
// Render lobby
// ---------------------------------------------------------------------------

static void renderLobby(RenderState& rs)
{
    float hOrient[4], hPos[3];
    {
        std::lock_guard<std::mutex> lk(gStateMutex);
        memcpy(hOrient, gHeadOrient, sizeof(hOrient));
        memcpy(hPos, gHeadPos, sizeof(hPos));
    }

    if (gRecenterRequested.exchange(false))
    {
        Pvr_ResetSensor(PXR_RESET_ALL);
        gLobby.recenter();
    }

    controller_sample cs[2];
    {
        std::lock_guard<std::mutex> lk(gStateMutex);
        memcpy(cs, gControllers, sizeof(cs));
    }

    gLobby.flush_pending_texture();

    glBindFramebuffer(GL_FRAMEBUFFER, rs.streamFbo);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    float ipd = gIpd.load();

    for (int eye = 0; eye < 2; eye++)
    {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
            GL_TEXTURE_2D, rs.swapTex[eye][rs.swapIdx], 0);
        glViewport(0, 0, eye_width, eye_height);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);

        gLobby.draw(eye, hOrient, hPos, cs, rs.eyeFov[eye], ipd, false);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Touch callback to Java
    if (gVM && gActivity && g_onLobbyTouchMethod)
    {
        bool anyHit = false;
        int hitHand = -1;
        for (int h = 0; h < 2; h++)
        {
            bool hasHit = gLobby.lobby_touch_x[h] >= 0;
            bool hasClick = gLobby.lobby_touch_down[h] || gLobby.lobby_touch_pressed[h];
            if (hasHit || hasClick)
            {
                anyHit = true;
                hitHand = h;
                break;
            }
        }

        bool prevAny = (prevTouchHand >= 0);
        bool stateChanged = (anyHit != prevAny) || (hitHand != prevTouchHand);

        float tx = -1.0f, ty = -1.0f;
        bool tdown = false, tpressed = false;
        float tthumb = 0.0f;
        if (anyHit)
        {
            tx = gLobby.lobby_touch_x[hitHand];
            ty = gLobby.lobby_touch_y[hitHand];
            tdown = gLobby.lobby_touch_down[hitHand];
            tpressed = gLobby.lobby_touch_pressed[hitHand];
            tthumb = gLobby.lobby_thumbstick_y[hitHand];
        }

        if (stateChanged || (anyHit && (tdown || tpressed)))
        {
            JNIEnv* env = nullptr;
            bool attached = false;
            if (gVM->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK)
            {
                if (gVM->AttachCurrentThread(&env, nullptr) == JNI_OK)
                    attached = true;
            }

            if (env)
            {
                env->CallVoidMethod(gActivity, g_onLobbyTouchMethod,
                                    tx, ty, tdown, tpressed, tthumb);
            }

            if (attached)
                gVM->DetachCurrentThread();
        }
        prevTouchHand = anyHit ? hitHand : -1;
    }

    for (int e = 0; e < 2; e++)
    {
        memcpy(rs.slots[rs.swapIdx].poseOrient[e], hOrient, sizeof(float) * 4);
        memcpy(rs.slots[rs.swapIdx].posePos[e], hPos, sizeof(float) * 3);
    }

    if (rs.slots[rs.swapIdx].fence) glDeleteSync(rs.slots[rs.swapIdx].fence);
    rs.slots[rs.swapIdx].fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();

    if (rs.prevSwapValid)
        submitToWarp(rs, rs.prevSwapIdx, 5000000ULL);
    else
        submitToWarp(rs, rs.swapIdx, 50000000ULL);

    rs.prevSwapIdx = rs.swapIdx;
    rs.prevSwapValid = true;
    rs.swapIdx = (rs.swapIdx + 1) % kSwapLen;
}

// ---------------------------------------------------------------------------
// Render thread entry point
// ---------------------------------------------------------------------------

static void* renderThread(void*)
{
    auto logger = spdlog::android_logger_mt("wivrn", "WiVRn-PVR");
    spdlog::set_default_logger(logger);
    spdlog::set_level(spdlog::level::debug);

    RenderState rs;

    if (!initEgl(rs))
    {
        LOGE("EGL init failed");
        return nullptr;
    }

    gLobby.init(eye_width, eye_height);

    initPvrSdk(rs);
    if (!rs.sdkInited)
    {
        LOGE("PVR SDK init failed");
        return nullptr;
    }

    initSwapchain(rs);

    gReady.store(true);
    LOGI("render thread ready");

    int targetFps = 72;
    int64_t targetFrameNs = 1000000000LL / targetFps;

    int frame = 0;

    while (gRunning.load())
    {
        int64_t tIterStart = 0;
        {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            tIterStart = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
        }

        // Handle window changes
        if (gWindowDirty.load())
        {
            ANativeWindow* newWin = nullptr;
            {
                std::lock_guard<std::mutex> lk(gWinMutex);
                newWin = gPendingWindow;
                gPendingWindow = nullptr;
                gWindowDirty.store(false);
            }
            onSurfaceChanged(rs, newWin);
        }

        if (rs.winSurface == EGL_NO_SURFACE)
        {
            usleep(5000);
            continue;
        }

        // Give the warp thread time to fully initialize after EV_InitRenderThread
        // before we start submitting frames. The SDK's internal state isn't ready
        // immediately and PVR_ChangeRenderPose will NPE if called too early.
        if (rs.rtInited && frame == 0)
        {
            usleep(100000); // 100ms
        }

        // Update head pose
        float hOrient[4], hPos[3];
        getHeadPose(hOrient, hPos);
        {
            std::lock_guard<std::mutex> lk(gStateMutex);
            memcpy(gHeadOrient, hOrient, sizeof(hOrient));
            memcpy(gHeadPos, hPos, sizeof(hPos));
        }

        renderLobby(rs);

        frame++;

        // Trailing sleep: pace to one vsync from the START of this iteration.
        // PVR_TimeWarpEvent already blocks on vsync backpressure, so we only
        // sleep the remainder to hold a steady 72Hz cadence. Always leave >=3ms
        // so the warp thread gets CPU time to run.
        const uint64_t kVsyncNs = 13888889ULL; // 72Hz
        const uint64_t kMinGapNs = 3000000ULL;
        struct timespec tsNow;
        clock_gettime(CLOCK_MONOTONIC, &tsNow);
        uint64_t now = (uint64_t)tsNow.tv_sec * 1000000000ULL + tsNow.tv_nsec;
        uint64_t work = now - (uint64_t)tIterStart;
        uint64_t sleepNs = (work < kVsyncNs) ? (kVsyncNs - work) : 0;
        if (sleepNs < kMinGapNs) sleepNs = kMinGapNs;
        usleep((useconds_t)(sleepNs / 1000));
    }

    gReady.store(false);

    // Cleanup
    if (rs.atwEnabled)
        Pvr_SetAsyncTimeWarp(0);

    if (rs.winSurface != EGL_NO_SURFACE)
    {
        eglMakeCurrent(rs.dpy, rs.pbuf, rs.pbuf, rs.ctx);
        eglDestroySurface(rs.dpy, rs.winSurface);
        rs.winSurface = EGL_NO_SURFACE;
    }
    if (rs.curWin)
    {
        ANativeWindow_release(rs.curWin);
        rs.curWin = nullptr;
    }

    for (int e = 0; e < 2; e++)
    {
        for (int i = 0; i < kSwapLen; i++)
        {
            if (rs.swapTex[e][i]) glDeleteTextures(1, &rs.swapTex[e][i]);
        }
    }
    for (int i = 0; i < kSwapLen; i++)
    {
        if (rs.slots[i].fence) glDeleteSync(rs.slots[i].fence);
    }
    if (rs.streamFbo) glDeleteFramebuffers(1, &rs.streamFbo);

    if (rs.warpCtx != EGL_NO_CONTEXT) eglDestroyContext(rs.dpy, rs.warpCtx);
    if (rs.ctx != EGL_NO_CONTEXT)
    {
        eglMakeCurrent(rs.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(rs.dpy, rs.ctx);
    }
    if (rs.pbuf != EGL_NO_SURFACE) eglDestroySurface(rs.dpy, rs.pbuf);
    eglTerminate(rs.dpy);

    LOGI("render thread exited");
    return nullptr;
}

// ---------------------------------------------------------------------------
// JNI bridge
// ---------------------------------------------------------------------------

extern "C" {

JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_pvr_MainActivity_nativeStart(JNIEnv* env, jobject thiz, jobject activity)
{
    if (gRunning.load())
    {
        LOGI("nativeStart ignored (already running)");
        return;
    }
    env->GetJavaVM(&gVM);
    gActivity = env->NewGlobalRef(activity);

    jclass actClass = env->GetObjectClass(activity);
    g_onLobbyTouchMethod = env->GetMethodID(actClass, "onLobbyTouch", "(FFZZF)V");

    jclass localVr = env->FindClass("com/psmart/vrlib/VrActivity");
    if (localVr)
    {
        gVrClass = (jclass)env->NewGlobalRef(localVr);
        env->DeleteLocalRef(localVr);
    }
    else
    {
        env->ExceptionClear();
        gVrClass = nullptr;
        LOGE("VrActivity class not found");
    }

    env->DeleteLocalRef(actClass);

    gRunning.store(true);
    pthread_create(&gThread, nullptr, renderThread, nullptr);
    LOGI("nativeStart: render thread launched");
}

JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_pvr_MainActivity_nativeSurfaceChanged(JNIEnv* env, jobject thiz, jobject surface)
{
    ANativeWindow* win = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    LOGI("nativeSurfaceChanged window=%p", (void*)win);
    setWindow(win);
}

JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_pvr_MainActivity_nativeSurfaceDestroyed(JNIEnv* env, jobject thiz)
{
    LOGI("nativeSurfaceDestroyed");
    setWindow(nullptr);
}

JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_pvr_MainActivity_nativeStop(JNIEnv* env, jobject thiz)
{
    if (!gRunning.exchange(false)) return;
    pthread_join(gThread, nullptr);
    if (gActivity) { env->DeleteGlobalRef(gActivity); gActivity = nullptr; }
    if (gVrClass) { env->DeleteGlobalRef(gVrClass); gVrClass = nullptr; }
    LOGI("nativeStop done");
}

JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_pvr_MainActivity_nativeGetHeadData(JNIEnv* env, jobject thiz, jfloatArray out)
{
    if (!out) return;
    jfloat* arr = env->GetFloatArrayElements(out, nullptr);
    if (arr && env->GetArrayLength(out) >= 7)
    {
        std::lock_guard<std::mutex> lk(gStateMutex);
        arr[0] = gHeadOrient[0]; arr[1] = gHeadOrient[1];
        arr[2] = gHeadOrient[2]; arr[3] = gHeadOrient[3];
        arr[4] = gHeadPos[0]; arr[5] = gHeadPos[1]; arr[6] = gHeadPos[2];
    }
    env->ReleaseFloatArrayElements(out, arr, 0);
}

JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_pvr_MainActivity_nativeControllerState(
        JNIEnv* env, jobject thiz, jint hand, jint conn,
        jfloatArray sensor, jfloatArray angVel, jintArray keys)
{
    if (hand < 0 || hand > 1) return;

    std::lock_guard<std::mutex> lk(gStateMutex);
    controller_sample& cs = gControllers[hand];
    cs.connected = (conn == 1);

    if (sensor && env->GetArrayLength(sensor) >= 7)
    {
        jfloat* s = env->GetFloatArrayElements(sensor, nullptr);
        cs.orientation[0] = s[0]; cs.orientation[1] = s[1];
        cs.orientation[2] = s[2]; cs.orientation[3] = s[3];
        cs.position[0] = s[4]; cs.position[1] = s[5]; cs.position[2] = s[6];
        env->ReleaseFloatArrayElements(sensor, s, JNI_ABORT);
    }

    if (angVel && env->GetArrayLength(angVel) >= 3)
    {
        jfloat* a = env->GetFloatArrayElements(angVel, nullptr);
        cs.angular_velocity[0] = a[0]; cs.angular_velocity[1] = a[1]; cs.angular_velocity[2] = a[2];
        cs.has_angular_velocity = true;
        env->ReleaseFloatArrayElements(angVel, a, JNI_ABORT);
    }
    else
    {
        cs.has_angular_velocity = false;
    }

    if (keys && env->GetArrayLength(keys) >= 12)
    {
        jint* k = env->GetIntArrayElements(keys, nullptr);
        cs.touch[0] = k[0]; cs.touch[1] = k[1];
        cs.trigger = k[2];
        cs.grip = (k[3] != 0);
        cs.thumbstick_click = (k[4] != 0);
        cs.menu = (k[5] != 0);
        cs.button_a = (k[6] != 0);
        cs.button_b = (k[7] != 0);
        cs.battery = k[10];
        cs.home = (k[11] != 0);
        env->ReleaseIntArrayElements(keys, k, JNI_ABORT);
    }
}

JNIEXPORT jboolean JNICALL
Java_org_meumeu_wivrn_pvr_MainActivity_nativeDrainHaptic(JNIEnv* env, jobject thiz, jint hand, jfloatArray out)
{
    if (hand < 0 || hand > 1 || !out) return JNI_FALSE;
    std::lock_guard<std::mutex> lk(gHapticMutex);
    if (gPendingHaptics[hand].amplitude <= 0) return JNI_FALSE;
    jfloat* arr = env->GetFloatArrayElements(out, nullptr);
    if (arr && env->GetArrayLength(out) >= 2)
    {
        arr[0] = gPendingHaptics[hand].amplitude;
        arr[1] = gPendingHaptics[hand].duration_ms;
    }
    env->ReleaseFloatArrayElements(out, arr, 0);
    gPendingHaptics[hand].amplitude = 0;
    return JNI_TRUE;
}

JNIEXPORT jint JNICALL
Java_org_meumeu_wivrn_pvr_MainActivity_nativeGetTextureId(JNIEnv* env, jobject thiz)
{
    if (!gLobby.is_initialized()) return 0;
    return (jint)gLobby.get_external_texture();
}

JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_pvr_MainActivity_nativeSetSurfaceTexture(JNIEnv* env, jobject thiz, jobject st)
{
    if (!st)
    {
        gLobby.set_surface_texture(env, nullptr, nullptr);
        return;
    }
    jobject global = env->NewGlobalRef(st);
    jclass stClass = env->GetObjectClass(st);
    jmethodID updateMethod = env->GetMethodID(stClass, "updateTexImage", "()V");
    env->DeleteLocalRef(stClass);
    gLobby.set_surface_texture(env, global, updateMethod);
}

JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_pvr_MainActivity_nativeOnFrameAvailable(JNIEnv* env, jobject thiz)
{
    gLobby.on_frame_available();
}

JNIEXPORT jboolean JNICALL
Java_org_meumeu_wivrn_pvr_MainActivity_nativeReady(JNIEnv* env, jobject thiz)
{
    return gReady.load() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_pvr_MainActivity_nativeSetIpd(JNIEnv* env, jobject thiz, jfloat ipdMm)
{
    gIpd.store(ipdMm * 0.001f);
}

// Streaming stubs
JNIEXPORT void JNICALL Java_org_meumeu_wivrn_pvr_MainActivity_nativeConnectServer(JNIEnv*, jobject, jstring, jint, jboolean) {}
JNIEXPORT void JNICALL Java_org_meumeu_wivrn_pvr_MainActivity_nativeDisconnectServer(JNIEnv*, jobject) {}
JNIEXPORT void JNICALL Java_org_meumeu_wivrn_pvr_MainActivity_nativeSetPin(JNIEnv*, jobject, jstring) {}
JNIEXPORT void JNICALL Java_org_meumeu_wivrn_pvr_MainActivity_nativeSetBitrate(JNIEnv*, jobject, jint) {}
JNIEXPORT void JNICALL Java_org_meumeu_wivrn_pvr_MainActivity_nativeSetDynamicBitrate(JNIEnv*, jobject, jboolean) {}
JNIEXPORT void JNICALL Java_org_meumeu_wivrn_pvr_MainActivity_nativeRequestAppList(JNIEnv*, jobject) {}
JNIEXPORT void JNICALL Java_org_meumeu_wivrn_pvr_MainActivity_nativeStartApp(JNIEnv*, jobject, jstring) {}
JNIEXPORT void JNICALL Java_org_meumeu_wivrn_pvr_MainActivity_nativeRequestRunningApps(JNIEnv*, jobject) {}
JNIEXPORT void JNICALL Java_org_meumeu_wivrn_pvr_MainActivity_nativeSetActiveApp(JNIEnv*, jobject, jint) {}
JNIEXPORT void JNICALL Java_org_meumeu_wivrn_pvr_MainActivity_nativeStopApp(JNIEnv*, jobject, jint) {}
JNIEXPORT void JNICALL Java_org_meumeu_wivrn_pvr_MainActivity_nativeSetMicrophone(JNIEnv*, jobject, jboolean) {}
JNIEXPORT void JNICALL Java_org_meumeu_wivrn_pvr_MainActivity_nativeSetStreamResolution(JNIEnv*, jobject, jint, jint) {}
JNIEXPORT void JNICALL Java_org_meumeu_wivrn_pvr_MainActivity_nativeSetRenderResolution(JNIEnv*, jobject, jint, jint) {}

} // extern "C"
