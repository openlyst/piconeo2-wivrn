// JNI entry points called from MainActivity. Thin: they latch input / publish
// state into the shared modules and start/stop the render thread; all the work
// happens on the render thread (render_thread.cpp).
#include <jni.h>
#include <android/native_window_jni.h>
#include <pthread.h>
#include <mutex>
#include <cstdio>
#include <vector>

#include "render_thread.h"   // gVM/gActivity/gVrClass/gRunning/gThread, setWindow, renderThread, gSleepReq
#include "input.h"           // CtrlState, gCtrl/gCtrlMutex, gHeadData/gHeadMutex
#include "app_state.h"       // gOkHeld/gSideHeld/gOkClick
#include "lobby.h"
#include "log.h"

static jmethodID g_onLobbyTouchMethod = nullptr;

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
        if (!p.pending) return JNI_FALSE;
        amp = p.amplitude; ms = p.durationMs;
        p.pending = false; p.amplitude = 0.0f; p.durationMs = 0;
    }
    float buf[2] = { amp, (float) ms };
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

// Update the lobby UI texture from Java-side Bitmap pixels (RGBA bytes).
extern "C" JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_neo2_pvr_MainActivity_nativeUpdateLobbyTexture(
        JNIEnv *env, jobject thiz, jbyteArray pixels, jint width, jint height) {
    (void) thiz;
    if (!gLobby || !pixels) return;
    jsize n = env->GetArrayLength(pixels);
    if (n < width * height * 4) return;
    std::vector<uint8_t> buf(n);
    env->GetByteArrayRegion(pixels, 0, n, (jbyte *)buf.data());
    gLobby->update_texture(width, height, buf.data());
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

// Recenter the lobby panel in front of the user.
extern "C" JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_neo2_pvr_MainActivity_nativeRecenter(JNIEnv *env, jobject thiz) {
    (void) thiz;
    if (!gLobby) return;
    float head[7] = {0,0,0,1,0,0,0};
    {
        std::lock_guard<std::mutex> lk(gHeadMutex);
        for (int i = 0; i < 7; i++) head[i] = gHeadData[i];
    }
    float qx = head[0], qy = head[1], qz = head[2], qw = head[3];
    float yaw = std::atan2f(2.0f * (qw * qz + qx * qy), 1.0f - 2.0f * (qy * qy + qz * qz));
    gLobby->recenter(&head[4], yaw);
}


