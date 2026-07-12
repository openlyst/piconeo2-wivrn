#include <jni.h>
#include <android/log.h>
#include <android_native_app_glue.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <unistd.h>
#include <sys/socket.h>
#include <vector>
#include <cstring>
#include <cmath>
#include <ctime>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <thread>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/android_sink.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#define XR_USE_PLATFORM_ANDROID 1
#define XR_USE_GRAPHICS_API_OPENGL_ES 1
#include "openxr/openxr.h"
#include "openxr/openxr_platform.h"

#include "lobby.h"
#include "streaming/streaming_client.h"
#include "streaming/oxr_blit.h"
#include "streaming/eye_tracking.h"
#include "pico_stutter.h"
#include "latency_tracker.h"

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
static jmethodID g_onLobbyTouchMethod = nullptr;
static jclass g_activityClass = nullptr;

static int prev_touch_hand = -1;
static bool prev_touch_down = false;
static float prev_touch_x = -1, prev_touch_y = -1;
static std::atomic<bool> g_ok_pressed{false};

// ---------------------------------------------------------------------------
// JNI: called from Java controller poll thread
// ---------------------------------------------------------------------------

extern "C" {

JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_oxr_MainActivity_nativeGetHeadData(JNIEnv * env, jobject thiz, jfloatArray out)
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
Java_org_meumeu_wivrn_oxr_MainActivity_nativeControllerState(
    JNIEnv * env, jobject thiz, jint hand, jint conn,
    jfloatArray sensor, jfloatArray angVel, jintArray keys)
{
    if (hand < 0 || hand > 1)
        return;

    float sensor_buf[7] = {0};
    float angvel_buf[3] = {0};
    int keys_buf[12] = {0};
    bool has_sensor = false, has_angvel = false, has_keys = false;

    if (sensor && env->GetArrayLength(sensor) >= 7)
    {
        env->GetFloatArrayRegion(sensor, 0, 7, sensor_buf);
        has_sensor = true;
    }
    if (angVel && env->GetArrayLength(angVel) >= 3)
    {
        env->GetFloatArrayRegion(angVel, 0, 3, angvel_buf);
        has_angvel = true;
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
            if (has_angvel)
            {
                c.angular_velocity[0] = angvel_buf[0];
                c.angular_velocity[1] = angvel_buf[1];
                c.angular_velocity[2] = angvel_buf[2];
                c.has_angular_velocity = true;
            }
            else
            {
                c.has_angular_velocity = false;
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
    XrSpace viewSpace = XR_NULL_HANDLE;
    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    bool sessionRunning = false;

    XrActionSet hapticActionSet = XR_NULL_HANDLE;
    XrAction hapticActions[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    XrPath hapticSubactionPaths[2] = {XR_NULL_PATH, XR_NULL_PATH};

    SwapchainState swapchains[NUM_EYES];
    XrViewConfigurationView viewConfigs[NUM_EYES] = {};

    GLuint fbo = 0;
    GLuint depth_rbo[NUM_EYES] = {};
    int depth_rbo_w[NUM_EYES] = {};
    int depth_rbo_h[NUM_EYES] = {};

    pico_lobby lobby;

    streaming_client stream;
    oxr_blit blit;

    bool shouldExit = false;
};

// ---------------------------------------------------------------------------
// JNI: SurfaceTexture bridge
// ---------------------------------------------------------------------------

extern "C" {

JNIEXPORT jboolean JNICALL
Java_org_meumeu_wivrn_oxr_MainActivity_nativeDrainHaptic(
    JNIEnv * env, jobject thiz, jint hand, jfloatArray out)
{
    if (!g_app || hand < 0 || hand > 1 || !out)
        return JNI_FALSE;
    if (env->GetArrayLength(out) < 2)
        return JNI_FALSE;

    float amp;
    int ms;
    {
        std::lock_guard lock(g_app->stream.haptics_mutex);
        auto & slot = g_app->stream.rumble[hand];
        if (!slot.active)
            return JNI_FALSE;
        amp = slot.amplitude;
        ms = slot.duration_ms;
        slot.active = false;
        slot.amplitude = 0.f;
        slot.duration_ms = 0;
    }

    float vals[2] = {amp, static_cast<float>(ms)};
    env->SetFloatArrayRegion(out, 0, 2, vals);
    return JNI_TRUE;
}

JNIEXPORT jint JNICALL
Java_org_meumeu_wivrn_oxr_MainActivity_nativeGetTextureId(JNIEnv * env, jobject thiz)
{
    if (!g_app)
        return 0;
    return (jint)g_app->lobby.get_external_texture();
}

JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_oxr_MainActivity_nativeSetSurfaceTexture(JNIEnv * env, jobject thiz, jobject surfaceTexture)
{
    if (!g_app || !surfaceTexture)
        return;

    jclass stClass = env->GetObjectClass(surfaceTexture);
    jmethodID updateMethod = env->GetMethodID(stClass, "updateTexImage", "()V");
    env->DeleteLocalRef(stClass);

    jobject globalST = env->NewGlobalRef(surfaceTexture);
    g_app->lobby.set_surface_texture(globalST, updateMethod);
}

JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_oxr_MainActivity_nativeOnFrameAvailable(JNIEnv * env, jobject thiz)
{
    if (g_app)
        g_app->lobby.on_frame_available();
}

JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_oxr_MainActivity_nativeConnectServer(JNIEnv * env, jobject thiz, jstring host, jint port, jboolean tcpOnly)
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
    g_app->stream.shutdown = false;
    g_app->stream.auto_reconnect.store(false);
    g_app->stream.try_connect();
}

JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_oxr_MainActivity_nativeDisconnectServer(JNIEnv * env, jobject thiz)
{
    if (!g_app) return;
    LOGI("nativeDisconnectServer");
    auto & s = g_app->stream;
    s.auto_reconnect.store(false);
    s.shutdown = true;
    if (s.session)
    {
        int fd = s.session->get_control_fd();
        ::shutdown(fd, SHUT_RDWR);
    }
    std::thread([&s] {
        std::lock_guard lock(s.connect_mutex);
        if (s.connect_thread.joinable())
            s.connect_thread.join();
        if (s.network_thread.joinable())
            s.network_thread.join();
        s.session.reset();
        s.reset_stream_state();
        s.shutdown = false;
    }).detach();
}

JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_oxr_MainActivity_nativeSetPin(JNIEnv * env, jobject thiz, jstring pin)
{
    if (!g_app) return;
    const char * p = env->GetStringUTFChars(pin, nullptr);
    g_app->stream.pairing_pin = p;
    env->ReleaseStringUTFChars(pin, p);
}

JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_oxr_MainActivity_nativeSetBitrate(JNIEnv * env, jobject thiz, jint bitrateMbps)
{
    if (!g_app) return;
    LOGI("nativeSetBitrate: %d Mbps", bitrateMbps);
    g_app->stream.bitrate_mbps.store(bitrateMbps);
    g_app->stream.max_bitrate_mbps.store(bitrateMbps);
    g_app->stream.current_bitrate_mbps.store(bitrateMbps);
}

JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_oxr_MainActivity_nativeSetDynamicBitrate(JNIEnv * env, jobject thiz, jboolean enabled)
{
    if (!g_app) return;
    g_app->stream.dynamic_bitrate_enabled.store(enabled);
    LOGI("Dynamic bitrate %s", enabled ? "enabled" : "disabled");
}

JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_oxr_MainActivity_nativeSetIpd(JNIEnv * env, jobject thiz, jfloat ipdMm)
{
    if (!g_app) return;
    float ipd_m = ipdMm * 0.001f;
    g_app->stream.tracker.soft_ipd.store(ipd_m);
    LOGI("Software IPD set to %.1f mm (%.4f m)", ipdMm, ipd_m);
}

JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_oxr_MainActivity_nativeSetMicrophone(JNIEnv * env, jobject thiz, jboolean enabled)
{
    if (!g_app) return;
    g_app->stream.microphone_enabled.store(enabled);
    if (g_app->stream.audio_handle)
        g_app->stream.audio_handle->set_mic_state(enabled);
    LOGI("Microphone %s", enabled ? "enabled" : "disabled");
}

JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_oxr_MainActivity_nativeSetStreamResolution(JNIEnv * env, jobject thiz, jint width, jint height)
{
    if (!g_app) return;
    int old_w = g_app->stream.stream_eye_width.load();
    int old_h = g_app->stream.stream_eye_height.load();
    g_app->stream.stream_eye_width.store(width);
    g_app->stream.stream_eye_height.store(height);
    if ((width != old_w || height != old_h) && g_app->stream.session)
    {
        g_app->stream.send_headset_info();
        LOGI("Sent updated stream resolution to server: %dx%d", width, height);
    }
    LOGI("Stream resolution set to %dx%d", width, height);
}

JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_oxr_MainActivity_nativeSetRenderResolution(JNIEnv * env, jobject thiz, jint width, jint height)
{
    if (!g_app) return;
    int old_w = g_app->stream.eye_width.load();
    int old_h = g_app->stream.eye_height.load();
    g_app->stream.eye_width.store(width);
    g_app->stream.eye_height.store(height);
    g_app->lobby.set_resolution(width, height);
    g_app->stream.blit_pipeline.set_resolution(width, height);
    if (width != old_w || height != old_h)
        g_app->stream.resolution_dirty.store(true);
    LOGI("Render resolution set to %dx%d", width, height);
}

JNIEXPORT jboolean JNICALL
Java_org_meumeu_wivrn_oxr_MainActivity_nativeReady(JNIEnv * env, jobject thiz)
{
    return g_app != nullptr ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_oxr_MainActivity_nativeRequestAppList(JNIEnv * env, jobject thiz)
{
    if (!g_app || !g_app->stream.session)
        return;
    LOGI("nativeRequestAppList");
    try
    {
        g_app->stream.session->send_control(from_headset::get_application_list{
            .language = "en",
            .country = "US",
            .variant = "",
        });
    }
    catch (std::exception & e)
    {
        LOGE("Failed to request app list: %s", e.what());
    }
}

JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_oxr_MainActivity_nativeStartApp(JNIEnv * env, jobject thiz, jstring appId)
{
    if (!g_app || !g_app->stream.session)
        return;
    const char * app_id_str = env->GetStringUTFChars(appId, nullptr);
    if (app_id_str)
    {
        LOGI("nativeStartApp: %s", app_id_str);
        try
        {
            g_app->stream.session->send_control(from_headset::start_app{
                .app_id = app_id_str,
            });
        }
        catch (std::exception & e)
        {
            LOGE("Failed to start app: %s", e.what());
        }
        env->ReleaseStringUTFChars(appId, app_id_str);
    }
}

JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_oxr_MainActivity_nativeRequestRunningApps(JNIEnv * env, jobject thiz)
{
    if (!g_app || !g_app->stream.session)
        return;
    try
    {
        g_app->stream.session->send_control(from_headset::get_running_applications{});
    }
    catch (std::exception & e)
    {
        LOGE("Failed to request running apps: %s", e.what());
    }
}

JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_oxr_MainActivity_nativeSetActiveApp(JNIEnv * env, jobject thiz, jint appId)
{
    if (!g_app || !g_app->stream.session)
        return;
    LOGI("nativeSetActiveApp: %d", (int)appId);
    try
    {
        g_app->stream.session->send_control(from_headset::set_active_application{
            .id = (uint32_t)appId,
        });
    }
    catch (std::exception & e)
    {
        LOGE("Failed to set active app: %s", e.what());
    }
}

JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_oxr_MainActivity_nativeStopApp(JNIEnv * env, jobject thiz, jint appId)
{
    if (!g_app || !g_app->stream.session)
        return;
    LOGI("nativeStopApp: %d", (int)appId);
    try
    {
        g_app->stream.session->send_control(from_headset::stop_application{
            .id = (uint32_t)appId,
        });
    }
    catch (std::exception & e)
    {
        LOGE("Failed to stop app: %s", e.what());
    }
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
    bool hasPicoViewState = false, hasPicoFrameEndInfo = false;
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
        if (strcmp(e.extensionName, XR_PICO_VIEW_STATE_EXT_ENABLE_EXTENSION_NAME) == 0)
            hasPicoViewState = true;
        if (strcmp(e.extensionName, XR_PICO_FRAME_END_INFO_EXT_EXTENSION_NAME) == 0)
            hasPicoFrameEndInfo = true;
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
    if (hasPicoViewState)
        enabledExtsVec.push_back(XR_PICO_VIEW_STATE_EXT_ENABLE_EXTENSION_NAME);
    if (hasPicoFrameEndInfo)
        enabledExtsVec.push_back(XR_PICO_FRAME_END_INFO_EXT_EXTENSION_NAME);

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
        XrResult br = g_pfnXrInvokeFunctionsPICO(app->session, XR_START_SDK_BOUNDARY, dummy_in, 0, &dummy_out, 0);
        LOGI("XR_START_SDK_BOUNDARY result: %d", br);
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

    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    r = xrCreateReferenceSpace(app->session, &rsci, &app->viewSpace);
    if (r != XR_SUCCESS) {
        LOGW("xrCreateReferenceSpace VIEW failed: %d, velocity will use EMA fallback", r);
        app->viewSpace = XR_NULL_HANDLE;
    }

    // Create haptic action set for controller vibration
    {
        XrActionSetCreateInfo asci = {};
        asci.type = XR_TYPE_ACTION_SET_CREATE_INFO;
        strcpy(asci.actionSetName, "haptics");
        strcpy(asci.localizedActionSetName, "Controller Haptics");
        asci.priority = 0;
        r = xrCreateActionSet(app->instance, &asci, &app->hapticActionSet);
        if (r != XR_SUCCESS) {
            LOGE("xrCreateActionSet failed: %d", r);
        } else {
            // Create subaction paths for left and right hands
            xrStringToPath(app->instance, "/user/hand/left", &app->hapticSubactionPaths[0]);
            xrStringToPath(app->instance, "/user/hand/right", &app->hapticSubactionPaths[1]);

            // Single vibrate action with both subaction paths (matches Pico HelloXR sample)
            XrActionCreateInfo aci = {};
            aci.type = XR_TYPE_ACTION_CREATE_INFO;
            strcpy(aci.actionName, "vibrate_hand");
            strcpy(aci.localizedActionName, "Vibrate Hand");
            aci.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
            aci.countSubactionPaths = 2;
            aci.subactionPaths = app->hapticSubactionPaths;
            r = xrCreateAction(app->hapticActionSet, &aci, &app->hapticActions[0]);
            if (r != XR_SUCCESS) {
                LOGE("xrCreateAction vibrate_hand failed: %d", r);
            } else {
                app->hapticActions[1] = app->hapticActions[0]; // same action, different subaction path
                LOGI("Vibrate action created successfully");
            }

            XrPath hapticLeftPath, hapticRightPath;
            xrStringToPath(app->instance, "/user/hand/left/output/haptic", &hapticLeftPath);
            xrStringToPath(app->instance, "/user/hand/right/output/haptic", &hapticRightPath);

            // Suggest bindings for all relevant interaction profiles
            const char* profilePaths[] = {
                "/interaction_profiles/pico/cv2_controller",
                "/interaction_profiles/khr/simple_controller",
                "/interaction_profiles/oculus/touch_controller",
            };

            for (auto profilePath : profilePaths) {
                XrActionSuggestedBinding suggestedBindings[2] = {
                    {app->hapticActions[0], hapticLeftPath},
                    {app->hapticActions[0], hapticRightPath},
                };

                XrInteractionProfileSuggestedBinding ipsb = {};
                ipsb.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
                xrStringToPath(app->instance, profilePath, &ipsb.interactionProfile);
                ipsb.countSuggestedBindings = 2;
                ipsb.suggestedBindings = suggestedBindings;
                r = xrSuggestInteractionProfileBindings(app->instance, &ipsb);
                if (r != XR_SUCCESS) {
                    LOGI("xrSuggestInteractionProfileBindings (%s) failed: %d", profilePath, r);
                } else {
                    LOGI("Suggested haptic bindings for %s", profilePath);
                }
            }

            // Attach action set to session
            XrSessionActionSetsAttachInfo sasai = {};
            sasai.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO;
            sasai.countActionSets = 1;
            sasai.actionSets = &app->hapticActionSet;
            r = xrAttachSessionActionSets(app->session, &sasai);
            if (r != XR_SUCCESS) {
                LOGE("xrAttachSessionActionSets failed: %d", r);
            } else {
                LOGI("Haptic action set attached successfully");
            }
        }
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
        swci.width = app->stream.eye_width.load();
        swci.height = app->stream.eye_height.load();
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

static bool recreate_swapchain(AppState* app) {
    int new_w = app->stream.eye_width.load();
    int new_h = app->stream.eye_height.load();

    for (uint32_t eye = 0; eye < NUM_EYES; eye++) {
        if (app->swapchains[eye].handle) {
            xrDestroySwapchain(app->swapchains[eye].handle);
            app->swapchains[eye].handle = XR_NULL_HANDLE;
            app->swapchains[eye].images.clear();
        }
    }

    for (uint32_t eye = 0; eye < NUM_EYES; eye++) {
        XrSwapchainCreateInfo swci = {};
        swci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
        swci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        swci.format = GL_RGBA8;
        swci.width = new_w;
        swci.height = new_h;
        swci.sampleCount = app->viewConfigs[eye].recommendedSwapchainSampleCount;
        swci.faceCount = 1;
        swci.arraySize = 1;
        swci.mipCount = 1;

        XrResult r = xrCreateSwapchain(app->session, &swci, &app->swapchains[eye].handle);
        if (r != XR_SUCCESS) {
            LOGE("recreate_swapchain eye %u failed: %d", eye, r);
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
        LOGI("Eye %u swapchain recreated: %ux%u, %u images", eye, swci.width, swci.height, imgCount);
    }

    app->lobby.set_resolution(new_w, new_h);
    app->stream.blit_pipeline.set_resolution(new_w, new_h);
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
    if (app->viewSpace) {
        xrDestroySpace(app->viewSpace);
        app->viewSpace = XR_NULL_HANDLE;
    }
    if (app->hapticActions[0]) {
        xrDestroyAction(app->hapticActions[0]);
        app->hapticActions[0] = XR_NULL_HANDLE;
        app->hapticActions[1] = XR_NULL_HANDLE;
    }
    if (app->hapticActionSet) {
        xrDestroyActionSet(app->hapticActionSet);
        app->hapticActionSet = XR_NULL_HANDLE;
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
            app->stream.tracker.recenter_height();
            if (g_pfnXrResetSensorPICO && app->session) {
                XrResult rr = g_pfnXrResetSensorPICO(app->session, XR_RESET_ALL);
                LOGI("xrResetSensorPICO result: %d", rr);
            }
            if (app->localSpace) {
                xrDestroySpace(app->localSpace);
                app->localSpace = XR_NULL_HANDLE;
            }
            if (app->viewSpace) {
                xrDestroySpace(app->viewSpace);
                app->viewSpace = XR_NULL_HANDLE;
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
            rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
            rr = xrCreateReferenceSpace(app->session, &rsci, &app->viewSpace);
            if (rr != XR_SUCCESS)
                LOGW("xrCreateReferenceSpace VIEW after recenter failed: %d", rr);
            app->lobby.recenter();
            break;
        }
        case XR_TYPE_EVENT_KEY_EVENT: {
            auto* evt = reinterpret_cast<XrEventDataKeyEvent*>(&edb);
            LOGI("XR key event: keyCode=%d keyAction=%d repeat=%d", evt->keyCode, evt->keyAction, evt->repeat);
            g_ok_pressed.store(evt->keyAction == 0);
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

static int g_stats_frame_count = 0;
static int64_t g_stats_last_ns = 0;
static uint64_t g_stats_prev_bytes_rx = 0;
static uint64_t g_stats_prev_bytes_tx = 0;
static float g_stats_bw_rx_smooth = 0;
static float g_stats_bw_tx_smooth = 0;

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

    if (app->stream.resolution_dirty.exchange(false) && app->session != XR_NULL_HANDLE)
    {
        int new_w = app->stream.eye_width.load();
        int new_h = app->stream.eye_height.load();
        if (new_w != app->swapchains[0].width || new_h != app->swapchains[0].height)
        {
            LOGI("Resolution changed, recreating swapchain %dx%d -> %dx%d",
                 app->swapchains[0].width, app->swapchains[0].height, new_w, new_h);
            recreate_swapchain(app);
        }
    }

    // Apply pending haptic feedback from server
    if (app->hapticActionSet != XR_NULL_HANDLE && app->hapticActions[0] != XR_NULL_HANDLE
        && app->stream.streaming.load())
    {
        for (int hand = 0; hand < 2; hand++)
        {
            float amp = 0.f;
            int ms = 0;
            {
                std::lock_guard lock(app->stream.haptics_mutex);
                auto & slot = app->stream.rumble[hand];
                if (!slot.active)
                    continue;
                amp = slot.amplitude;
                ms = slot.duration_ms;
                slot.active = false;
                slot.amplitude = 0.f;
                slot.duration_ms = 0;
            }

            if (amp > 0.f)
            {
                XrHapticActionInfo hai = {};
                hai.type = XR_TYPE_HAPTIC_ACTION_INFO;
                hai.action = app->hapticActions[0]; // single action for both hands
                hai.subactionPath = app->hapticSubactionPaths[hand];

                XrHapticVibration vibration = {};
                vibration.type = XR_TYPE_HAPTIC_VIBRATION;
                vibration.duration = (XrDuration)ms * 1000000LL;
                vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
                vibration.amplitude = amp;

                XrResult hr = xrApplyHapticFeedback(app->session, &hai, (XrHapticBaseHeader*)&vibration);
                if (hr != XR_SUCCESS)
                {
                    static int haptic_err_count = 0;
                    if (haptic_err_count++ % 100 == 0)
                        LOGE("xrApplyHapticFeedback hand %d failed: %d", hand, hr);
                }
                else
                {
                    static int haptic_ok_count = 0;
                    if (haptic_ok_count++ % 100 == 0)
                        LOGI("xrApplyHapticFeedback hand %d amp=%.2f ms=%d ok", hand, amp, ms);
                }
            }
        }
    }

    static std::shared_ptr<pico_decoded_frame> render_frames[3];
    static bool was_streaming = false;

    if (!app->stream.streaming.load() && was_streaming)
    {
        render_frames[0].reset();
        render_frames[1].reset();
        render_frames[2].reset();
        was_streaming = false;
    }

    XrViewState vstate = {};
    vstate.type = XR_TYPE_VIEW_STATE;

    XrViewStatePICOEXT vsPico = {};
    vsPico.type = XR_TYPE_VIEW_STATE;
    vstate.next = &vsPico;

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
    float hw_ipd = sqrtf(dx*dx + dy*dy + dz*dz);
    float ipd = app->stream.tracker.soft_ipd.load();
    if (ipd <= 0.001f) ipd = hw_ipd;

    float hw_lin_vel[3] = {0, 0, 0};
    float hw_ang_vel[3] = {0, 0, 0};
    bool have_hw_vel = false;
    if (app->viewSpace != XR_NULL_HANDLE)
    {
        XrSpaceVelocity velocity = {};
        velocity.type = XR_TYPE_SPACE_VELOCITY;
        XrSpaceLocation location = {};
        location.type = XR_TYPE_SPACE_LOCATION;
        location.next = &velocity;
        XrResult vr = xrLocateSpace(app->viewSpace, app->localSpace,
                                     fs.predictedDisplayTime, &location);
        static int vel_diag_count = 0;
        if (vel_diag_count++ < 5)
            LOGI("xrLocateSpace VIEW: result=%d velFlags=0x%x linValid=%d angValid=%d",
                 vr, velocity.velocityFlags,
                 (velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) ? 1 : 0,
                 (velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) ? 1 : 0);
        if (vr == XR_SUCCESS &&
            (velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) &&
            (velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT))
        {
            hw_lin_vel[0] = velocity.linearVelocity.x;
            hw_lin_vel[1] = velocity.linearVelocity.y;
            hw_lin_vel[2] = velocity.linearVelocity.z;
            hw_ang_vel[0] = velocity.angularVelocity.x;
            hw_ang_vel[1] = velocity.angularVelocity.y;
            hw_ang_vel[2] = velocity.angularVelocity.z;
            have_hw_vel = true;
        }
    }

    if (have_hw_vel)
        app->stream.tracker.set_head_pose(head_orient, head_pos, hw_lin_vel, hw_ang_vel);
    else
        app->stream.tracker.set_head_pose(head_orient, head_pos);

    // Pico Neo 2 has a square ~101 degree per-eye FOV.
    // The OpenXR runtime reports incorrect/wider values that cause zoom-in.
    // Use the known-correct value, same as the PVR client.
    constexpr float k_fov_half = streaming_client::k_pico_fov_half;
    app->stream.eye_fov[0] = {-k_fov_half, k_fov_half, k_fov_half, -k_fov_half};
    app->stream.eye_fov[1] = {-k_fov_half, k_fov_half, k_fov_half, -k_fov_half};

    static int hmd_log_count = 0;
    if (hmd_log_count++ % 120 == 0)
    {
        LOGI("RENDER HMD: q=(%.4f,%.4f,%.4f,%.4f) pos_m=(%.4f,%.4f,%.4f) ipd=%.4f "
             "hw_vel=%d lv=(%.2f,%.2f,%.2f) av=(%.2f,%.2f,%.2f)",
             head_orient[0], head_orient[1], head_orient[2], head_orient[3],
             head_pos[0], head_pos[1], head_pos[2], ipd,
             have_hw_vel ? 1 : 0,
             hw_lin_vel[0], hw_lin_vel[1], hw_lin_vel[2],
             hw_ang_vel[0], hw_ang_vel[1], hw_ang_vel[2]);
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
                app->stream.tracker.recenter_height();
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
                if (app->viewSpace)
                {
                    xrDestroySpace(app->viewSpace);
                    app->viewSpace = XR_NULL_HANDLE;
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
                rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
                rr = xrCreateReferenceSpace(app->session, &rsci, &app->viewSpace);
                if (rr != XR_SUCCESS)
                    LOGW("xrCreateReferenceSpace VIEW after recenter failed: %d", rr);
                app->lobby.recenter();
            }
            else if (home_press_ts[h] > 0)
            {
                LOGI("controller %d home released too quickly (%llums), no recenter", h, (now_ns - home_press_ts[h]) / 1000000ULL);
            }
            home_press_ts[h] = 0;
        }
        prev_home[h] = home_now;
    }

    // Toggle in-stream UI when both thumbsticks are clicked
    if (app->stream.streaming.load())
    {
        static bool prev_stick_click[2] = {false, false};
        bool stick_click[2] = {cs[0].connected && cs[0].thumbstick_click,
                                cs[1].connected && cs[1].thumbstick_click};

        bool both_now = stick_click[0] && stick_click[1];
        bool both_prev = prev_stick_click[0] && prev_stick_click[1];
        if (both_now && !both_prev)
        {
            app->stream.stream_ui_visible.store(!app->stream.stream_ui_visible.load());
            LOGI("Both thumbsticks clicked: stream_ui_visible -> %d",
                 (int)app->stream.stream_ui_visible.load());
        }
        prev_stick_click[0] = stick_click[0];
        prev_stick_click[1] = stick_click[1];
    }

    for (int h = 0; h < 2; h++)
    {
        if (cs[h].connected)
        {
            app->stream.tracker.update_controller(
                h, cs[h].orientation, cs[h].position,
                cs[h].trigger, cs[h].touch, cs[h].battery,
                cs[h].button_a, cs[h].button_b, cs[h].grip,
                cs[h].thumbstick_click, cs[h].menu,
                cs[h].has_angular_velocity ? cs[h].angular_velocity : nullptr);

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
    if (!app->stream.streaming.load() || app->stream.stream_ui_visible.load())
    {
        JNIEnv * env = nullptr;
        bool attached = false;
        if (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
            if (g_jvm->AttachCurrentThread(&env, nullptr) == JNI_OK)
                attached = true;
        }
        if (env)
            app->lobby.update_tex_image(env);
        if (attached)
            g_jvm->DetachCurrentThread();
    }
    clock_gettime(CLOCK_MONOTONIC, &tfl1);

    struct timespec tf;
    clock_gettime(CLOCK_MONOTONIC, &tf);

    // Late frame selection — deferred to just before the blit to minimize
    // the time between frame decode and frame display. The lambda captures
    // the latest decoded frames as late as possible in the render pipeline.
    auto select_latest_frames = [&]() {
        if (!app->stream.streaming.load())
            return;

        was_streaming = true;
        std::lock_guard lock(app->stream.decoded_frame_mutex);

        uint64_t idx0 = app->stream.latest_decoded_frame_index_per_stream[0].load(std::memory_order_acquire);
        uint64_t idx1 = app->stream.latest_decoded_frame_index_per_stream[1].load(std::memory_order_acquire);

        bool got_pair = false;
        if (idx0 > 0 && idx1 > 0)
        {
            uint64_t chosen = std::min(idx0, idx1);
            auto f0 = app->stream.get_frame(chosen, 0);
            auto f1 = app->stream.get_frame(chosen, 1);
            if (f0 && f0->valid && f1 && f1->valid)
            {
                render_frames[0] = f0;
                render_frames[1] = f1;
                got_pair = true;
            }
        }

        if (!got_pair)
        {
            for (int e = 0; e < 2; e++)
            {
                uint64_t idx = app->stream.latest_decoded_frame_index_per_stream[e].load(std::memory_order_acquire);
                if (idx > 0)
                {
                    auto f = app->stream.get_frame(idx, e);
                    if (f && f->valid)
                        render_frames[e] = f;
                }
            }
        }

        for (int e = 0; e < 2; e++)
        {
            if (render_frames[e] && render_frames[e]->valid)
                g_latency.on_frame_rendered(render_frames[e]->frame_index, e);
        }
    };

    // Stall detection uses whatever frames we last selected.
    if (app->stream.streaming.load())
    {
        was_streaming = true;
        // Stall detection: if the same frame has been displayed for >2s
        // without any new frames arriving, mark the stream as stalled.
        uint64_t cur_frame = render_frames[0] ? render_frames[0]->frame_index : 0;
        int64_t now_ns = app->stream.get_timestamp_ns();
        uint64_t prev_frame = app->stream.last_displayed_frame.load();
        if (cur_frame != prev_frame)
        {
            app->stream.last_displayed_frame.store(cur_frame);
            app->stream.frame_first_displayed_ns.store(now_ns);
            if (app->stream.stream_stalled.load())
            {
                app->stream.stream_stalled.store(false);
                LOGI("Stream recovered from stall at frame %llu", (unsigned long long)cur_frame);
            }
        }
        else if (cur_frame > 0)
        {
            int64_t first_ns = app->stream.frame_first_displayed_ns.load();
            if (first_ns > 0 && (now_ns - first_ns) > 2'000'000'000LL)
            {
                if (!app->stream.stream_stalled.load())
                {
                    app->stream.stream_stalled.store(true);
                    app->stream.stream_ui_visible.store(true);
                    LOGW("Stream stalled: frame %llu displayed for %.1fs, no new frames",
                         (unsigned long long)cur_frame, (now_ns - first_ns) / 1e9f);
                }
                // After 5s of stall, attempt automatic reconnect
                if ((now_ns - first_ns) > 5'000'000'000LL && app->stream.auto_reconnect.load())
                {
                    LOGW("Stream stalled for 5s, triggering reconnect");
                    app->stream.stream_stalled.store(false);
                    app->stream.stream_ui_visible.store(false);
                    app->stream.try_connect();
                    app->stream.last_displayed_frame.store(0);
                    app->stream.frame_first_displayed_ns.store(0);
                }
            }
        }
    }

    XrCompositionLayerProjection layers[1];
    XrCompositionLayerProjectionView layerViews[NUM_EYES];

    float sc_wait_ms = 0, gl_draw_ms = 0, sc_rel_ms = 0;

    // Pre-acquire and wait on both swapchain images before selecting frames.
    // This lets us pick up frames decoded during the swapchain wait.
    uint32_t imgIndices[NUM_EYES] = {0, 0};
    bool swapchain_ok[NUM_EYES] = {false, false};
    for (uint32_t eye = 0; eye < NUM_EYES; eye++) {
        XrSwapchainImageAcquireInfo ai = {};
        ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
        r = xrAcquireSwapchainImage(app->swapchains[eye].handle, &ai, &imgIndices[eye]);
        if (r != XR_SUCCESS) {
            LOGE("xrAcquireSwapchainImage eye %u failed: %d", eye, r);
            continue;
        }

        XrSwapchainImageWaitInfo wi = {};
        wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
        wi.timeout = 10000000000; // 10s in ns
        clock_gettime(CLOCK_MONOTONIC, &tc);
        r = xrWaitSwapchainImage(app->swapchains[eye].handle, &wi);
        clock_gettime(CLOCK_MONOTONIC, &td);
        sc_wait_ms += ts_diff(td, tc) * 1000.0f;
        if (r != XR_SUCCESS) {
            LOGE("xrWaitSwapchainImage eye %u failed: %d", eye, r);
            continue;
        }
        swapchain_ok[eye] = true;
    }

    // Late frame selection — now that swapchain waits are done, grab the
    // newest decoded frames. This minimizes rend_wait latency.
    select_latest_frames();

    for (uint32_t eye = 0; eye < NUM_EYES; eye++) {
        if (!swapchain_ok[eye])
            continue;

        uint32_t imgIndex = imgIndices[eye];
        GLuint glImg = app->swapchains[eye].images[imgIndex].image;
        int32_t w = app->swapchains[eye].width;
        int32_t h = app->swapchains[eye].height;
        app->lobby.set_resolution(w, h);
        app->stream.blit_pipeline.set_resolution(w, h);

        glBindFramebuffer(GL_FRAMEBUFFER, app->fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, glImg, 0);

        bool is_streaming_blit = app->stream.streaming.load() && !app->stream.stream_ui_visible.load();

        if (!is_streaming_blit) {
            if (app->depth_rbo_w[eye] != w || app->depth_rbo_h[eye] != h) {
                glBindRenderbuffer(GL_RENDERBUFFER, app->depth_rbo[eye]);
                glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
                app->depth_rbo_w[eye] = w;
                app->depth_rbo_h[eye] = h;
            }
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, app->depth_rbo[eye]);
        }

        std::shared_ptr<pico_decoded_frame> decoded;
        if (app->stream.streaming.load())
        {
            decoded = render_frames[eye];
        }

        if (is_streaming_blit) {
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
            static int lobby_log_count = 0;
            if (eye == 0 && (lobby_log_count++ % 300 == 0)) LOGI("LOBBY: streaming=%d", (int)app->stream.streaming.load());
            constexpr float k_lobby_fov_half = 101.0f * 0.5f * 0.01745329252f;
            XrFovf lobby_fov = {-k_lobby_fov_half, k_lobby_fov_half, k_lobby_fov_half, -k_lobby_fov_half};
            app->lobby.draw(eye, head_orient, head_pos, cs, lobby_fov, ipd, g_ok_pressed.load());
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
        if (app->stream.streaming.load() && !app->stream.stream_ui_visible.load() && render_frames[eye] && render_frames[eye]->valid)
        {
            layerViews[eye].pose = render_frames[eye]->server_pose[eye];
            layerViews[eye].fov = render_frames[eye]->server_fov[eye];
        }
        else
        {
            layerViews[eye].pose = views[eye].pose;
            layerViews[eye].fov = app->stream.eye_fov[eye];
        }
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

    // Check lobby touch state and call back to Java
    int hit_hand = -1;
    for (int h = 0; h < 2; h++) {
        if (app->lobby.lobby_touch_x[h] >= 0 || app->lobby.lobby_touch_down[h]) {
            hit_hand = h;
            break;
        }
    }

    float tx = -1, ty = -1;
    bool tdown = false, tpressed = false;
    float tthumb = 0;

    if (hit_hand >= 0) {
        tx = app->lobby.lobby_touch_x[hit_hand];
        ty = app->lobby.lobby_touch_y[hit_hand];
        tdown = app->lobby.lobby_touch_down[hit_hand];
        tpressed = app->lobby.lobby_touch_pressed[hit_hand];
        tthumb = app->lobby.lobby_thumbstick_y[hit_hand];
    } else if (app->lobby.head_touch_x >= 0 || app->lobby.head_touch_down) {
        tx = app->lobby.head_touch_x;
        ty = app->lobby.head_touch_y;
        tdown = app->lobby.head_touch_down;
        tpressed = app->lobby.head_touch_pressed;
        hit_hand = -2;
    }

    bool state_changed = (hit_hand != prev_touch_hand) ||
                         (tdown != prev_touch_down) ||
                         (tx != prev_touch_x) ||
                         (ty != prev_touch_y);

    if (hit_hand >= 0 || state_changed) {
        static int jni_log_count = 0;
        if (jni_log_count++ % 30 == 0 || tpressed) {
            LOGI("JNI_TOUCH hit_hand=%d tx=%.1f ty=%.1f tdown=%d tpressed=%d tthumb=%.2f state_changed=%d",
                 hit_hand, tx, ty, (int)tdown, (int)tpressed, tthumb, (int)state_changed);
        }
        if (g_jvm && g_activity && g_onLobbyTouchMethod) {
            JNIEnv * env = nullptr;
            bool attached = false;
            if (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
                if (g_jvm->AttachCurrentThread(&env, nullptr) == JNI_OK)
                    attached = true;
            }
            if (env) {
                env->CallVoidMethod(g_activity, g_onLobbyTouchMethod, tx, ty, tdown, tpressed, tthumb);
            } else {
                LOGE("JNI_TOUCH failed to get JNIEnv for callback");
            }
            if (attached)
                g_jvm->DetachCurrentThread();
        } else {
            LOGE("JNI_TOUCH missing callback g_jvm=%p g_activity=%p g_onLobbyTouchMethod=%p",
                 g_jvm, g_activity, g_onLobbyTouchMethod);
        }
    }

    prev_touch_hand = hit_hand;
    prev_touch_down = tdown;
    prev_touch_x = tx;
    prev_touch_y = ty;

    const XrCompositionLayerBaseHeader* layerHeaders[1] = {
        reinterpret_cast<XrCompositionLayerBaseHeader*>(&layers[0])
    };

    XrFrameEndInfoEXT fePico = {};
    fePico.type = XR_TYPE_FRAME_END_INFO;
    fePico.useHeadposeExt = 1;
    fePico.gsIndex = vsPico.gsIndex;

    XrFrameEndInfo fe = {};
    fe.type = XR_TYPE_FRAME_END_INFO;
    fe.next = &fePico;
    fe.displayTime = fs.predictedDisplayTime;
    fe.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fe.layerCount = 1;
    fe.layers = layerHeaders;

    if (app->stream.streaming.load() && !app->stream.stream_ui_visible.load())
    {
        struct timespec submit_ts;
        clock_gettime(CLOCK_MONOTONIC, &submit_ts);
        int64_t submit_ns = (int64_t)submit_ts.tv_sec * 1000000000LL + submit_ts.tv_nsec;
        for (int e = 0; e < 2; e++)
        {
            if (render_frames[e] && render_frames[e]->valid)
                g_latency.on_frame_submitted(render_frames[e]->frame_index, e, submit_ns);
        }
    }

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

    // Collect detailed stats for the stats screen every ~200ms
    g_stats_frame_count++;
    {
        struct timespec stats_now;
        clock_gettime(CLOCK_MONOTONIC, &stats_now);
        int64_t stats_now_ns = (int64_t)stats_now.tv_sec * 1000000000LL + stats_now.tv_nsec;

        if (g_stats_last_ns == 0)
            g_stats_last_ns = stats_now_ns;

        int64_t stats_elapsed = stats_now_ns - g_stats_last_ns;
        if (stats_elapsed >= 200000000LL)
        {
            float dt = stats_elapsed * 1e-9f;
            int fps = (int)(g_stats_frame_count / dt);
            g_stats_frame_count = 0;
            g_stats_last_ns = stats_now_ns;

            uint64_t rx = app->stream.session ? app->stream.session->bytes_received() : 0;
            uint64_t tx = app->stream.session ? app->stream.session->bytes_sent() : 0;
            float bw_rx = (float)(rx - g_stats_prev_bytes_rx) / dt;
            float bw_tx = (float)(tx - g_stats_prev_bytes_tx) / dt;
            g_stats_prev_bytes_rx = rx;
            g_stats_prev_bytes_tx = tx;

            g_stats_bw_rx_smooth = 0.7f * g_stats_bw_rx_smooth + 0.3f * bw_rx;
            g_stats_bw_tx_smooth = 0.7f * g_stats_bw_tx_smooth + 0.3f * bw_tx;

            float cpu_ms = (ts_diff(t2, t1) + ts_diff(t3, t2)) * 1000.0f;
            float gpu_ms = gl_draw_ms;

            int64_t avg_latency_ns = g_latency.get_avg_total_latency_ns();
            float total_latency_ms = avg_latency_ns > 0 ? avg_latency_ns / 1e6f : 0;

            float bitrate_mbps = (float)app->stream.current_bitrate_mbps.load();

            // Get latency breakdown from the latency tracker
            float encode_ms = 0, send_ms = 0, network_ms = 0, decode_ms = 0, render_wait_ms = 0, blit_ms = 0;
            {
                auto breakdown = g_latency.get_avg_breakdown_ms();
                encode_ms = breakdown[0];
                send_ms = breakdown[1];
                network_ms = breakdown[2];
                decode_ms = breakdown[3];
                render_wait_ms = breakdown[4];
                blit_ms = breakdown[5];
            }

            // Pack into float array for JNI
            float stats_data[] = {
                (float)fps,
                total_latency_ms,
                g_stats_bw_rx_smooth * 8,
                g_stats_bw_tx_smooth * 8,
                bitrate_mbps,
                cpu_ms,
                gpu_ms,
                encode_ms,
                send_ms,
                network_ms,
                decode_ms,
                render_wait_ms,
                blit_ms
            };
            app->stream.notify_stream_stats_detailed(stats_data, 13);
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
    app->stream.tracker.recenter_height();
    if (g_pfnXrResetSensorPICO && app->session) {
        XrResult rr = g_pfnXrResetSensorPICO(app->session, XR_RESET_ALL);
        LOGI("xrResetSensorPICO result: %d", rr);
    }
    if (app->localSpace) {
        xrDestroySpace(app->localSpace);
        app->localSpace = XR_NULL_HANDLE;
        LOGI("old localSpace destroyed");
    }
    if (app->viewSpace) {
        xrDestroySpace(app->viewSpace);
        app->viewSpace = XR_NULL_HANDLE;
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
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    rr = xrCreateReferenceSpace(app->session, &rsci, &app->viewSpace);
    if (rr != XR_SUCCESS)
        LOGW("xrCreateReferenceSpace VIEW after recenter failed: %d", rr);
    app->lobby.recenter();
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
        if (code == AKEYCODE_CALL || code == AKEYCODE_DPAD_CENTER || code == AKEYCODE_ENTER) {
            g_ok_pressed.store(action == AKEY_EVENT_ACTION_DOWN);
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
    bool attached = false;
    if (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        if (g_jvm->AttachCurrentThread(&env, nullptr) == JNI_OK)
            attached = true;
    }
    if (env) {
        g_activity = env->NewGlobalRef(androidApp->activity->clazz);
        jclass clazz = env->GetObjectClass(g_activity);
        g_activityClass = (jclass)env->NewGlobalRef(clazz);
        g_onLobbyTouchMethod = env->GetMethodID(clazz, "onLobbyTouch", "(FFZZF)V");
        LOGI("JNI refs: g_activity=%p g_onLobbyTouchMethod=%p", g_activity, g_onLobbyTouchMethod);

        jclass ctxClass = env->FindClass("android/content/Context");
        jmethodID getSP = env->GetMethodID(ctxClass, "getSharedPreferences", "(Ljava/lang/String;I)Landroid/content/SharedPreferences;");
        jstring spName = env->NewStringUTF("wivrn_settings");
        jobject sp = env->CallObjectMethod(g_activity, getSP, spName, 0);
        env->DeleteLocalRef(spName);

        if (sp) {
            jclass spClass = env->GetObjectClass(sp);
            jmethodID getInt = env->GetMethodID(spClass, "getInt", "(Ljava/lang/String;I)I");
            jmethodID getBool = env->GetMethodID(spClass, "getBoolean", "(Ljava/lang/String;Z)Z");

            jstring keyRes = env->NewStringUTF("res_width");
            int savedRes = env->CallIntMethod(sp, getInt, keyRes, 1664);
            env->DeleteLocalRef(keyRes);

            jstring keyTcp = env->NewStringUTF("tcp_only");
            bool savedTcp = env->CallBooleanMethod(sp, getBool, keyTcp, false);
            env->DeleteLocalRef(keyTcp);

            jstring keyLr = env->NewStringUTF("lower_res_wireless");
            bool savedLr = env->CallBooleanMethod(sp, getBool, keyLr, false);
            env->DeleteLocalRef(keyLr);

            jstring keyDb = env->NewStringUTF("dynamic_bitrate");
            bool savedDb = env->CallBooleanMethod(sp, getBool, keyDb, true);
            env->DeleteLocalRef(keyDb);

            jstring keyBr = env->NewStringUTF("bitrate");
            int savedBr = env->CallIntMethod(sp, getInt, keyBr, 50);
            env->DeleteLocalRef(keyBr);

            int effRes = savedRes;
            if (savedLr && !savedTcp)
                effRes = std::min(savedRes, 1280);
            int effH = effRes * 2160 / 2048;
            effRes = (effRes / 2) * 2;
            effH = (effH / 2) * 2;
            app.stream.eye_width.store(effRes);
            app.stream.eye_height.store(effH);
            app.stream.stream_eye_width.store(effRes);
            app.stream.stream_eye_height.store(effH);
            app.stream.dynamic_bitrate_enabled.store(savedDb);
            app.stream.bitrate_mbps.store(savedBr);
            app.stream.max_bitrate_mbps.store(savedBr);
            app.stream.current_bitrate_mbps.store(savedBr);
            LOGI("Initial resolution from settings: %dx%d (tcp=%d lr=%d db=%d br=%d)",
                effRes, effH, savedTcp, savedLr, savedDb, savedBr);

            env->DeleteLocalRef(spClass);
            env->DeleteLocalRef(sp);
        }
        env->DeleteLocalRef(ctxClass);
        env->DeleteLocalRef(clazz);
    } else {
        LOGE("Failed to get JNIEnv for JNI ref setup");
    }
    if (attached)
        g_jvm->DetachCurrentThread();

    app.stream.vm = g_jvm;
    app.stream.activity = g_activity;

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

    initEyeTracking();

    int eye_w = app.swapchains[0].width;
    int eye_h = app.swapchains[0].height;
    app.lobby.init(eye_w, eye_h);
    app.blit.init(app.display, eye_w, eye_h);
    app.stream.blit_pipeline.init(eye_w, eye_h);
    app.stream.eye_width.store(eye_w);
    app.stream.eye_height.store(eye_h);
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
        g_onLobbyTouchMethod = nullptr;
    }
    g_jvm = nullptr;

    LOGI("Main loop exited");
}
