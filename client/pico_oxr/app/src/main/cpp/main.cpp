#include <jni.h>
#include <android/log.h>
#include <android_native_app_glue.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <unistd.h>
#include <vector>
#include <cstring>
#include <cmath>
#include <ctime>
#include <mutex>
#include <atomic>
#include <algorithm>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/android_sink.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#define XR_USE_PLATFORM_ANDROID 1
#define XR_USE_GRAPHICS_API_OPENGL_ES 1
#include "openxr/openxr.h"
#include "openxr/openxr_platform.h"

#include "streaming/streaming_client.h"
#include "streaming/oxr_blit.h"
#include "pico_stutter.h"

#define LOG_TAG "WiVRn-OXR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define CHK_XR(call, msg) do { \
    XrResult _r = (call); \
    if (_r != XR_SUCCESS) { \
        LOGE("%s failed: %d", msg, _r); \
    } \
} while (0)

#define NUM_EYES 2

// ---------------------------------------------------------------------------
// Shared state between render thread and JNI controller poll thread
// ---------------------------------------------------------------------------

static std::mutex g_state_mutex;
static float g_head_orient[4] = {0, 0, 0, 1};
static float g_head_pos[3] = {0, 0, 0};
static controller_sample g_controllers[2];

struct AppState;
static AppState* g_app = nullptr;
static JavaVM* g_jvm = nullptr;
static jobject g_activity = nullptr;
static PFN_xrGetConfigPICO g_pfnXrGetConfigPICO = nullptr;
static PFN_xrResetSensorPICO g_pfnXrResetSensorPICO = nullptr;
static PFN_xrInvokeFunctionsPICO g_pfnXrInvokeFunctionsPICO = nullptr;
static bool g_hasPicoSessionBegin = false;
static jclass g_activityClass = nullptr;

// ---------------------------------------------------------------------------
// JNI: called from Java controller poll thread
// ---------------------------------------------------------------------------

extern "C" {

JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_oxr_StreamingActivity_nativeGetHeadData(JNIEnv * env, jobject thiz, jfloatArray out)
{
    if (!out || env->GetArrayLength(out) < 7)
        return;
    std::lock_guard lock(g_state_mutex);
    float buf[7] = {
        g_head_orient[0], g_head_orient[1], g_head_orient[2], g_head_orient[3],
        g_head_pos[0], g_head_pos[1], g_head_pos[2]};
    env->SetFloatArrayRegion(out, 0, 7, buf);
}

JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_oxr_StreamingActivity_nativeControllerState(
    JNIEnv * env, jobject thiz, jint hand, jint conn,
    jfloatArray sensor, jfloatArray angVel, jintArray keys)
{
    if (hand < 0 || hand > 1)
        return;

    float sensor_buf[7] = {0};
    int keys_buf[12] = {0};
    bool has_sensor = false, has_keys = false;

    if (sensor && env->GetArrayLength(sensor) >= 7)
    {
        env->GetFloatArrayRegion(sensor, 0, 7, sensor_buf);
        has_sensor = true;
    }
    if (keys && env->GetArrayLength(keys) >= 12)
    {
        env->GetIntArrayRegion(keys, 0, 12, keys_buf);
        has_keys = true;
    }

    {
        std::lock_guard lock(g_state_mutex);
        auto & c = g_controllers[hand];

        if (conn != 1)
        {
            c.connected = false;
        }
        else
        {
            c.connected = true;
            if (has_sensor)
            {
                c.orientation[0] = sensor_buf[0];
                c.orientation[1] = sensor_buf[1];
                c.orientation[2] = sensor_buf[2];
                c.orientation[3] = sensor_buf[3];
                c.position[0] = sensor_buf[4];
                c.position[1] = sensor_buf[5];
                c.position[2] = sensor_buf[6];
            }
            if (has_keys)
            {
                c.trigger = keys_buf[8];
                c.touch[0] = keys_buf[0];
                c.touch[1] = keys_buf[1];
                c.battery = keys_buf[10];
                c.button_a = keys_buf[6] != 0;
                c.button_b = keys_buf[7] != 0;
                c.grip = keys_buf[3] != 0;
                c.thumbstick_click = keys_buf[4] != 0;
                c.menu = keys_buf[5] != 0;
                c.home = keys_buf[11] != 0;

                static bool prev_a[2] = {false, false};
                static bool prev_b[2] = {false, false};
                if (c.button_a != prev_a[hand] || c.button_b != prev_b[hand])
                {
                    LOGI("BTNCHG hand=%d A=%d->%d B=%d->%d keys[6]=%d keys[7]=%d",
                         hand, prev_a[hand], c.button_a, prev_b[hand], c.button_b,
                         keys_buf[6], keys_buf[7]);
                    prev_a[hand] = c.button_a;
                    prev_b[hand] = c.button_b;
                }
            }

            static int jni_log_count = 0;
            if (has_sensor && jni_log_count++ % 120 == 0)
            {
                LOGI("JNI controller %d: raw q=(%.4f,%.4f,%.4f,%.4f) pos_mm=(%.1f,%.1f,%.1f) trigger=%d",
                     hand,
                     sensor_buf[0], sensor_buf[1], sensor_buf[2], sensor_buf[3],
                     sensor_buf[4], sensor_buf[5], sensor_buf[6],
                     keys_buf[2]);
            }
        }
    }
}

} // extern "C"

// ---------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------

struct SwapchainState {
    XrSwapchain handle = XR_NULL_HANDLE;
    int32_t width = 0;
    int32_t height = 0;
    std::vector<XrSwapchainImageOpenGLESKHR> images;
};

struct AppState {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLConfig config = nullptr;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;

    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE;
    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    bool sessionRunning = false;

    SwapchainState swapchains[NUM_EYES];
    XrViewConfigurationView viewConfigs[NUM_EYES] = {};

    GLuint fbo = 0;
    GLuint depth_rbo[NUM_EYES] = {};
    int depth_rbo_w[NUM_EYES] = {};
    int depth_rbo_h[NUM_EYES] = {};

    streaming_client stream;
    oxr_blit blit;

    bool shouldExit = false;
};

// ---------------------------------------------------------------------------
// JNI: streaming connection
// ---------------------------------------------------------------------------

