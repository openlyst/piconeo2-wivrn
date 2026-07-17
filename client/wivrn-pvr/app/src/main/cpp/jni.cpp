// JNI entry points called from MainActivity. Thin: they latch input / publish
// state into the shared modules and start/stop the render thread; all the work
// happens on the render thread (render_thread.cpp).
#include <jni.h>
#include <android/native_window_jni.h>
#include <pthread.h>
#include <sys/socket.h>
#include <thread>
#include <mutex>
#include <cstdio>
#include <vector>

#include "render_thread.h"   // gVM/gActivity/gVrClass/gRunning/gThread, setWindow, renderThread, gSleepReq
#include "input.h"           // CtrlState, gCtrl/gCtrlMutex, gHeadData/gHeadMutex
#include "app_state.h"       // gOkHeld/gSideHeld/gOkClick
#include "passthrough.h"     // gPassthrough (extern in render_thread.cpp)
#include "lobby.h"
#include "log.h"
#include "pico_sdk.h"        // Pvr_ResetSensor
#include "streaming/streaming_client.h"
#include <ctime>

static jmethodID g_onLobbyTouchMethod = nullptr;

// HMD home button long-press tracking for recenter (mirrors pico_oxr).
static struct timespec g_home_press_ts = {};
static bool g_home_pressed = false;
static constexpr double HOME_RECENTER_THRESHOLD_MS = 500.0;

// Full recenter: tracker height calibration + Pvr sensor reset + lobby panel
// re-anchor. Called from HMD home press, controller home long-press,
// and the Java nativeRecenter entry point. The lobby recenter is deferred to
// the render thread (via lobby_recenter_requested) because it needs the head
// rotation matrix which is only available there.
static void do_full_recenter()
{
    LOGI("full recenter triggered");

    if (g_stream)
    {
        // Capture the current standing height BEFORE the sensor reset zeroes
        // the position. recenter_height() stores head_pos.y + height_offset
        // as the new offset so the server still sees the correct eye height
        // after the reset makes Y=0.
        g_stream->tracker.recenter_height();
        g_stream->tracker.recenter_requested.store(true);
        g_stream->tracker.lobby_recenter_requested.store(true);
    }

    // Full sensor reset: reset position + orientation (including tilt for 3DoF),
    // then recenter the head tracker so the current facing becomes forward.
    Pvr_ResetSensorAll();
    svrRecenterOrientation();
    recenterHeadTrackerAW();
    LOGI("recenter: ResetSensorAll + svrRecenterOrientation + recenterHeadTrackerAW done");
}

// Called once (onCreate) to start the long-lived render thread.
extern "C" JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_neo2_pvr_MainActivity_nativeStart(JNIEnv *env, jobject thiz,
                                                jobject activity) {
    (void) thiz;
    if (gRunning.load()) { LOGI("nativeStart ignored (already running)"); return; }
    env->GetJavaVM(&gVM);
    gActivity = env->NewGlobalRef(activity);
    jclass clazz = env->GetObjectClass(activity);
    g_onLobbyTouchMethod = env->GetMethodID(clazz, "onLobbyTouch", "(FFZZF)V");
    LOGI("nativeStart: onLobbyTouch method=%p", g_onLobbyTouchMethod);
    // VrActivity ships in the Pico system runtime; guard against a firmware that
    // lacks it so callVrStatic() degrades to a no-op instead of dereferencing null.
    jclass localVr = env->FindClass("com/psmart/vrlib/VrActivity");
    if (localVr) {
        gVrClass = (jclass) env->NewGlobalRef(localVr);
    } else {
        env->ExceptionClear();
        gVrClass = nullptr;
        LOGE("VrActivity class not found; Pico VrActivity calls disabled");
    }

    // Create the WiVRn stream client up front so dashboard intents and the lobby
    // can connect as soon as the native side is ready.
    if (!g_stream) {
        g_stream = new streaming_client();
        g_stream->vm = gVM;
        g_stream->activity = gActivity;
        g_stream->tracker.pvr_sensor_mode.store(true);
        // Sync config atomics -> streaming client.
        g_stream->bitrate_mbps.store((int)gWivrnBitrateMbps.load());
        g_stream->max_bitrate_mbps.store((int)gWivrnBitrateMbps.load());
        g_stream->microphone_enabled.store(gWivrnMicrophone.load());
        LOGI("nativeStart: created streaming_client");
    }

    LOGI("nativeStart");
    gRunning.store(true);
    pthread_create(&gThread, nullptr, renderThread, nullptr);
}

