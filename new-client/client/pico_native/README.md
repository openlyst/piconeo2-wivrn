# WiVRn Pico Neo 2 Client

VR streaming client for Pico Neo 2 using PvrSDK-Native.

## Prerequisites

1. **Pico Neo 2 SDK (PvrSDK-Native)**
   - Register at https://developer.picoxr.com/
   - Download PvrSDK-Native 2.8.5.4 or compatible
   - Extract `PvrSDK-Native-release.aar` to `external/pvrsdk-native/`
   - Extract `libPvr_NativeSDK.so` to `external/pvrsdk-native/jni/arm64-v8a/`

2. **Android NDK** version 28.2.13676358 or later

3. **CMake** 3.31.5 or later

4. **Gradle** 8.13 (wrapper included)

## Build

```bash
cd new-client/client/pico_native
./gradlew assembleDebug
```

The APK will be at `build/outputs/apk/debug/wivrn-neo2-debug.apk`.

## Install

```bash
adb install build/outputs/apk/debug/wivrn-neo2-debug.apk
```

## Usage

1. Launch "WiVRn Neo2" from the Pico app library
2. On your PC, run the WiVRn server
3. On the headset, open the app and enter the server IP (or use a `wivrn://` URI)
4. Enter the PIN shown on the PC server

## Architecture

- `neo2_session.h/.cpp` — WiVRn protocol session (TCP control + UDP stream)
- `neo2_video_decoder.h/.cpp` — Hardware H.264/H.265 decoder (MediaCodec + AImageReader)
- `neo2_audio.h/.cpp` — Audio output/input (AAudio)
- `neo2_blit.h/.cpp` — GLES2 blit pipeline for rendering decoded frames
- `neo2_client.h/.cpp` — Main client logic (connection, tracking, network I/O)
- `neo2_jni.cpp` — JNI entry points for Android Activity lifecycle
- `java/.../MainActivity.java` — Pico VRActivity implementation
- `common/` — WiVRn protocol library (vendored from WiVRn common)