extern "C" {

JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_oxr_StreamingActivity_nativeConnectServer(JNIEnv * env, jobject thiz, jstring host, jint port, jboolean tcpOnly)
{
    if (!g_app) {
        LOGE("nativeConnectServer: g_app not ready");
        return;
    }
    const char * h = env->GetStringUTFChars(host, nullptr);
    LOGI("nativeConnectServer: %s:%d tcp=%d", h, port, tcpOnly);
    g_app->stream.server_host = h;
    g_app->stream.server_port = port;
    g_app->stream.tcp_only = tcpOnly;
    env->ReleaseStringUTFChars(host, h);
    g_app->stream.try_connect();
}

JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_oxr_StreamingActivity_nativeDisconnectServer(JNIEnv * env, jobject thiz)
{
    if (!g_app) return;
    g_app->stream.shutdown = true;
    if (g_app->stream.session)
        g_app->stream.session.reset();
}

JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_oxr_StreamingActivity_nativeSetPin(JNIEnv * env, jobject thiz, jstring pin)
{
    if (!g_app) return;
    const char * p = env->GetStringUTFChars(pin, nullptr);
    g_app->stream.pairing_pin = p;
    env->ReleaseStringUTFChars(pin, p);
}

JNIEXPORT jboolean JNICALL
Java_org_meumeu_wivrn_oxr_StreamingActivity_nativeReady(JNIEnv * env, jobject thiz)
{
    return g_app != nullptr ? JNI_TRUE : JNI_FALSE;
}

} // extern "C"

// ---------------------------------------------------------------------------
// EGL: windowless context (pbuffer surface) for OpenXR rendering
// ---------------------------------------------------------------------------

static bool egl_init(AppState* app) {
    app->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (app->display == EGL_NO_DISPLAY) {
        LOGE("eglGetDisplay failed");
        return false;
    }

    EGLint major, minor;
    if (!eglInitialize(app->display, &major, &minor)) {
        LOGE("eglInitialize failed: 0x%x", eglGetError());
        return false;
    }
    LOGI("EGL %d.%d", major, minor);

    const EGLint configAttribs[] = {
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_NONE
    };

    EGLint numConfigs = 0;
    if (!eglChooseConfig(app->display, configAttribs, &app->config, 1, &numConfigs) || numConfigs == 0) {
        LOGE("eglChooseConfig failed: 0x%x", eglGetError());
        return false;
    }

    const EGLint ctxAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    app->context = eglCreateContext(app->display, app->config, EGL_NO_CONTEXT, ctxAttribs);
    if (app->context == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed: 0x%x", eglGetError());
        return false;
    }

    const EGLint pbAttribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    app->surface = eglCreatePbufferSurface(app->display, app->config, pbAttribs);
    if (app->surface == EGL_NO_SURFACE) {
        LOGE("eglCreatePbufferSurface failed: 0x%x", eglGetError());
        return false;
    }

    if (!eglMakeCurrent(app->display, app->surface, app->surface, app->context)) {
        LOGE("eglMakeCurrent failed: 0x%x", eglGetError());
        return false;
    }

    LOGI("EGL context created (GLES3, windowless)");
    return true;
}

