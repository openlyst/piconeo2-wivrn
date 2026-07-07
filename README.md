# Pico Neo 2 WiVRn
Native WiVRn client for the Pico Neo 2 and Pico Neo 2 Eye.

Got questions or want to contribute? Join the Discord for dev discussion and help: [https://discord.gg/RQ9nSpmtfU](https://discord.gg/RQ9nSpmtfU).

## Status
### Beta

## Supported headsets
- Pico Neo 2
- Pico Neo 2 Eye* (not tested no eye tracking)

## Features
- 6/3 DOF Tracking
- Controller inputs
- Native client and UI (OpenXR)
- Controller models with pointer rays in lobby
- Vibration
- Audio

### Getting the Pico OpenXR SDK

You need the Pico Neo 2 OpenXR SDK — it's not bundled here because of licensing.

1. Sign up at [https://developer.picoxr.com/](https://developer.picoxr.com/) or download from [https://archive.org/details/pico-neo-2-sdks-exes.-7z](https://archive.org/details/pico-neo-2-sdks-exes.-7z)
2. Download the OpenXR SDK (v1.0.13 or newer)
3. Copy `libopenxr_loader.so` from `OpenXR/v1.0.13/OpenXR/Libs/Android/arm64-v8a/` to `client/pico_oxr/app/src/main/cpp/lib/arm64-v8a/`
4. Copy the OpenXR headers from `OpenXR/v1.0.13/OpenXR/Include/openxr/` to `client/pico_oxr/app/src/main/cpp/openxr/` (or use the bundled headers)

If you forget this step, the build will yell at you with a message telling you exactly what to do.

### Building macOS/Linux

```bash
export ANDROID_HOME=/path/to/Android/sdk
export ANDROID_SDK_ROOT=$ANDROID_HOME
export ANDROID_NDK_ROOT=$ANDROID_HOME/ndk/28.2.13676358
export JAVA_HOME=/opt/homebrew/opt/openjdk@17/libexec/openjdk.jdk/Contents/Home
export PATH="$JAVA_HOME/bin:$PATH"

cd client/pico_oxr
./gradlew assembleDebug
```
The APK ends up in `app/build/outputs/apk/debug/` (or `release/`).

### Acknowledgments
- [WiVRn](https://github.com/Vrixyz/WiVRn) - The main project that this is based on
- [Juspertinry - ALVR](https://github.com/Juspertinry/alvr-pico-legacy) - rendering a 3d object.
- [Pico SDK](https://developer.picoxr.com/) - The Pico SDK for the Pico Neo 2*