# Pico Neo 2 WiVRn

Native WiVRn client for the Pico Neo 2 and Pico Neo 2 Eye. Stream PC VR games to your headset over Wi-Fi or USB with low latency.

> [!NOTE]
> This project is in **Beta**. It works but expect some rough edges.

## Supported headsets

| Headset | Status |
|---------|--------|
| Pico Neo 2 | Working |
| Pico Neo 2 Eye | Working (eye tracking supported) |

## Features

- 6DOF and 3DOF head tracking
- Controller inputs with 3D model rendering
- Head-gaze laser interaction for the lobby UI (no controllers needed)
- Audio streaming
- Overlay application support
- Eye tracking (Pico Neo 2 Eye only) — gaze direction for foveated encoding, eye blink, look directions, and pupil dilation (encoded in unused fb_face2 blendshape slots 6/7, mm/10)
- In-app settings panel with EQ, brightness, IPD, FOV, and codec/bitrate controls
- Low-latency hardware compositor pipeline with async reprojection

## WiVRn Version

This client is compatible with WiVRn server version **26.6.2**.

## Prerequisites

### Get the Pico SDK

The Pico Neo 2 SDK is not bundled due to licensing.

1. Sign up at [https://developer.picoxr.com/](https://developer.picoxr.com/) or download from [Internet Archive](https://archive.org/details/pico-neo-2-sdks-exes.-7z)
2. Get the Pico Native SDK (the one with `libPvr_UnitySDK.so`, `libtracking_module.so`, `libnative.so`)
3. Copy the prebuilt `.so` files to `client/wivrn-pvr/app/src/main/jniLibs/armeabi-v7a/`
4. Copy `pvr_classes.jar` to `client/wivrn-pvr/app/libs/`

If you skip this step, the build will fail with a message telling you exactly what's missing.

### Environment

- Android SDK with NDK 26.3.11579264
- JDK 17
- CMake 3.22.1
- Pico Neo 2 with USB debugging enabled

## Building

```bash
export ANDROID_HOME=/path/to/Android/sdk
export ANDROID_SDK_ROOT=$ANDROID_HOME
export ANDROID_NDK_ROOT=$ANDROID_HOME/ndk/26.3.11579264
export JAVA_HOME=/path/to/jdk-17
export PATH="$JAVA_HOME/bin:$PATH"

cd client/wivrn-pvr
./gradlew assembleDebug
```

The APK will be in `app/build/outputs/apk/debug/` (or `release/` for signed release builds).

### Signed release builds

Release signing is configured via `keystore.properties` placed one directory above the gradle project (i.e. `client/keystore.properties`):

```properties
storeFile=wivrn-release.keystore
storePassword=your_store_password
keyAlias=your_key_alias
keyPassword=your_key_password
```

The keystore file path is relative to `client/wivrn-pvr/`. If the file is absent, release builds are unsigned.

## Installing

```bash
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

Or grab a pre-built signed APK from the [GitHub releases](https://github.com/openlyst/piconeo2-wivrn/releases).

## Usage

1. Install the WiVRn server on your PC ([WiVRn server setup](https://github.com/WiVRn/WiVRn))
2. Launch the app on your headset
3. Pair with your PC by entering the PIN shown on screen
4. Stream VR games over Wi-Fi or USB

## Community

Got questions or want to contribute? Join the Discord: [https://discord.gg/RQ9nSpmtfU](https://discord.gg/RQ9nSpmtfU)

## Acknowledgments

- [WiVRn](https://github.com/Vrixyz/WiVRn) — The upstream project this client is based on
- [ALVR Pico Legacy](https://github.com/Juspertinry/alvr-pico-legacy) — Rendering reference for the Pico Neo 2
- [Pico SDK](https://developer.picoxr.com/) — Pico Neo 2 SDK
- [NeoRevived](https://openlyst.github.io/neorevived/#/entry/streaming/gitlab.HttpAnimations.piconeo2-wivrn) — Listed on the NeoRevived Pico Neo 2 revival hub