static void egl_shutdown(AppState* app) {
    if (app->display != EGL_NO_DISPLAY) {
        eglMakeCurrent(app->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (app->surface != EGL_NO_SURFACE) {
            eglDestroySurface(app->display, app->surface);
            app->surface = EGL_NO_SURFACE;
        }
        if (app->context != EGL_NO_CONTEXT) {
            eglDestroyContext(app->display, app->context);
            app->context = EGL_NO_CONTEXT;
        }
        eglTerminate(app->display);
        app->display = EGL_NO_DISPLAY;
    }
}

// ---------------------------------------------------------------------------
// OpenXR initialization
// ---------------------------------------------------------------------------

static bool openxr_init(struct android_app* androidApp, AppState* app) {
    PFN_xrInitializeLoaderKHR initializeLoader = nullptr;
    if (XR_SUCCEEDED(
            xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                                  (PFN_xrVoidFunction*)(&initializeLoader)))) {
        XrLoaderInitInfoAndroidKHR loaderInitInfo;
        memset(&loaderInitInfo, 0, sizeof(loaderInitInfo));
        loaderInitInfo.type = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR;
        loaderInitInfo.next = nullptr;
        loaderInitInfo.applicationVM = androidApp->activity->vm;
        loaderInitInfo.applicationContext = androidApp->activity->clazz;
        CHK_XR(initializeLoader((const XrLoaderInitInfoBaseHeaderKHR*)&loaderInitInfo),
               "xrInitializeLoaderKHR");
        LOGI("xrInitializeLoaderKHR succeeded");
    }

    uint32_t extCount = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    std::vector<XrExtensionProperties> exts(extCount);
    for (auto& e : exts) e.type = XR_TYPE_EXTENSION_PROPERTIES;
    xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data());

    bool hasGLES = false, hasAndroidCI = false;
    bool hasPicoConfigs = false, hasPicoSessionBegin = false, hasPicoResetSensor = false, hasPicoBoundary = false;
    for (const auto& e : exts) {
        LOGI("Extension: %s", e.extensionName);
        if (strcmp(e.extensionName, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME) == 0)
            hasGLES = true;
        if (strcmp(e.extensionName, XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME) == 0)
            hasAndroidCI = true;
        if (strcmp(e.extensionName, XR_PICO_CONFIGS_EXT_EXTENSION_NAME) == 0)
            hasPicoConfigs = true;
        if (strcmp(e.extensionName, XR_PICO_SESSION_BEGIN_INFO_EXT_ENABLE_EXTENSION_NAME) == 0)
            hasPicoSessionBegin = true;
        if (strcmp(e.extensionName, XR_PICO_RESET_SENSOR_EXTENSION_NAME) == 0)
            hasPicoResetSensor = true;
        if (strcmp(e.extensionName, XR_PICO_BOUNDARY_EXT_EXTENSION_NAME) == 0)
            hasPicoBoundary = true;
    }
    if (!hasGLES) {
        LOGE("XR_KHR_opengl_es_enable not supported");
        return false;
    }

    std::vector<const char*> enabledExtsVec = {
        XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
    };
    if (hasPicoConfigs)
        enabledExtsVec.push_back(XR_PICO_CONFIGS_EXT_EXTENSION_NAME);
    if (hasPicoSessionBegin)
        enabledExtsVec.push_back(XR_PICO_SESSION_BEGIN_INFO_EXT_ENABLE_EXTENSION_NAME);
    if (hasPicoResetSensor)
        enabledExtsVec.push_back(XR_PICO_RESET_SENSOR_EXTENSION_NAME);
    if (hasPicoBoundary)
        enabledExtsVec.push_back(XR_PICO_BOUNDARY_EXT_EXTENSION_NAME);

    XrInstanceCreateInfoAndroidKHR androidCI = {};
    androidCI.type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR;
    androidCI.next = nullptr;
    androidCI.applicationVM = androidApp->activity->vm;
    androidCI.applicationActivity = androidApp->activity->clazz;

    XrInstanceCreateInfo ci = {};
    ci.type = XR_TYPE_INSTANCE_CREATE_INFO;
    ci.next = &androidCI;
    ci.enabledExtensionCount = (uint32_t)enabledExtsVec.size();
    ci.enabledExtensionNames = enabledExtsVec.data();
    strcpy(ci.applicationInfo.applicationName, "WiVRn OXR");
    ci.applicationInfo.applicationVersion = 1;
    strcpy(ci.applicationInfo.engineName, "No Engine");
    ci.applicationInfo.engineVersion = 0;
    ci.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

    XrResult r = xrCreateInstance(&ci, &app->instance);
    if (r != XR_SUCCESS) {
        LOGE("xrCreateInstance failed: %d", r);
        return false;
    }

    PFN_xrGetConfigPICO pfnXrGetConfigPICO = nullptr;
    if (hasPicoConfigs) {
        xrGetInstanceProcAddr(app->instance, "xrGetConfigPICO",
                              reinterpret_cast<PFN_xrVoidFunction*>(&pfnXrGetConfigPICO));
        g_pfnXrGetConfigPICO = pfnXrGetConfigPICO;
    }
    if (hasPicoResetSensor) {
        xrGetInstanceProcAddr(app->instance, "xrResetSensorPICO",
                              reinterpret_cast<PFN_xrVoidFunction*>(&g_pfnXrResetSensorPICO));
        LOGI("xrResetSensorPICO %s", g_pfnXrResetSensorPICO ? "loaded" : "failed to load");
    }
    if (hasPicoBoundary) {
        xrGetInstanceProcAddr(app->instance, "xrInvokeFunctionsPICO",
                              reinterpret_cast<PFN_xrVoidFunction*>(&g_pfnXrInvokeFunctionsPICO));
        LOGI("xrInvokeFunctionsPICO %s", g_pfnXrInvokeFunctionsPICO ? "loaded" : "failed to load");
    }
    g_hasPicoSessionBegin = hasPicoSessionBegin;

    XrInstanceProperties ip = {};
    ip.type = XR_TYPE_INSTANCE_PROPERTIES;
    xrGetInstanceProperties(app->instance, &ip);
    LOGI("OpenXR runtime: %s (v%d.%d.%d)", ip.runtimeName,
         XR_VERSION_MAJOR(ip.runtimeVersion),
         XR_VERSION_MINOR(ip.runtimeVersion),
         XR_VERSION_PATCH(ip.runtimeVersion));

    XrSystemGetInfo si = {};
    si.type = XR_TYPE_SYSTEM_GET_INFO;
    si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    r = xrGetSystem(app->instance, &si, &app->systemId);
    if (r != XR_SUCCESS) {
        LOGE("xrGetSystem failed: %d", r);
        return false;
    }

    XrSystemProperties sp = {};
    sp.type = XR_TYPE_SYSTEM_PROPERTIES;
    xrGetSystemProperties(app->instance, app->systemId, &sp);
    LOGI("System: %s, maxLayers=%u, maxSwapchainW=%u, maxSwapchainH=%u",
         sp.systemName, sp.graphicsProperties.maxLayerCount,
         sp.graphicsProperties.maxSwapchainImageWidth,
         sp.graphicsProperties.maxSwapchainImageHeight);

    PFN_xrGetOpenGLESGraphicsRequirementsKHR pfnGetGLESReqs = nullptr;
    xrGetInstanceProcAddr(app->instance, "xrGetOpenGLESGraphicsRequirementsKHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&pfnGetGLESReqs));
    if (!pfnGetGLESReqs) {
        LOGE("Failed to load xrGetOpenGLESGraphicsRequirementsKHR");
        return false;
    }

    XrGraphicsRequirementsOpenGLESKHR req = {};
    req.type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR;
    r = pfnGetGLESReqs(app->instance, app->systemId, &req);
    if (r != XR_SUCCESS) {
        LOGE("xrGetOpenGLESGraphicsRequirementsKHR failed: %d", r);
        return false;
    }
    LOGI("GLES requirements: min=%d.%d, max=%d.%d",
         XR_VERSION_MAJOR(req.minApiVersionSupported),
         XR_VERSION_MINOR(req.minApiVersionSupported),
         XR_VERSION_MAJOR(req.maxApiVersionSupported),
         XR_VERSION_MINOR(req.maxApiVersionSupported));

    return true;
}

