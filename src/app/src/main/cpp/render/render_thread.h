#pragma once
// Public interface of the render thread: lifetime/JNI globals, window hand-off,
// and proximity-sleep flag. Shared between render_thread.cpp and jni.cpp.
#include <jni.h>
#include <pthread.h>
#include <mutex>
#include <atomic>
#include <android/native_window.h>

// JNI + thread lifetime (set in nativeStart, torn down in nativeStop).
extern JavaVM   *gVM;
extern jobject   gActivity;
extern jclass    gVrClass;
extern pthread_t gThread;
extern std::atomic<bool> gRunning;   // render-thread lifetime

// Window handed in by SurfaceView callbacks; render thread owns one long-lived
// EGL display+context and (re)creates only the window surface.
extern std::mutex     gWinMutex;
extern ANativeWindow *gPendingWindow;   // latest window (or null), guarded by gWinMutex
// Atomic so the render loop can read on its fast path without taking gWinMutex.
// gPendingWindow itself stays mutex-guarded.
extern std::atomic<bool> gWindowDirty;  // a change is pending

// Proximity power-sleep: set by Java proximity listener; render loop acts on edge.
extern std::atomic<bool> gSleepReq;

class pico_lobby;
extern pico_lobby * gLobby;

// Hand the render thread a new window (or null when the surface is destroyed).
void  setWindow(ANativeWindow *win);
// Set $HOME from the activity's files dir (needed before any file-based config).
void  setHomeFromFilesDir(JNIEnv *env, jobject activity);
// The long-lived render-thread entry point (started via pthread_create).
void *renderThread(void *);