// surfaceCreated/Changed -> hand the (new) window to the render thread.
extern "C" JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_neo2_pvr_MainActivity_nativeSurfaceChanged(JNIEnv *env, jobject thiz,
                                                         jobject surface) {
    (void) thiz;
    ANativeWindow *win = ANativeWindow_fromSurface(env, surface);  // ref held until render thread releases
    LOGI("nativeSurfaceChanged window=%p", (void *) win);
    setWindow(win);
}

// surfaceDestroyed -> tell the render thread to drop the surface (pause).
extern "C" JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_neo2_pvr_MainActivity_nativeSurfaceDestroyed(JNIEnv *env, jobject thiz) {
    (void) env; (void) thiz;
    LOGI("nativeSurfaceDestroyed");
    setWindow(nullptr);
}

// Headset key events (forwarded from MainActivity.onKeyDown). The side OK button
// (Pico custom keycode 1001) is the "select" click: the render loop commits the
// Software-IPD value the head-gaze crosshair is currently pointing at. ENTER/
// DPAD_CENTER/BUTTON_A kept as alternates. We only latch an edge here; the actual
// value comes from the gaze-vs-slider hit computed on the render thread.
extern "C" JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_neo2_pvr_MainActivity_nativeKeyEvent(JNIEnv *env, jobject thiz,
                                                   jint keyCode, jboolean down) {
    (void) env; (void) thiz;
    bool isOk = (keyCode == 1001 || keyCode == 66 || keyCode == 23 || keyCode == 96);
    if (isOk) gOkHeld.store(down == JNI_TRUE);   // track held state (gaze EQ drag)
    if (keyCode == 1001) gSideHeld.store(down == JNI_TRUE);  // side button (5s hold = lobby toggle)

    // HMD home button -> full recenter. The Pico system sends repeated key-down
    // events (keyCode=3) for the held home button and may not deliver a key-up
    // (the system intercepts it for the shortcut panel). Fire on the initial
    // press edge only.
    if (keyCode == 3 /* AKEYCODE_HOME */) {
        if (down == JNI_TRUE && !g_home_pressed) {
            g_home_pressed = true;
            LOGI("HMD home button pressed, triggering full recenter");
            do_full_recenter();
        } else if (down == JNI_FALSE) {
            g_home_pressed = false;
        }
        return;
    }

    if (!down) return;
    LOGI("KEY down keyCode=%d", (int) keyCode);
    if (isOk) gOkClick.store(true);
}

// Per-hand controller snapshot pushed from the Java ControllerClient poller.
// hand 0=left/1=right; sensor = float[7]{qx,qy,qz,qw,px,py,pz}; angVel=float[3];
// keys = int[] (touchpad x,y then button slots).
extern "C" JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_neo2_pvr_MainActivity_nativeGetHeadData(
        JNIEnv *env, jobject thiz, jfloatArray out) {
    (void) thiz;
    if (!out) return;
    if (env->GetArrayLength(out) < 7) return;
    float buf[7];
    { std::lock_guard<std::mutex> lk(gHeadMutex); for (int i=0;i<7;i++) buf[i]=gHeadData[i]; }
    env->SetFloatArrayRegion(out, 0, 7, buf);
}