static bool openxr_create_session(AppState* app) {
    XrGraphicsBindingOpenGLESAndroidKHR binding = {};
    binding.type = XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR;
    binding.display = app->display;
    binding.config = app->config;
    binding.context = app->context;

    XrSessionCreateInfo sci = {};
    sci.type = XR_TYPE_SESSION_CREATE_INFO;
    sci.next = &binding;
    sci.systemId = app->systemId;

    XrResult r = xrCreateSession(app->instance, &sci, &app->session);
    if (r != XR_SUCCESS) {
        LOGE("xrCreateSession failed: %d", r);
        return false;
    }
    LOGI("Session created");

    if (g_pfnXrInvokeFunctionsPICO) {
        void* dummy_in = nullptr;
        void* dummy_out = nullptr;
        XrResult br;
        br = g_pfnXrInvokeFunctionsPICO(app->session, XR_DISABLE_BOUNDARY, dummy_in, 0, &dummy_out, 0);
        LOGI("XR_DISABLE_BOUNDARY result: %d", br);
        br = g_pfnXrInvokeFunctionsPICO(app->session, XR_SHUTDOWN_SDK_GUARDIANSYSTEM, dummy_in, 0, &dummy_out, 0);
        LOGI("XR_SHUTDOWN_SDK_GUARDIANSYSTEM result: %d", br);
        if (br == XR_SUCCESS) {
            LOGI("Boundary disabled");
        }
    }

    if (g_pfnXrGetConfigPICO) {
        float targetFps = 0, refreshRate = 0;
        g_pfnXrGetConfigPICO(app->session, TARGET_FRAME_RATE, &targetFps);
        g_pfnXrGetConfigPICO(app->session, DISPLAY_REFRESH_RATE, &refreshRate);
        LOGI("PICO config: targetFPS=%.1f, refreshRate=%.1f", targetFps, refreshRate);
    }

    XrReferenceSpaceCreateInfo rsci = {};
    rsci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1.f;
    rsci.poseInReferenceSpace.position.y = 0.f;

    r = xrCreateReferenceSpace(app->session, &rsci, &app->localSpace);
    if (r != XR_SUCCESS) {
        LOGE("xrCreateReferenceSpace failed: %d", r);
        return false;
    }

    for (uint32_t i = 0; i < NUM_EYES; i++)
        app->viewConfigs[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;

    uint32_t actualCount = NUM_EYES;
    r = xrEnumerateViewConfigurationViews(
        app->instance, app->systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        NUM_EYES, &actualCount, app->viewConfigs);
    if (r != XR_SUCCESS) {
        LOGE("xrEnumerateViewConfigurationViews failed: %d", r);
        return false;
    }
    for (uint32_t i = 0; i < NUM_EYES; i++) {
        LOGI("Eye %u: recommended=%ux%u max=%ux%u samples=%u",
             i, app->viewConfigs[i].recommendedImageRectWidth,
             app->viewConfigs[i].recommendedImageRectHeight,
             app->viewConfigs[i].maxImageRectWidth,
             app->viewConfigs[i].maxImageRectHeight,
             app->viewConfigs[i].recommendedSwapchainSampleCount);
    }

    for (uint32_t eye = 0; eye < NUM_EYES; eye++) {
        XrSwapchainCreateInfo swci = {};
        swci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
        swci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        swci.format = GL_RGBA8;
        swci.width = 1664;
        swci.height = 1664;
        swci.sampleCount = app->viewConfigs[eye].recommendedSwapchainSampleCount;
        swci.faceCount = 1;
        swci.arraySize = 1;
        swci.mipCount = 1;

        r = xrCreateSwapchain(app->session, &swci, &app->swapchains[eye].handle);
        if (r != XR_SUCCESS) {
            LOGE("xrCreateSwapchain eye %u failed: %d", eye, r);
            return false;
        }
        app->swapchains[eye].width = swci.width;
        app->swapchains[eye].height = swci.height;

        uint32_t imgCount = 0;
        xrEnumerateSwapchainImages(app->swapchains[eye].handle, 0, &imgCount, nullptr);
        app->swapchains[eye].images.resize(imgCount);
        for (auto& img : app->swapchains[eye].images)
            img.type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        xrEnumerateSwapchainImages(app->swapchains[eye].handle, imgCount, &imgCount,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(app->swapchains[eye].images.data()));
        LOGI("Eye %u swapchain: %ux%u, %u images", eye, swci.width, swci.height, imgCount);
    }

    return true;
}

static void openxr_destroy_session(AppState* app) {
    for (uint32_t eye = 0; eye < NUM_EYES; eye++) {
        if (app->swapchains[eye].handle) {
            xrDestroySwapchain(app->swapchains[eye].handle);
            app->swapchains[eye].handle = XR_NULL_HANDLE;
            app->swapchains[eye].images.clear();
        }
    }
    if (app->localSpace) {
        xrDestroySpace(app->localSpace);
        app->localSpace = XR_NULL_HANDLE;
    }
    if (app->session) {
        xrDestroySession(app->session);
        app->session = XR_NULL_HANDLE;
    }
}

// ---------------------------------------------------------------------------
// Event handling
// ---------------------------------------------------------------------------

static void poll_events(AppState* app) {
    XrEventDataBuffer edb = {};
    edb.type = XR_TYPE_EVENT_DATA_BUFFER;
    while (xrPollEvent(app->instance, &edb) == XR_SUCCESS) {
        switch (edb.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            auto* evt = reinterpret_cast<XrEventDataSessionStateChanged*>(&edb);
            XrSessionState old = app->sessionState;
            app->sessionState = evt->state;
            LOGI("Session state: %d -> %d", old, evt->state);

            if (evt->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi = {};
                bi.type = XR_TYPE_SESSION_BEGIN_INFO;
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                XrResult r = xrBeginSession(app->session, &bi);
                if (r == XR_SUCCESS) {
                    app->sessionRunning = true;
                    LOGI("Session begun");
                } else {
                    LOGE("xrBeginSession failed: %d", r);
                }
            } else if (evt->state == XR_SESSION_STATE_STOPPING) {
                app->sessionRunning = false;
                xrEndSession(app->session);
                LOGI("Session ended");
            } else if (evt->state == XR_SESSION_STATE_EXITING) {
                app->shouldExit = true;
                LOGI("Session exiting");
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
            auto* evt = reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(&edb);
            LOGI("Reference space change pending (space=%d)", evt->referenceSpaceType);
            if (g_pfnXrResetSensorPICO && app->session) {
                XrResult rr = g_pfnXrResetSensorPICO(app->session, XR_RESET_ALL);
                LOGI("xrResetSensorPICO result: %d", rr);
            }
            if (app->localSpace) {
                xrDestroySpace(app->localSpace);
                app->localSpace = XR_NULL_HANDLE;
            }
            XrReferenceSpaceCreateInfo rsci = {};
            rsci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
            rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
            rsci.poseInReferenceSpace.orientation.w = 1.f;
            rsci.poseInReferenceSpace.position.y = 0.f;
            XrResult rr = xrCreateReferenceSpace(app->session, &rsci, &app->localSpace);
            if (rr != XR_SUCCESS)
                LOGE("xrCreateReferenceSpace after recenter failed: %d", rr);
            else
                LOGI("Reference space recreated after recenter");
            break;
        }
        default:
            break;
        }
        edb.type = XR_TYPE_EVENT_DATA_BUFFER;
    }
}

// ---------------------------------------------------------------------------
// Render frame
// ---------------------------------------------------------------------------

static int g_frame_count = 0;
static struct timespec g_fps_start = {};
static float g_wait_total = 0, g_render_total = 0, g_end_total = 0;
static float g_begin_total = 0, g_locate_total = 0, g_sc_wait_total = 0, g_gl_draw_total = 0, g_sc_rel_total = 0;
static float g_mid_total = 0, g_sc_acq_total = 0;
static float g_math_total = 0, g_flush_total = 0;
static float g_flush_direct_total = 0;
static float g_inter_total = 0;
static struct timespec g_prev_end = {};

static double ts_diff(const struct timespec& a, const struct timespec& b) {
    return (a.tv_sec - b.tv_sec) + (a.tv_nsec - b.tv_nsec) * 1e-9;
}

static void render_frame(AppState* app) {
    struct timespec t0, t1, t2, t3;
    struct timespec ta, tb, tc, td, te;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    if (g_prev_end.tv_sec != 0) {
        g_inter_total += ts_diff(t0, g_prev_end);
    }

    XrFrameWaitInfo fw = {};
    fw.type = XR_TYPE_FRAME_WAIT_INFO;
    XrFrameState fs = {};
    fs.type = XR_TYPE_FRAME_STATE;
    XrResult r = xrWaitFrame(app->session, &fw, &fs);
    if (r != XR_SUCCESS) {
        LOGE("xrWaitFrame failed: %d", r);
        return;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    XrFrameBeginInfo fb = {};
    fb.type = XR_TYPE_FRAME_BEGIN_INFO;
    r = xrBeginFrame(app->session, &fb);
    if (r != XR_SUCCESS) {
        LOGE("xrBeginFrame failed: %d", r);
        return;
    }
    clock_gettime(CLOCK_MONOTONIC, &ta);

    XrViewState vstate = {};
    vstate.type = XR_TYPE_VIEW_STATE;

    XrViewLocateInfo vli = {};
    vli.type = XR_TYPE_VIEW_LOCATE_INFO;
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vli.displayTime = fs.predictedDisplayTime;
    vli.space = app->localSpace;

    XrView views[NUM_EYES] = {};
    for (auto& v : views) v.type = XR_TYPE_VIEW;

    uint32_t viewCount = 0;
    r = xrLocateViews(app->session, &vli, &vstate, NUM_EYES, &viewCount, views);
    clock_gettime(CLOCK_MONOTONIC, &tb);
    if (r != XR_SUCCESS || viewCount != NUM_EYES) {
        LOGE("xrLocateViews failed: %d (count=%u)", r, viewCount);
        xrEndFrame(app->session, nullptr);
        return;
    }

    float head_orient[4] = {
        views[0].pose.orientation.x,
        views[0].pose.orientation.y,
        views[0].pose.orientation.z,
        views[0].pose.orientation.w
    };
    float head_pos[3] = {
        (views[0].pose.position.x + views[1].pose.position.x) * 0.5f,
        (views[0].pose.position.y + views[1].pose.position.y) * 0.5f,
        (views[0].pose.position.z + views[1].pose.position.z) * 0.5f
    };
    float dx = views[0].pose.position.x - views[1].pose.position.x;
    float dy = views[0].pose.position.y - views[1].pose.position.y;
    float dz = views[0].pose.position.z - views[1].pose.position.z;
    float ipd = sqrtf(dx*dx + dy*dy + dz*dz);

    app->stream.tracker.set_head_pose(head_orient, head_pos);

    // Pico Neo 2 has a square ~101 degree per-eye FOV.
    // The OpenXR runtime reports incorrect/wider values that cause zoom-in.
    // Use the known-correct value, same as the PVR client.
    constexpr float k_fov_half = 101.0f * 0.5f * 0.01745329252f;
    app->stream.eye_fov[0] = {-k_fov_half, k_fov_half, k_fov_half, -k_fov_half};
    app->stream.eye_fov[1] = {-k_fov_half, k_fov_half, k_fov_half, -k_fov_half};

    static int hmd_log_count = 0;
    if (hmd_log_count++ % 120 == 0)
    {
        LOGI("RENDER HMD: q=(%.4f,%.4f,%.4f,%.4f) pos_m=(%.4f,%.4f,%.4f) ipd=%.4f",
             head_orient[0], head_orient[1], head_orient[2], head_orient[3],
             head_pos[0], head_pos[1], head_pos[2], ipd);
    }

    static int fov_log_count = 0;
    if (fov_log_count++ < 5)
        LOGI("OpenXR FOV: L=[%.4f,%.4f] U=[%.4f,%.4f] | R: L=[%.4f,%.4f] U=[%.4f,%.4f]",
             views[0].fov.angleLeft, views[0].fov.angleRight,
             views[0].fov.angleUp, views[0].fov.angleDown,
             views[1].fov.angleLeft, views[1].fov.angleRight,
             views[1].fov.angleUp, views[1].fov.angleDown);

    struct timespec tm1;
    clock_gettime(CLOCK_MONOTONIC, &tm1);

    {
        std::lock_guard lock(g_state_mutex);
        memcpy(g_head_orient, head_orient, sizeof(g_head_orient));
        memcpy(g_head_pos, head_pos, sizeof(g_head_pos));
    }

    controller_sample cs[2];
    {
        std::lock_guard lock(g_state_mutex);
        cs[0] = g_controllers[0];
        cs[1] = g_controllers[1];
    }

    static bool prev_home[2] = {false, false};
    static uint64_t home_press_ts[2] = {0, 0};
    for (int h = 0; h < 2; h++)
    {
        bool home_now = cs[h].connected && cs[h].home;
        uint64_t now_ns = (uint64_t)fs.predictedDisplayTime;
        if (home_now && !prev_home[h])
        {
            home_press_ts[h] = now_ns;
            LOGI("controller %d home button pressed", h);
        }
        else if (home_now && prev_home[h] && home_press_ts[h] > 0)
        {
            uint64_t held_ns = now_ns - home_press_ts[h];
            if (held_ns > 500000000ULL && held_ns < 510000000ULL)
                LOGI("controller %d home held for %llums, will recenter on release", h, held_ns / 1000000ULL);
        }
        else if (!home_now && prev_home[h])
        {
            if (home_press_ts[h] > 0 && now_ns - home_press_ts[h] > 500000000ULL)
            {
                uint64_t held_ns = now_ns - home_press_ts[h];
                LOGI("recenter triggered by controller %d home button long press (%llums)", h, held_ns / 1000000ULL);
                if (g_pfnXrResetSensorPICO && app->session)
                {
                    XrResult rr = g_pfnXrResetSensorPICO(app->session, XR_RESET_ALL);
                    LOGI("xrResetSensorPICO result: %d", rr);
                }
                if (app->localSpace)
                {
                    xrDestroySpace(app->localSpace);
                    app->localSpace = XR_NULL_HANDLE;
                    LOGI("old localSpace destroyed");
                }
                XrReferenceSpaceCreateInfo rsci = {};
                rsci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
                rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
                rsci.poseInReferenceSpace.orientation.w = 1.f;
                rsci.poseInReferenceSpace.position.y = 0.f;
                XrResult rr = xrCreateReferenceSpace(app->session, &rsci, &app->localSpace);
                if (rr != XR_SUCCESS)
                    LOGE("xrCreateReferenceSpace after recenter failed: %d", rr);
                else
                    LOGI("Reference space recreated after recenter");
            }
            else if (home_press_ts[h] > 0)
            {
                LOGI("controller %d home released too quickly (%llums), no recenter", h, (now_ns - home_press_ts[h]) / 1000000ULL);
            }
            home_press_ts[h] = 0;
        }
        prev_home[h] = home_now;
    }

    for (int h = 0; h < 2; h++)
    {
        if (cs[h].connected)
        {
            app->stream.tracker.update_controller(
                h, cs[h].orientation, cs[h].position,
                cs[h].trigger, cs[h].touch, cs[h].battery,
                cs[h].button_a, cs[h].button_b, cs[h].grip,
                cs[h].thumbstick_click, cs[h].menu);

            static int fwd_log_count = 0;
            if (fwd_log_count++ % 120 == 0)
            {
                LOGI("RENDER fwd controller %d: q=(%.4f,%.4f,%.4f,%.4f) pos_mm=(%.1f,%.1f,%.1f)",
                     h,
                     cs[h].orientation[0], cs[h].orientation[1],
                     cs[h].orientation[2], cs[h].orientation[3],
                     cs[h].position[0], cs[h].position[1], cs[h].position[2]);
            }
        }
        else
        {
            app->stream.tracker.clear_controller(h);
        }
    }

    struct timespec tm2;
    clock_gettime(CLOCK_MONOTONIC, &tm2);

    struct timespec tfl0, tfl1;
    clock_gettime(CLOCK_MONOTONIC, &tfl0);
    clock_gettime(CLOCK_MONOTONIC, &tfl1);

    struct timespec tf;
    clock_gettime(CLOCK_MONOTONIC, &tf);

    XrCompositionLayerProjection layers[1];
    XrCompositionLayerProjectionView layerViews[NUM_EYES];

    float sc_wait_ms = 0, gl_draw_ms = 0, sc_rel_ms = 0;

    // Common frame selection — match upstream WiVRn's common_frame() approach.
    // Find a frame index present in both eyes' rolling buffers, closest to predictedDisplayTime.
    // Falls back to per-eye latest if no common frame exists.
    static std::shared_ptr<pico_decoded_frame> render_frames[3];
    static bool was_streaming = false;

    if (app->stream.streaming.load())
    {
        was_streaming = true;
        std::lock_guard lock(app->stream.decoded_frame_mutex);

        auto &buf0 = app->stream.decoded_frame_buffers[0];
        auto &buf1 = app->stream.decoded_frame_buffers[1];

        // Collect valid frame indices from each buffer
        uint64_t best_common_idx = UINT64_MAX;
        int64_t best_common_diff = INT64_MAX;

        for (auto &s0 : buf0)
        {
            if (!s0 || !s0->valid) continue;
            for (auto &s1 : buf1)
            {
                if (!s1 || !s1->valid) continue;
                if (s0->frame_index == s1->frame_index)
                {
                    int64_t diff = (int64_t)s0->display_time - (int64_t)fs.predictedDisplayTime;
                    if (diff < 0) diff = -diff;
                    if (diff < best_common_diff)
                    {
                        best_common_diff = diff;
                        best_common_idx = s0->frame_index;
                    }
                }
            }
        }

        if (best_common_idx != UINT64_MAX)
        {
            render_frames[0] = app->stream.get_frame(best_common_idx, 0);
            render_frames[1] = app->stream.get_frame(best_common_idx, 1);
        }
        else
        {
            // No common frame — fall back to latest valid per eye
            render_frames[0].reset();
            render_frames[1].reset();
            for (auto &s : buf0)
                if (s && s->valid && (!render_frames[0] || s->frame_index > render_frames[0]->frame_index))
                    render_frames[0] = s;
            for (auto &s : buf1)
                if (s && s->valid && (!render_frames[1] || s->frame_index > render_frames[1]->frame_index))
                    render_frames[1] = s;
        }
    }
    else if (was_streaming)
    {
        render_frames[0].reset();
        render_frames[1].reset();
        render_frames[2].reset();
        was_streaming = false;
    }

    for (uint32_t eye = 0; eye < NUM_EYES; eye++) {
        uint32_t imgIndex = 0;
        XrSwapchainImageAcquireInfo ai = {};
        ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
        r = xrAcquireSwapchainImage(app->swapchains[eye].handle, &ai, &imgIndex);
        if (r != XR_SUCCESS) {
            LOGE("xrAcquireSwapchainImage eye %u failed: %d", eye, r);
            continue;
        }

        XrSwapchainImageWaitInfo wi = {};
        wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
        wi.timeout = XR_INFINITE_DURATION;
        clock_gettime(CLOCK_MONOTONIC, &tc);
        r = xrWaitSwapchainImage(app->swapchains[eye].handle, &wi);
        clock_gettime(CLOCK_MONOTONIC, &td);
        sc_wait_ms += ts_diff(td, tc) * 1000.0f;
        if (r != XR_SUCCESS) {
            LOGE("xrWaitSwapchainImage eye %u failed: %d", eye, r);
            continue;
        }

        GLuint glImg = app->swapchains[eye].images[imgIndex].image;
        int32_t w = app->swapchains[eye].width;
        int32_t h = app->swapchains[eye].height;

        glBindFramebuffer(GL_FRAMEBUFFER, app->fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, glImg, 0);

        if (app->depth_rbo_w[eye] != w || app->depth_rbo_h[eye] != h) {
            glBindRenderbuffer(GL_RENDERBUFFER, app->depth_rbo[eye]);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
            app->depth_rbo_w[eye] = w;
            app->depth_rbo_h[eye] = h;
        }
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, app->depth_rbo[eye]);

        GLenum fbStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);

        std::shared_ptr<pico_decoded_frame> decoded;
        if (app->stream.streaming.load())
        {
            decoded = render_frames[eye];
        }

        if (fbStatus != GL_FRAMEBUFFER_COMPLETE) {
            LOGE("Framebuffer incomplete: 0x%x (eye %u)", fbStatus, eye);
        } else if (app->stream.streaming.load()) {
            static int blit_log_count = 0;
            if (eye == 0 && (blit_log_count++ % 300 == 0)) LOGI("STREAMING: blit path active");

            if (eye == 0) {
                static int sync_log_count = 0;
                bool has_both = render_frames[0] && render_frames[0]->valid && render_frames[1] && render_frames[1]->valid;
                if (has_both)
                {
                    uint64_t fi0 = render_frames[0]->frame_index;
                    uint64_t fi1 = render_frames[1]->frame_index;
                    int64_t fi_diff = (int64_t)fi0 - (int64_t)fi1;
                    if (fi_diff < 0) fi_diff = -fi_diff;

                    if (++sync_log_count % 30 == 1 || sync_log_count <= 5 || fi_diff > 0)
                    {
                        LOGI("EYE_SYNC fi0=%llu fi1=%llu diff=%lld",
                             (unsigned long long)fi0, (unsigned long long)fi1,
                             (long long)fi_diff);
                    }
                }

                for (int e = 0; e < 2; e++) {
                    if (render_frames[e] && render_frames[e]->valid) {
                        g_stutter.on_pose_update(e, render_frames[e]->frame_index, render_frames[e]->server_pose[e]);
                    }
                }
                g_stutter.on_frame_begin(
                    render_frames[0] ? render_frames[0]->frame_index : 0,
                    render_frames[1] ? render_frames[1]->frame_index : 0);
                g_stutter.on_frame_end();
                g_stutter.log_summary();

                static int fov_cmp_count = 0;
                if (fov_cmp_count++ % 30 == 0 && render_frames[0] && render_frames[0]->valid)
                {
                    for (int e = 0; e < 2; e++)
                    {
                        const XrFovf &cf = app->stream.eye_fov[e];
                        const XrFovf &sf = render_frames[e] ? render_frames[e]->server_fov[e] : XrFovf{};
                        constexpr float rad2deg = 180.0f / 3.14159265358979f;
                        float client_h_fov = (cf.angleRight - cf.angleLeft) * rad2deg;
                        float client_v_fov = (cf.angleUp - cf.angleDown) * rad2deg;
                        float server_h_fov = (sf.angleRight - sf.angleLeft) * rad2deg;
                        float server_v_fov = (sf.angleUp - sf.angleDown) * rad2deg;
                        float h_ratio = (server_h_fov > 0.001f) ? client_h_fov / server_h_fov : 0;
                        float v_ratio = (server_v_fov > 0.001f) ? client_v_fov / server_v_fov : 0;
                        const XrPosef &sp = render_frames[e]->server_pose[e];
                        LOGI("FOV_CMP eye=%d | client: H=%.1f V=%.1f deg | server: H=%.1f V=%.1f deg | ratio: H=%.2f V=%.2f%s | server_pose: q=(%.3f,%.3f,%.3f,%.3f) p=(%.3f,%.3f,%.3f)",
                             e, client_h_fov, client_v_fov, server_h_fov, server_v_fov,
                             h_ratio, v_ratio,
                             (h_ratio < 0.95f || h_ratio > 1.05f) ? " [MISMATCH]" : "",
                             sp.orientation.x, sp.orientation.y, sp.orientation.z, sp.orientation.w,
                             sp.position.x, sp.position.y, sp.position.z);
                    }
                }
            }

            app->blit.blit(&app->stream.blit_pipeline, render_frames[eye], eye, glImg, w, h);
        } else {
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        }
        clock_gettime(CLOCK_MONOTONIC, &te);
        gl_draw_ms += ts_diff(te, td) * 1000.0f;

        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        XrSwapchainImageReleaseInfo ri = {};
        ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
        xrReleaseSwapchainImage(app->swapchains[eye].handle, &ri);
        clock_gettime(CLOCK_MONOTONIC, &tc);
        sc_rel_ms += ts_diff(tc, te) * 1000.0f;

        layerViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        if (app->stream.streaming.load() && render_frames[eye] && render_frames[eye]->valid)
            layerViews[eye].pose = render_frames[eye]->server_pose[eye];
        else
            layerViews[eye].pose = views[eye].pose;
        if (app->stream.streaming.load() && render_frames[eye] && render_frames[eye]->valid)
            layerViews[eye].fov = render_frames[eye]->server_fov[eye];
        else
            layerViews[eye].fov = app->stream.eye_fov[eye];
        layerViews[eye].subImage.swapchain = app->swapchains[eye].handle;
        layerViews[eye].subImage.imageRect.offset = {0, 0};
        layerViews[eye].subImage.imageRect.extent = {w, h};
        layerViews[eye].subImage.imageArrayIndex = 0;
    }

    clock_gettime(CLOCK_MONOTONIC, &t2);

    layers[0].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
    layers[0].layerFlags = 0;
    layers[0].space = app->localSpace;
    layers[0].viewCount = NUM_EYES;
    layers[0].views = layerViews;

    const XrCompositionLayerBaseHeader* layerHeaders[1] = {
        reinterpret_cast<XrCompositionLayerBaseHeader*>(&layers[0])
    };

    XrFrameEndInfo fe = {};
    fe.type = XR_TYPE_FRAME_END_INFO;
    fe.displayTime = fs.predictedDisplayTime;
    fe.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fe.layerCount = 1;
    fe.layers = layerHeaders;

    r = xrEndFrame(app->session, &fe);
    if (r != XR_SUCCESS) {
        LOGE("xrEndFrame failed: %d", r);
    }
    clock_gettime(CLOCK_MONOTONIC, &t3);
    g_prev_end = t3;

    g_wait_total += ts_diff(t1, t0);
    g_begin_total += ts_diff(ta, t1);
    g_locate_total += ts_diff(tb, ta);
    g_mid_total += ts_diff(tf, tb);
    g_math_total += ts_diff(tm1, tb);
    g_flush_total += ts_diff(tf, tm2);
    g_flush_direct_total += ts_diff(tfl1, tfl0);
    g_sc_wait_total += sc_wait_ms / 1000.0f;
    g_gl_draw_total += gl_draw_ms / 1000.0f;
    g_sc_rel_total += sc_rel_ms / 1000.0f;
    g_render_total += ts_diff(t2, t1);
    g_end_total += ts_diff(t3, t2);

    g_frame_count++;
    if (g_fps_start.tv_sec == 0) {
        clock_gettime(CLOCK_MONOTONIC, &g_fps_start);
    } else {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        float elapsed = (now.tv_sec - g_fps_start.tv_sec) + (now.tv_nsec - g_fps_start.tv_nsec) * 1e-9f;
        if (elapsed >= 10.0f) {
            float fps = g_frame_count / elapsed;
            float frame_time = elapsed * 1000.0f / g_frame_count;
            LOGI("FPS: %.1f | frame=%.1fms | flush=%.1f glDraw=%.1f end=%.1f",
                 fps, frame_time,
                 g_flush_direct_total * 1000.0f / g_frame_count,
                 g_gl_draw_total * 1000.0f / g_frame_count,
                 g_end_total * 1000.0f / g_frame_count);
            g_frame_count = 0;
            g_wait_total = g_render_total = g_end_total = 0;
            g_begin_total = g_locate_total = g_sc_wait_total = g_gl_draw_total = g_sc_rel_total = g_mid_total = g_math_total = g_flush_total = g_flush_direct_total = g_inter_total = 0;
            g_fps_start = now;
        }
    }
}

