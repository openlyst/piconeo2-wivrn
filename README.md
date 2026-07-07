# Pico Neo 2 WiVRn

Native WiVRn client for the Pico Neo 2 and Pico Neo 2 Eye. Stream PC VR games to your headset over Wi-Fi or USB with low latency.

> [!NOTE]
> This project is in **Beta**. It works but expect some rough edges.

## Supported headsets

| Headset | Status |
|---------|--------|
| Pico Neo 2 | Working |
| Pico Neo 2 Eye | Untested (no eye tracking) |

## Features

- 6DOF and 3DOF head tracking
- Controller inputs
- OpenXR
- Audio streaming
- Overlay application support 

## Prerequisites

### Get the Pico OpenXR SDK

The Pico Neo 2 OpenXR SDK is not bundled due to licensing.

1. Sign up at [https://developer.picoxr.com/](https://developer.picoxr.com/) or download from [Internet Archive](https://archive.org/details/pico-neo-2-sdks-exes.-7z)
2. Get the OpenXR SDK (v1.0.13 or newer)
3. Copy `libopenxr_loader.so` from `OpenXR/v1.0.13/OpenXR/Libs/Android/arm64-v8a/` to `client/pico_oxr/app/src/main/cpp/lib/arm64-v8a/`
4. Copy the OpenXR headers from `OpenXR/v1.0.13/OpenXR/Include/openxr/` to `client/pico_oxr/app/src/main/cpp/openxr/` (or use the bundled headers)

If you skip this step, the build will fail with a message telling you exactly what's missing.

### Environment

- Android SDK with NDK 28.2.13676358 (or compatible)
- JDK 17
- CMake
- Android device with USB debugging enabled

## Building

```bash
export ANDROID_HOME=/path/to/Android/sdk
export ANDROID_SDK_ROOT=$ANDROID_HOME
export ANDROID_NDK_ROOT=$ANDROID_HOME/ndk/28.2.13676358
export JAVA_HOME=/path/to/jdk-17
export PATH="$JAVA_HOME/bin:$PATH"

cd client/pico_oxr
./gradlew assembleDebug
```

The APK will be in `app/build/outputs/apk/debug/` (or `release/` for release builds).

## Installing

```bash
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

## Usage

1. Install the WiVRn server on your PC ([WiVRn server setup](https://github.com/Vrixyz/WiVRn))
2. Launch the app on your headset
3. Pair with your PC by entering the PIN shown on screen
4. Stream VR games over Wi-Fi or USB

## Community

Got questions or want to contribute? Join the Discord: [https://discord.gg/RQ9nSpmtfU](https://discord.gg/RQ9nSpmtfU)

## Acknowledgments

- [WiVRn](https://github.com/Vrixyz/WiVRn) — The upstream project this client is based on
- [ALVR Pico Legacy](https://github.com/Juspertinry/alvr-pico-legacy) — Rendering reference for the Pico Neo 2
- [Pico SDK](https://developer.picoxr.com/) — Pico Neo 2 SDK