extern "C" JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_neo2_pvr_MainActivity_nativeControllerState(
        JNIEnv *env, jobject thiz, jint hand, jint conn,
        jfloatArray sensor, jfloatArray angVel, jintArray keys) {
    (void) thiz;
    if (hand < 0 || hand > 1) return;
    CtrlState s;
    s.conn = conn;
    if (sensor) {
        jsize n = env->GetArrayLength(sensor);
        float buf[7] = {0,0,0,1,0,0,0};
        env->GetFloatArrayRegion(sensor, 0, n < 7 ? n : 7, buf);
        s.q[0]=buf[0]; s.q[1]=buf[1]; s.q[2]=buf[2]; s.q[3]=buf[3];
        s.pos[0]=buf[4]; s.pos[1]=buf[5]; s.pos[2]=buf[6];
    }
    if (angVel) {
        jsize n = env->GetArrayLength(angVel);
        env->GetFloatArrayRegion(angVel, 0, n < 3 ? n : 3, s.angVel);
    }
    if (keys) {
        jsize n = env->GetArrayLength(keys);
        s.keyCount = n < 16 ? n : 16;
        env->GetIntArrayRegion(keys, 0, s.keyCount, s.keys);
    }
    s.fresh = true;
    { std::lock_guard<std::mutex> lk(gCtrlMutex); gCtrl[hand] = s; }
    if (g_stream) {
        int keys12[12] = {0};
        for (int i = 0; i < s.keyCount && i < 12; i++) keys12[i] = s.keys[i];
        float sensor[7] = { s.q[0], s.q[1], s.q[2], s.q[3], s.pos[0], s.pos[1], s.pos[2] };
        g_stream->tracker.update_controller_from_jni(hand, conn, sensor, s.angVel, keys12);
    }
#ifndef NDEBUG
    // DEV-ONLY controller logging: runs on every ~90Hz per-hand state push, so the
    // snprintf + logcat is pure overhead during streaming. Compiled out of the
    // optimized (-DNDEBUG) build; kept for button-slot mapping in local debug builds.
    // Log full key array whenever a button slot (>=2) changes -> map slots. Runs
    // in the JNI sink so it works in the lobby (not gated on the streaming loop).
    {
        static int  lastK[2][16];
        static bool lkI[2] = { false, false };
        bool ch = !lkI[hand];
        for (int i = 2; i < s.keyCount && i < 16; i++)
            if (s.keys[i] != lastK[hand][i]) ch = true;
        if (ch) {
            lkI[hand] = true;
            for (int i = 0; i < s.keyCount && i < 16; i++) lastK[hand][i] = s.keys[i];
            char kb[160]; int kp = 0;
            for (int i = 0; i < s.keyCount && i < 16; i++)
                kp += snprintf(kb+kp, sizeof(kb)-kp, "%d ", s.keys[i]);
            LOGI("CTRLKEYS[%d] n=%d: %s", (int)hand, s.keyCount, kb);
        }
    }
    static int lc[2] = {0,0};
    if ((lc[hand]++ % 30) == 0) {
        LOGI("CTRL[%d] conn=%d q=(%.2f,%.2f,%.2f,%.2f) p=(%.2f,%.2f,%.2f) av=(%.2f,%.2f,%.2f) keys[0..4]=%d,%d,%d,%d,%d (n=%d)",
             (int)hand, conn, s.q[0],s.q[1],s.q[2],s.q[3], s.pos[0],s.pos[1],s.pos[2],
             s.angVel[0],s.angVel[1],s.angVel[2],
             s.keyCount>0?s.keys[0]:0, s.keyCount>1?s.keys[1]:0, s.keyCount>2?s.keys[2]:0,
             s.keyCount>3?s.keys[3]:0, s.keyCount>4?s.keys[4]:0, s.keyCount);
    }
#endif // NDEBUG
}

// Drain a pending controller rumble for `hand` (0=L/1=R), set by the render
// thread's ALVR HAPTICS handler. Returns true and fills out[0]=amplitude(0..1),
// out[1]=durationMs when a pulse is waiting; the Java poller then calls
// ControllerClient.vibrateCV2ControllerStrength(out[0], (int)out[1], hand).
extern "C" JNIEXPORT jboolean JNICALL
Java_org_meumeu_wivrn_neo2_pvr_MainActivity_nativeDrainHaptic(
        JNIEnv *env, jobject thiz, jint hand, jfloatArray out) {
    (void) thiz;
    if (hand < 0 || hand > 1 || !out || env->GetArrayLength(out) < 2) return JNI_FALSE;
    float amp; int ms;
    {
        std::lock_guard<std::mutex> lk(gHapticMutex);
        PendingHaptic &p = gHaptic[hand];
        if (!p.pending && g_stream) {
            std::lock_guard<std::mutex> hlk(g_stream->haptics_mutex);
            auto &r = g_stream->rumble[hand];
            if (r.active) {
                p.pending = true;
                p.amplitude = r.amplitude;
                p.durationMs = r.duration_ms;
                r.active = false;
                r.amplitude = 0.0f;
                r.duration_ms = 0;
            }
        }
        if (!p.pending) return JNI_FALSE;
        amp = p.amplitude; ms = p.durationMs;
        p.pending = false; p.amplitude = 0.0f; p.durationMs = 0;
    }
    float buf[2] = { amp * gWivrnCtrlVibration.load(), (float) ms };
    env->SetFloatArrayRegion(out, 0, 2, buf);
    return JNI_TRUE;
}