// ---------------------------------------------------------------------------
// Android main
// ---------------------------------------------------------------------------

struct AndroidAppState {
    ANativeWindow* nativeWindow = nullptr;
    bool resumed = false;
};

static void app_handle_cmd(struct android_app* app, int32_t cmd) {
    AndroidAppState* state = (AndroidAppState*)app->userData;
    switch (cmd) {
        case APP_CMD_RESUME:
            state->resumed = true;
            break;
        case APP_CMD_PAUSE:
            state->resumed = false;
            break;
        case APP_CMD_INIT_WINDOW:
            state->nativeWindow = app->window;
            break;
        case APP_CMD_TERM_WINDOW:
            state->nativeWindow = nullptr;
            break;
        case APP_CMD_DESTROY:
            state->nativeWindow = nullptr;
            break;
    }
}

static struct timespec g_home_press_ts = {};
static bool g_home_pressed = false;

static void trigger_recenter(AppState* app) {
    LOGI("recenter triggered by Pico home button");
    if (g_pfnXrResetSensorPICO && app->session) {
        XrResult rr = g_pfnXrResetSensorPICO(app->session, XR_RESET_ALL);
        LOGI("xrResetSensorPICO result: %d", rr);
    }
    if (app->localSpace) {
        xrDestroySpace(app->localSpace);
        app->localSpace = XR_NULL_HANDLE;
        LOGI("old localSpace destroyed");
    }
    XrReferenceSpaceCreateInfo rsci = {};
    rsci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1.f;
    rsci.poseInReferenceSpace.position.y = 0.f;
    XrResult rr = xrCreateReferenceSpace(app->session, &rsci, &app->localSpace);
    if (rr != XR_SUCCESS)
        LOGE("xrCreateReferenceSpace after recenter failed: %d", rr);
    else
        LOGI("Reference space recreated after recenter");
}

