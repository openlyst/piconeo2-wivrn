# Pico Neo 2 WiVRn
Native WiVRn client for the Pico Neo 2 and Pico Neo 2 Eye.

Got questions or want to contribute? Join the Discord for dev discussion and help: [https://discord.gg/RQ9nSpmtfU](https://discord.gg/RQ9nSpmtfU).

## Status
### Alpha
This project is really early and still has issues with the code. But it should stil work.

## Supported headsets
- Pico Neo 2
- Pico Neo 2 Eye* (not tested no eye tracking)

## Features
- 6/3 DOF Tracking
- Controller inputs
- Native client and UI (pvr)
- Vibrataion 
- Audio

### Todo before a 1.0

#### Known bugs
- [ ] Client will crash if you pair from the clint rather then server (tested on usb)

- [ ] Killing a applcation doesn't apply a visual indiactor.
- [ ] Laucning a applcation doesnt apply a visual indiactor.
- [ ] Cursor on the UI doesn't work
- [X] Right eye is slightly jittery compared to the left eye
- [X] Stream will crash while streaming and andriod will say the app is not responing even when it is witch breaks controller input and sends the user to a 2d view
- [X] Reprojection is broken causing some people to get sick 
- [X] UI gets super laggy when active (32bit)

#### Features
- [ ] Create a Gitlab pipeline for building.
- [ ] Add i18n for Chinese Simplfed and English so we aint hardcoding.
- [X] Implement Audio; Dekstop -> Client
- [X] Implement controller haptics

### Getting the Pico SDK

You need the Pico Neo 2 native SDK — it's not bundled here because of licensing.

1. Sign up at [https://developer.picoxr.com/](https://developer.picoxr.com/) or download from [https://archive.org/details/pico-neo-2-sdks-exes.-7z](https://archive.org/details/pico-neo-2-sdks-exes.-7z)
2. Download the Android Native SDK (PvrSDK-Native 2.8.5.4 or newer)
3. Drop `PvrSDK-Native-release.aar` into `external/pvrsdk-native/`
4. Extract the `jni/` folder so you end up with:
   `external/pvrsdk-native/jni/arm64-v8a/libPvr_NativeSDK.so`

If you forget this step, the build will yell at you with a message telling you exactly what to do.

### Building macOS/Linux

```bash
export ANDROID_HOME=/path/to/Android/sdk
export ANDROID_SDK_ROOT=$ANDROID_HOME
export ANDROID_NDK_ROOT=$ANDROID_HOME/ndk/28.2.13676358
export JAVA_HOME=/opt/homebrew/opt/openjdk@17/libexec/openjdk.jdk/Contents/Home
export PATH="$JAVA_HOME/bin:$PATH"

cd client/pico_native
./gradlew assembleDebug
```
The APK ends up in `build/outputs/apk/debug/` (or `release/`).

### Acknowledgments
- [WiVRn](https://github.com/Vrixyz/WiVRn) - The main project that this is based on
- [Juspertinry - ALVR](https://github.com/Juspertinry/alvr-pico-legacy) - rendering a 3d object.
- [Pico SDK](https://developer.picoxr.com/) - The Pico SDK for the Pico Neo 2*