# WiVRn Pico Native Client

This is a standalone WiVRn client for the Pico Neo 2. It uses the PvrSDK-Native
(GLES, not OpenXR) since the Neo 2 doesn't have OpenXR support.

The client builds on its own — you don't need the WiVRn server or root build
system. Everything it needs from the WiVRn common library is vendored under
`common/`.

## What you need

- Android SDK (API 34 for compile, 26 minimum)
- Android NDK 28.2.13676358 (or whatever compatible version you have)
- CMake 3.31.5
- JDK 17 — Gradle 8 won't work with newer JDKs, so stick with 17
- Perl (OpenSSL's Configure script needs it)

On macOS, the easy way to get set up:

```bash
brew install openjdk@17 cmake pkg-config perl
sudo ln -sfn /opt/homebrew/opt/openjdk@17/libexec/openjdk.jdk /Library/Java/JavaVirtualMachines/openjdk-17.jdk
```

Grab the NDK and CMake through Android Studio or sdkmanager:

```bash
sdkmanager "ndk;28.2.13676358" "cmake;3.31.5"
```

## Getting the Pico SDK

You need the Pico Neo 2 native SDK — it's not bundled here because of licensing.

1. Sign up at https://developer.picoxr.com/
2. Download the Android Native SDK (PvrSDK-Native 2.8.5.4 or newer)
3. Drop `PvrSDK-Native-release.aar` into `external/pvrsdk-native/`
4. Extract the `jni/` folder so you end up with:
   `external/pvrsdk-native/jni/arm64-v8a/libPvr_NativeSDK.so`

If you forget this step, the build will yell at you with a message telling you
exactly what to do.

## Building

```bash
export ANDROID_HOME=/path/to/Android/sdk
export ANDROID_SDK_ROOT=$ANDROID_HOME
export ANDROID_NDK_ROOT=$ANDROID_HOME/ndk/28.2.13676358
export JAVA_HOME=/opt/homebrew/opt/openjdk@17/libexec/openjdk.jdk/Contents/Home
export PATH="$JAVA_HOME/bin:$PATH"

cd client/pico_native
./gradlew assembleDebug
```

For a release build, just swap `assembleDebug` for `assembleRelease`.

The APK ends up in `build/outputs/apk/debug/` (or `release/`).

### Heads up about the first build

The first build is slow — it downloads and compiles OpenSSL, Boost, spdlog,
and the OpenXR loader from source. This takes a couple minutes. After that,
the cache kicks in and rebuilds are quick.

If you want to point the cache somewhere specific:

```bash
./gradlew assembleDebug -Pfetchcontent_base_dir=/path/to/shared-cache
```

## Project layout

- `main.cpp` — PvrSDK init, GLES setup, render loop, tracking, controllers
- `wivrn_client_pico.h/cpp` — Networking (talks to the WiVRn server directly)
- `pico_decoder.cpp` — Decodes incoming video frames
- `pico_audio.cpp` — Audio playback
- `pico_blit.cpp` — GLES blitting for rendering
- `common/` — Vendored bits from the WiVRn common library (sockets, crypto, packets)
- `CMakeLists.txt` — Self-contained build, doesn't touch the WiVRn root
- `build.gradle` — Gradle config for the APK

## Supported headsets

- Pico Neo 2
- Pico Neo 2 Eye (untested, no eye tracking support yet)

## Building on Linux

<!-- TODO: Write Linux build instructions -->

## TODO

- [ ] Haptic feedback forwarding
- [ ] Verify controller 6DOF position tracking
- [ ] Pico Neo 2 Eye support