static int32_t on_input_event(struct android_app* app, AInputEvent* event) {
    int type = AInputEvent_getType(event);
    if (type == AINPUT_EVENT_TYPE_KEY) {
        int32_t code = AKeyEvent_getKeyCode(event);
        int32_t action = AKeyEvent_getAction(event);
        if (code == AKEYCODE_HOME) {
            if (action == AKEY_EVENT_ACTION_DOWN && !g_home_pressed) {
                clock_gettime(CLOCK_MONOTONIC, &g_home_press_ts);
                g_home_pressed = true;
                LOGI("Pico home button pressed, starting recenter timer");
            } else if (action == AKEY_EVENT_ACTION_UP && g_home_pressed) {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                double held_ms = (now.tv_sec - g_home_press_ts.tv_sec) * 1000.0
                               + (now.tv_nsec - g_home_press_ts.tv_nsec) / 1000000.0;
                g_home_pressed = false;
                LOGI("Pico home button released after %.0fms", held_ms);
                if (held_ms >= 500.0 && g_app) {
                    trigger_recenter(g_app);
                }
            }
            return 1;
        }
        if (code == AKEYCODE_BACK) {
            return 1;
        }
    }
    return 0;
}

extern "C" void android_main(struct android_app* androidApp) {
    auto logger = spdlog::android_logger_mt("WiVRn-Pico", "WiVRn-Pico");
    spdlog::set_default_logger(logger);
    spdlog::set_level(spdlog::level::debug);

    AndroidAppState androidState = {};
    androidApp->userData = &androidState;
    androidApp->onAppCmd = app_handle_cmd;
    androidApp->onInputEvent = on_input_event;

    AppState app;
    g_app = &app;

    g_jvm = androidApp->activity->vm;
    JNIEnv * env = nullptr;
    if (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK) {
        g_activity = env->NewGlobalRef(androidApp->activity->clazz);
        jclass clazz = env->GetObjectClass(g_activity);
        g_activityClass = (jclass)env->NewGlobalRef(clazz);
        env->DeleteLocalRef(clazz);
    }

    if (!egl_init(&app)) {
        LOGE("EGL init failed");
        return;
    }

    if (!openxr_init(androidApp, &app)) {
        LOGE("OpenXR init failed");
        egl_shutdown(&app);
        return;
    }

    if (!openxr_create_session(&app)) {
        LOGE("Session creation failed");
        xrDestroyInstance(app.instance);
        egl_shutdown(&app);
        return;
    }

    glGenFramebuffers(1, &app.fbo);
    glGenRenderbuffers(NUM_EYES, app.depth_rbo);

    int eye_w = app.swapchains[0].width;
    int eye_h = app.swapchains[0].height;
    app.blit.init(app.display, eye_w, eye_h);
    app.stream.blit_pipeline.init(eye_w, eye_h);
    app.stream.eye_width = eye_w;
    app.stream.eye_height = eye_h;
    app.stream.vm = g_jvm;
    app.stream.activity = g_activity;
    g_stream = &app.stream;

    LOGI("Entering main loop");

    while (androidApp->destroyRequested == 0) {
        for (;;) {
            int events;
            struct android_poll_source* source;
            int timeout = (!androidState.resumed && !app.sessionRunning) ? -1 : 0;
            int ret = ALooper_pollOnce(timeout, nullptr, &events, (void**)&source);
            if (ret == ALOOPER_POLL_TIMEOUT || ret == ALOOPER_POLL_ERROR)
                break;
            if (source)
                source->process(androidApp, source);
            if (androidApp->destroyRequested)
                break;
        }

        poll_events(&app);

        if (app.shouldExit)
            break;

        if (app.sessionRunning) {
            render_frame(&app);
        } else {
            usleep(16000);
        }
    }

    g_stream = nullptr;
    if (app.fbo) glDeleteFramebuffers(1, &app.fbo);
    glDeleteRenderbuffers(NUM_EYES, app.depth_rbo);
    openxr_destroy_session(&app);
    if (app.instance)
        xrDestroyInstance(app.instance);
    egl_shutdown(&app);

    g_app = nullptr;
    if (g_activity && g_jvm) {
        JNIEnv * env = nullptr;
        if (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK) {
            env->DeleteGlobalRef(g_activity);
            if (g_activityClass)
                env->DeleteGlobalRef(g_activityClass);
        }
        g_activity = nullptr;
        g_activityClass = nullptr;
    }
    g_jvm = nullptr;

    LOGI("Main loop exited");
}