// Proximity sleep toggle from the Java proximity listener. true = off-head timeout
// elapsed -> the render thread pauses the stream; false = donned -> resume.
extern "C" JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_neo2_pvr_MainActivity_nativeSetSleep(JNIEnv *env, jobject thiz, jboolean sleep) {
    (void) env; (void) thiz;
    gSleepReq.store(sleep == JNI_TRUE);
    LOGI("nativeSetSleep(%d)", (int)(sleep == JNI_TRUE));
}

// Test hook (fired from adb via a broadcast, see MainActivity): arm the low-battery
// popup at `pct` without waiting for a real battery crossing. Sets exactly the state
// pollBatteryWarn() would, so the render loop's drawBatteryWarn picks it up.
extern "C" JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_neo2_pvr_MainActivity_nativeTestBatteryWarn(JNIEnv *env, jobject thiz, jint pct) {
    (void) env; (void) thiz;
    int p = (int) pct; if (p < 0) p = 0; if (p > 100) p = 100;
    gBattWarnPct.store(p);
    gBattWarnStartNs.store(nowNs());
    LOGI("nativeTestBatteryWarn(%d) -> low-battery popup armed (adb test hook)", p);
}

extern "C" JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_neo2_pvr_MainActivity_nativeStop(JNIEnv *env, jobject thiz) {
    (void) thiz;
    if (!gRunning.exchange(false)) return;
    pthread_join(gThread, nullptr);
    if (gActivity) { env->DeleteGlobalRef(gActivity); gActivity = nullptr; }
    if (gVrClass)  { env->DeleteGlobalRef(gVrClass);  gVrClass = nullptr; }
    LOGI("nativeStop done");
}

// Update the lobby UI texture from Java-side Bitmap pixels (ARGB ints).
// The per-pixel ARGB→RGBA conversion is done here in C++ instead of a slow
// Java loop — 1.26M iterations in interpreted Java was the UI framerate killer.
extern "C" JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_neo2_pvr_MainActivity_nativeUpdateLobbyTexture(
        JNIEnv *env, jobject thiz, jintArray pixels, jint width, jint height) {
    (void) thiz;
    if (!gLobby || !pixels) return;
    jsize n = env->GetArrayLength(pixels);
    if (n < width * height) return;
    std::vector<uint32_t> argb(n);
    env->GetIntArrayRegion(pixels, 0, n, (jint *)argb.data());
    gLobby->update_texture_argb(width, height, argb.data());
}

