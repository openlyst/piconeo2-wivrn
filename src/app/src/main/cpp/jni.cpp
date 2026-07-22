// JNI entry points called from MainActivity. Thin: latch input / publish state
// and start/stop the render thread.
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
#include "server_list.h"
#include "log.h"
#include "pico_sdk.h"        // Pvr_ResetSensor
#include "streaming/streaming_client.h"
#include <ctime>

static jmethodID g_onServerConnectMethod = nullptr;
static jmethodID g_onServerRemoveMethod = nullptr;
static jmethodID g_onServerAutoconnectMethod = nullptr;
static jmethodID g_onRefreshServersMethod = nullptr;

// HMD home button long-press tracking for recenter (mirrors pico_oxr).
static struct timespec g_home_press_ts = {};
static bool g_home_pressed = false;
static constexpr double HOME_RECENTER_THRESHOLD_MS = 500.0;

// Full recenter: tracker height calibration + Pvr sensor reset + lobby panel
// re-anchor. Lobby recenter is deferred to the render thread (needs head rotation).
static void do_full_recenter()
{
    LOGI("full recenter triggered");

    if (g_stream)
    {
        // Capture standing height BEFORE the sensor reset zeroes position.
        // recenter_height() stores head_pos.y + offset so the server still sees
        // correct eye height after the reset makes Y=0.
        g_stream->tracker.recenter_height();
        g_stream->tracker.recenter_requested.store(true);
        g_stream->tracker.lobby_recenter_requested.store(true);
    }

    // Full sensor reset: position + orientation (including tilt for 3DoF),
    // then recenter head tracker so current facing becomes forward.
    Pvr_ResetSensorAll();
    svrRecenterOrientation();
    recenterHeadTrackerAW();
    LOGI("recenter: ResetSensorAll + svrRecenterOrientation + recenterHeadTrackerAW done");
}

