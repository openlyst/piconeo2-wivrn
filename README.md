# Pico Neo 2 WiVRn
Native WiVRn client for the Pico Neo 2 and Pico Neo 2 Eye.

Got questions or want to contribute? Join the Discord for dev discussion and help: [https://discord.gg/RQ9nSpmtfU](https://discord.gg/RQ9nSpmtfU).

## Status
### Experimental
This project is really early and still has issues with the code. The project does work but wouldn't say playable. 

### Can I still play?
No sorry, this is not a usable client for now please us this ALVR port instead: [https://github.com/Juspertinry/alvr-pico-legacy](https://github.com/Juspertinry/alvr-pico-legacy-2)

## Supported headsets
- Pico Neo 2
- Pico Neo 2 Eye* (not tested no eye tracking)

## Features
- [ ] 6/3 DOF Tracking
- [ ] Controller inputs
- [ ] Native client and UI (pvr)

### Todo before a 1.0

#### Tweaks
- [ ] Killing a applcation doesn't apply a visual indiactor.
- [ ] Laucning a applcation doesnt apply a visual indiactor.

#### Known bugs
- [ ] Client will crash if you pair from the clint rather then server (tested on usb)
- [ ] Reprojection is broken causing some people to get sick

#### Cleanup
- [ ] Remove the copyed ALVR grid for something custom. 
- [ ] Clean up the code base 
- [ ] Orginize the files

#### Features
- [ ] Create a Gitlab pipeline for building.
- [ ] Implament Audio; Dekstop -> Client
- [ ] Implament controller haptics
- [ ] Add i18n for Chinese Simplfed and English so we aint hardcoding.

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
- [Juspertinry - ALVR](https://github.com/Juspertinry/alvr-pico-legacy) - stolen grid and rendering a 3d object.
- [Pico SDK](https://developer.picoxr.com/) - The Pico SDK for the Pico Neo 2*