// Push the latest lobby pointer state back to the Java UI thread. Called from the
// native render loop whenever the ray/pose state changes, so the cursor tracks the
// controller/laser every native frame instead of waiting for a Java-side poll.
void push_lobby_touch_to_java(int hand, float x, float y, bool down, bool pressed, float thumbstickY) {
    (void) hand;
    if (!gVM || !gActivity || !g_onLobbyTouchMethod) return;
    JNIEnv *env = nullptr;
    bool attached = false;
    if (gVM->GetEnv((void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if (gVM->AttachCurrentThread(&env, nullptr) == JNI_OK)
            attached = true;
    }
    if (env) {
        env->CallVoidMethod(gActivity, g_onLobbyTouchMethod,
                            x, y,
                            down ? JNI_TRUE : JNI_FALSE,
                            pressed ? JNI_TRUE : JNI_FALSE,
                            thumbstickY);
    }
    if (attached)
        gVM->DetachCurrentThread();
}

// Set the ALVR stream FOV (full per-eye degrees). Mirrors the release-commit slider
// in the old settings_panel.cpp; the render thread picks up gFovDirty and reapplies
// the warp mesh + view params.
extern "C" JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_neo2_pvr_MainActivity_nativeSetFov(
        JNIEnv *env, jobject thiz, jfloat fovDeg) {
    (void) env; (void) thiz;
    float v = fovDeg;
    if (v < kFovMin) v = kFovMin;
    if (v > kFovMax) v = kFovMax;
    gStreamFovDeg.store(v);
    gFovDirty.store(true);
    LOGI("nativeSetFov: %.1f deg", v);
}

extern "C" JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_neo2_pvr_MainActivity_nativeOnLobbyTouch(
        JNIEnv *env, jobject thiz,
        jint hand, jfloat x, jfloat y, jboolean down, jboolean pressed, jfloat thumbstickY) {
    (void) env; (void) thiz;
    if (!gLobby) return;
    if (hand < 0 || hand > 1) return;
    gLobby->lobby_touch_x[hand] = x;
    gLobby->lobby_touch_y[hand] = y;
    gLobby->lobby_touch_down[hand] = (down == JNI_TRUE);
    gLobby->lobby_touch_pressed[hand] = (pressed == JNI_TRUE);
    gLobby->lobby_thumbstick_y[hand] = thumbstickY;
}

// Recenter: tracker height + sensor reset + lobby panel re-anchor.
extern "C" JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_neo2_pvr_MainActivity_nativeRecenter(JNIEnv *env, jobject thiz) {
    (void) env; (void) thiz;
    do_full_recenter();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_meumeu_wivrn_neo2_pvr_MainActivity_nativeReady(JNIEnv *, jobject) {
    return g_stream ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_neo2_pvr_MainActivity_nativeConnect(JNIEnv *env, jobject thiz,
                                                   jstring hostname, jint port, jboolean tcpOnly) {
    (void) thiz;
    if (!g_stream) {
        LOGE("nativeConnect: streaming_client not created");
        return;
    }
    const char *host = env->GetStringUTFChars(hostname, nullptr);
    g_stream->server_host = host;
    g_stream->server_port = port;
    g_stream->tcp_only = (tcpOnly == JNI_TRUE);
    g_stream->shutdown = false;
    g_stream->auto_reconnect.store(false);
    env->ReleaseStringUTFChars(hostname, host);

    LOGI("nativeConnect: %s:%d tcp=%d", g_stream->server_host.c_str(), g_stream->server_port, g_stream->tcp_only ? 1 : 0);
    g_stream->try_connect();
}

extern "C" JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_neo2_pvr_MainActivity_nativeDisconnect(JNIEnv *env, jobject thiz) {
    (void) env; (void) thiz;
    if (!g_stream) return;

    LOGI("nativeDisconnect");
    g_stream->shutdown = true;
    g_stream->auto_reconnect.store(false);
    if (g_stream->session)
    {
        int fd = g_stream->session->get_control_fd();
        ::shutdown(fd, SHUT_RDWR);
    }
    std::thread([s = g_stream] {
        std::lock_guard lock(s->connect_mutex);
        if (s->connect_thread.joinable())
            s->connect_thread.join();
        if (s->network_thread.joinable())
            s->network_thread.join();
        s->session.reset();
        s->reset_stream_state();
        s->shutdown = false;
    }).detach();
}

extern "C" JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_neo2_pvr_MainActivity_nativeSetPin(JNIEnv *env, jobject thiz, jstring pin) {
    (void) thiz;
    if (!g_stream) return;
    const char *p = env->GetStringUTFChars(pin, nullptr);
    g_stream->pairing_pin = p;
    try { g_stream->pin_promise.set_value(p); } catch (...) {}
    env->ReleaseStringUTFChars(pin, p);
}

extern "C" JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_neo2_pvr_MainActivity_nativeSetBitrate(JNIEnv *, jobject, jint bitrate_mbps) {
    if (!g_stream) return;
    LOGI("nativeSetBitrate: %d Mbps", bitrate_mbps);
    g_stream->bitrate_mbps.store(bitrate_mbps);
    g_stream->max_bitrate_mbps.store(bitrate_mbps);
    g_stream->current_bitrate_mbps.store(bitrate_mbps);
    if (g_stream->streaming.load() && g_stream->session)
        g_stream->send_bitrate_change(bitrate_mbps);
}

extern "C" JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_neo2_pvr_MainActivity_nativeSetDynamicBitrate(JNIEnv *, jobject, jboolean enabled) {
    if (!g_stream) return;
    g_stream->dynamic_bitrate_enabled.store(enabled == JNI_TRUE);
    LOGI("Dynamic bitrate %s", enabled == JNI_TRUE ? "enabled" : "disabled");
}

extern "C" JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_neo2_pvr_MainActivity_nativeSetPassthrough(JNIEnv *, jobject, jboolean enabled) {
    gWivrnPassthrough.store(enabled == JNI_TRUE);
    extern pico_passthrough * gPassthrough;
    if (enabled == JNI_TRUE) {
        if (gPassthrough && !gPassthrough->is_camera_on()) gPassthrough->start();
    } else {
        if (gPassthrough && gPassthrough->is_camera_on()) gPassthrough->stop();
    }
    saveAllConfig();
    LOGI("Passthrough %s", enabled == JNI_TRUE ? "enabled" : "disabled");
}

extern "C" JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_neo2_pvr_MainActivity_nativeSetIpd(JNIEnv *, jobject, jfloat ipd_mm) {
    if (!g_stream) return;
    float ipd_m = ipd_mm * 0.001f;
    g_stream->tracker.soft_ipd.store(ipd_m);
    LOGI("Software IPD set to %.1f mm (%.4f m)", ipd_mm, ipd_m);
}

extern "C" JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_neo2_pvr_MainActivity_nativeSetMicrophone(JNIEnv *, jobject, jboolean enabled) {
    if (!g_stream) return;
    g_stream->microphone_enabled.store(enabled == JNI_TRUE);
    if (g_stream->audio_handle)
        g_stream->audio_handle->set_mic_state(enabled == JNI_TRUE);
    LOGI("Microphone %s", enabled == JNI_TRUE ? "enabled" : "disabled");
}

extern "C" JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_neo2_pvr_MainActivity_nativeSetStreamResolution(JNIEnv *, jobject, jint width, jint height) {
    if (!g_stream) return;
    int old_w = g_stream->stream_eye_width.load();
    int old_h = g_stream->stream_eye_height.load();
    g_stream->stream_eye_width.store(width);
    g_stream->stream_eye_height.store(height);
    if ((width != old_w || height != old_h) && g_stream->session)
    {
        g_stream->send_headset_info();
        LOGI("Sent updated stream resolution to server: %dx%d", width, height);
    }
    LOGI("Stream resolution set to %dx%d", width, height);
}

extern "C" JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_neo2_pvr_MainActivity_nativeSetRenderResolution(JNIEnv *, jobject, jint width, jint height) {
    if (!g_stream) return;
    int old_w = g_stream->eye_width.load();
    int old_h = g_stream->eye_height.load();
    g_stream->eye_width.store(width);
    g_stream->eye_height.store(height);
    g_stream->blit_pipeline.set_resolution(width, height);
    if (width != old_w || height != old_h)
        g_stream->resolution_dirty.store(true);
    LOGI("Render resolution set to %dx%d", width, height);
}

extern "C" JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_neo2_pvr_MainActivity_nativeRequestAppList(JNIEnv *, jobject) {
    if (!g_stream || !g_stream->session) return;
    LOGI("nativeRequestAppList");
    try {
        g_stream->session->send_control(wivrn::from_headset::get_application_list{
            .language = "en",
            .country = "US",
            .variant = "",
        });
    } catch (std::exception & e) {
        LOGE("Failed to request app list: %s", e.what());
    }
}

extern "C" JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_neo2_pvr_MainActivity_nativeStartApp(JNIEnv * env, jobject, jstring appId) {
    if (!g_stream || !g_stream->session) return;
    const char * app_id_str = env->GetStringUTFChars(appId, nullptr);
    if (app_id_str) {
        LOGI("nativeStartApp: %s", app_id_str);
        try {
            g_stream->session->send_control(wivrn::from_headset::start_app{
                .app_id = app_id_str,
            });
        } catch (std::exception & e) {
            LOGE("Failed to start app: %s", e.what());
        }
        env->ReleaseStringUTFChars(appId, app_id_str);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_neo2_pvr_MainActivity_nativeRequestRunningApps(JNIEnv *, jobject) {
    if (!g_stream || !g_stream->session) return;
    try {
        g_stream->session->send_control(wivrn::from_headset::get_running_applications{});
    } catch (std::exception & e) {
        LOGE("Failed to request running apps: %s", e.what());
    }
}

extern "C" JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_neo2_pvr_MainActivity_nativeSetActiveApp(JNIEnv *, jobject, jint appId) {
    if (!g_stream || !g_stream->session) return;
    LOGI("nativeSetActiveApp: %d", (int)appId);
    try {
        g_stream->session->send_control(wivrn::from_headset::set_active_application{
            .id = (uint32_t)appId,
        });
    } catch (std::exception & e) {
        LOGE("Failed to set active app: %s", e.what());
    }
}

extern "C" JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_neo2_pvr_MainActivity_nativeStopApp(JNIEnv *, jobject, jint appId) {
    if (!g_stream || !g_stream->session) return;
    LOGI("nativeStopApp: %d", (int)appId);
    try {
        g_stream->session->send_control(wivrn::from_headset::stop_application{
            .id = (uint32_t)appId,
        });
    } catch (std::exception & e) {
        LOGE("Failed to stop app: %s", e.what());
    }
}