// Called once (onCreate) to start the render thread.
extern "C" JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_neo2_pvr_MainActivity_nativeStart(JNIEnv *env, jobject thiz,
                                                jobject activity) {
    (void) thiz;
    if (gRunning.load()) { LOGI("nativeStart ignored (already running)"); return; }
    env->GetJavaVM(&gVM);
    gActivity = env->NewGlobalRef(activity);
    jclass clazz = env->GetObjectClass(activity);
    g_onServerConnectMethod = env->GetMethodID(clazz, "onServerConnect", "(Ljava/lang/String;IZ)V");
    g_onServerRemoveMethod = env->GetMethodID(clazz, "onServerRemove", "(Ljava/lang/String;I)V");
    g_onServerAutoconnectMethod = env->GetMethodID(clazz, "onServerAutoconnect", "(Ljava/lang/String;I)V");
    g_onRefreshServersMethod = env->GetMethodID(clazz, "onRefreshServers", "()V");
    LOGI("nativeStart: onServerConnect=%p onServerRemove=%p onServerAutoconnect=%p onRefreshServers=%p",
         g_onServerConnectMethod, g_onServerRemoveMethod, g_onServerAutoconnectMethod,
         g_onRefreshServersMethod);

    // Wire the native server list CONNECT button to Java's onServerConnect.
    gOnServerConnect = [](const ServerInfo &s) {
        if (!gVM || !gActivity || !g_onServerConnectMethod) return;
        JNIEnv *env = nullptr;
        bool attached = false;
        if (gVM->GetEnv((void **)&env, JNI_VERSION_1_6) != JNI_OK) {
            if (gVM->AttachCurrentThread(&env, nullptr) == JNI_OK) attached = true;
        }
        if (!env) return;
        jstring jhost = env->NewStringUTF(s.hostname.c_str());
        env->CallVoidMethod(gActivity, g_onServerConnectMethod, jhost, s.port, s.tcp_only);
        env->DeleteLocalRef(jhost);
        if (attached) gVM->DetachCurrentThread();
    };

    // Wire the native server list X (remove) button to Java's onServerRemove.
    gOnServerRemove = [](const std::string &hostname, int port) {
        if (!gVM || !gActivity || !g_onServerRemoveMethod) return;
        JNIEnv *env = nullptr;
        bool attached = false;
        if (gVM->GetEnv((void **)&env, JNI_VERSION_1_6) != JNI_OK) {
            if (gVM->AttachCurrentThread(&env, nullptr) == JNI_OK) attached = true;
        }
        if (!env) return;
        jstring jhost = env->NewStringUTF(hostname.c_str());
        env->CallVoidMethod(gActivity, g_onServerRemoveMethod, jhost, port);
        env->DeleteLocalRef(jhost);
        if (attached) gVM->DetachCurrentThread();
    };

    // Wire the native server list autoconnect toggle to Java's onServerAutoconnect.
    gOnServerAutoconnect = [](const std::string &hostname, int port) {
        if (!gVM || !gActivity || !g_onServerAutoconnectMethod) return;
        JNIEnv *env = nullptr;
        bool attached = false;
        if (gVM->GetEnv((void **)&env, JNI_VERSION_1_6) != JNI_OK) {
            if (gVM->AttachCurrentThread(&env, nullptr) == JNI_OK) attached = true;
        }
        if (!env) return;
        jstring jhost = env->NewStringUTF(hostname.c_str());
        env->CallVoidMethod(gActivity, g_onServerAutoconnectMethod, jhost, port);
        env->DeleteLocalRef(jhost);
        if (attached) gVM->DetachCurrentThread();
    };

    // Wire the native server list Refresh button to Java's onRefreshServers.
    gOnRefreshServers = []() {
        if (!gVM || !gActivity || !g_onRefreshServersMethod) return;
        JNIEnv *env = nullptr;
        bool attached = false;
        if (gVM->GetEnv((void **)&env, JNI_VERSION_1_6) != JNI_OK) {
            if (gVM->AttachCurrentThread(&env, nullptr) == JNI_OK) attached = true;
        }
        if (!env) return;
        env->CallVoidMethod(gActivity, g_onRefreshServersMethod);
        if (attached) gVM->DetachCurrentThread();
    };

    // Wire the EXIT tab to finish the activity.
    gOnExit = []() {
        if (!gVM || !gActivity) return;
        JNIEnv *env = nullptr;
        bool attached = false;
        if (gVM->GetEnv((void **)&env, JNI_VERSION_1_6) != JNI_OK) {
            if (gVM->AttachCurrentThread(&env, nullptr) == JNI_OK) attached = true;
        }
        if (!env) return;
        jclass clazz = env->GetObjectClass(gActivity);
        jmethodID finishMethod = env->GetMethodID(clazz, "finish", "()V");
        if (finishMethod) env->CallVoidMethod(gActivity, finishMethod);
        env->DeleteLocalRef(clazz);
        if (attached) gVM->DetachCurrentThread();
    };
    // VrActivity ships in the Pico system runtime; guard against a firmware that
    // lacks it so callVrStatic() degrades to a no-op instead of crashing.
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
        // Load persisted settings before creating the client so the initial
        // headset_info (sent on connect) reflects the saved mic preference.
        // The render thread also calls loadAllConfig, but that runs later and
        // would race with the connect thread sending headset_info first.
        setHomeFromFilesDir(env, activity);
        loadAllConfig();
        g_stream = new streaming_client();
        g_stream->vm = gVM;
        g_stream->activity = gActivity;
        g_stream->tracker.pvr_sensor_mode.store(true);
        g_stream->bitrate_mbps.store((int)gWivrnBitrateMbps.load());
        g_stream->max_bitrate_mbps.store((int)gWivrnBitrateMbps.load());
        g_stream->microphone_enabled.store(gWivrnMicrophone.load());
        LOGI("nativeStart: created streaming_client (mic=%d)", gWivrnMicrophone.load() ? 1 : 0);
    }

    LOGI("nativeStart");
    gRunning.store(true);
    pthread_create(&gThread, nullptr, renderThread, nullptr);
}

// surfaceCreated/Changed -> hand the window to the render thread.
extern "C" JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_neo2_pvr_MainActivity_nativeSurfaceChanged(JNIEnv *env, jobject thiz,
                                                         jobject surface) {
    (void) thiz;
    ANativeWindow *win = ANativeWindow_fromSurface(env, surface);  // ref held until render thread releases
    LOGI("nativeSurfaceChanged window=%p", (void *) win);
    setWindow(win);
}

// surfaceDestroyed -> tell the render thread to drop the surface.
extern "C" JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_neo2_pvr_MainActivity_nativeSurfaceDestroyed(JNIEnv *env, jobject thiz) {
    (void) env; (void) thiz;
    LOGI("nativeSurfaceDestroyed");
    setWindow(nullptr);
}

// Headset key events (forwarded from MainActivity.onKeyDown). The side OK button
// (Pico keycode 1001) is the "select" click for the Software-IPD slider.
// ENTER/DPAD_CENTER/BUTTON_A are alternates. We only latch an edge here.
extern "C" JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_neo2_pvr_MainActivity_nativeKeyEvent(JNIEnv *env, jobject thiz,
                                                   jint keyCode, jboolean down) {
    (void) env; (void) thiz;
    bool isOk = (keyCode == 1001 || keyCode == 66 || keyCode == 23 || keyCode == 96);
    if (isOk) gOkHeld.store(down == JNI_TRUE);   // gaze EQ drag
    if (keyCode == 1001) gSideHeld.store(down == JNI_TRUE);  // 5s hold = lobby toggle

    // HMD home button -> full recenter. The Pico system sends repeated key-down
    // events for the held home button and may not deliver a key-up. Fire on the
    // initial press edge only.
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
// hand 0=left/1=right; sensor = float[7]{qx,qy,qz,qw,px,py,pz}; keys = int[].
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
    // DEV-ONLY: compiled out of the optimized build. Logs the full key array
    // whenever a button slot (>=2) changes, for mapping slots in debug builds.
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

// Drain a pending controller rumble for `hand` (0=L/1=R). Returns true and fills
// out[0]=amplitude(0..1), out[1]=durationMs when a pulse is waiting.
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

// Proximity sleep toggle. true = off-head -> pause stream; false = donned -> resume.
extern "C" JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_neo2_pvr_MainActivity_nativeSetSleep(JNIEnv *env, jobject thiz, jboolean sleep) {
    (void) env; (void) thiz;
    gSleepReq.store(sleep == JNI_TRUE);
    LOGI("nativeSetSleep(%d)", (int)(sleep == JNI_TRUE));
}

// Test hook (fired from adb via a broadcast): arm the low-battery popup at `pct`.
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

// Sync the discovered server list from Java to the native 3D server list panel.
extern "C" JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_neo2_pvr_MainActivity_nativeSetServerList(
        JNIEnv *env, jobject thiz, jobjectArray names, jobjectArray hosts,
        jintArray ports, jbooleanArray tcpOnly, jbooleanArray discovered,
        jbooleanArray autoconnect) {
    (void) thiz;
    if (!names || !hosts) return;
    jsize n = env->GetArrayLength(names);
    if (n <= 0) { setServerList({}); return; }
    std::vector<ServerInfo> servers(n);
    jint *portsArr = ports ? env->GetIntArrayElements(ports, nullptr) : nullptr;
    jboolean *tcpArr = tcpOnly ? env->GetBooleanArrayElements(tcpOnly, nullptr) : nullptr;
    jboolean *discArr = discovered ? env->GetBooleanArrayElements(discovered, nullptr) : nullptr;
    jboolean *autoArr = autoconnect ? env->GetBooleanArrayElements(autoconnect, nullptr) : nullptr;
    for (jsize i = 0; i < n; i++) {
        jstring jname = (jstring) env->GetObjectArrayElement(names, i);
        jstring jhost = (jstring) env->GetObjectArrayElement(hosts, i);
        const char *cname = jname ? env->GetStringUTFChars(jname, nullptr) : "";
        const char *chost = jhost ? env->GetStringUTFChars(jhost, nullptr) : "";
        servers[i].name = cname;
        servers[i].hostname = chost;
        servers[i].port = portsArr ? portsArr[i] : 0;
        servers[i].tcp_only = tcpArr ? tcpArr[i] : false;
        servers[i].discovered = discArr ? discArr[i] : false;
        servers[i].autoconnect = autoArr ? autoArr[i] : false;
        if (jname) { env->ReleaseStringUTFChars(jname, cname); env->DeleteLocalRef(jname); }
        if (jhost) { env->ReleaseStringUTFChars(jhost, chost); env->DeleteLocalRef(jhost); }
    }
    if (portsArr) env->ReleaseIntArrayElements(ports, portsArr, JNI_ABORT);
    if (tcpArr) env->ReleaseBooleanArrayElements(tcpOnly, tcpArr, JNI_ABORT);
    if (discArr) env->ReleaseBooleanArrayElements(discovered, discArr, JNI_ABORT);
    if (autoArr) env->ReleaseBooleanArrayElements(autoconnect, autoArr, JNI_ABORT);
    setServerList(servers);
}

// Set the ALVR stream FOV (per-eye degrees). Render thread picks up gFovDirty
// and reapplies the warp mesh + view params.
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
    setConnecting(true);
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


