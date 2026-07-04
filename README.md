# Pico Neo 2 WiVRn
A native VR client for the Pico Neo 2 and Pico Neo 2 Eye that allows PCVR gameplay over WiFi and USB for Linux.

Got questions or want to contribute? Join the Discord for dev discussion and help: [https://discord.gg/RQ9nSpmtfU](https://discord.gg/RQ9nSpmtfU) and head to the ``wivrn`` channel.

## Status
### Experimental
This project is really early and still has issues with the code. The project does work but wouldn't say playable. 

### Can I still play?
No sorry, this is not a usable client for now please us this ALVR port instead: [https://github.com/Juspertinry/alvr-pico-legacy](https://github.com/Juspertinry/alvr-pico-legacy-2)

## Supported headsets
- Pico Neo 2
- Pico Neo 2 Eye* (not tested no eye tracking)

### Todo before a 1.0

#### Tweaks
- [ ] Killing a applcation doesn't apply a visual indiactor.
- [ ] Laucning a applcation doesnt apply a visual indiactor.
- [ ] Remove the copyed ALVR grid for something custom. 

#### Known bugs
- [ ] Client will crash if you pair from the clint rather then server (tested on usb)
- [ ] Reprojection is broken causing some people to get sick

#### Features
- [ ] Create a gitlab builder
- [ ] Implament audio
- [ ] Implament controller haptics 
- [ ] Add i18n for Chinese Simplfed and English so we aint hardcoding.

### Getting the Pico SDK

You need the Pico Neo 2 native SDK — it's not bundled here because of licensing.

1. Sign up at [https://developer.picoxr.com/](https://developer.picoxr.com/) or download from [https://archive.org/details/pico-neo-2-sdks-exes.-7z](https://archive.org/details/pico-neo-2-sdks-exes.-7z)
2. Download the Android Native SDK (PvrSDK-Native 2.8.5.4 or newer)
3. Drop `PvrSDK-Native-release.aar` into `external/pvrsdk-native/`
4. Extract the `jni/` folder so you end up with:
   `external/pvrsdk-native/jni/arm64-v8a/libPvr_NativeSDK.so`

If you forget this step, the build will yell at you with a message telling you
exactly what to do.

### Building

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

### Building on Linux

<!-- TODO: Write Linux build instructions -->


### Acknowledgments
- [WiVRn](https://github.com/Vrixyz/WiVRn) - The main project that this is based on
- [Juspertinry - ALVR](https://github.com/Juspertinry/alvr-pico-legacy) - stolen grid and rendering a 3d object.
- [Pico SDK](https://developer.picoxr.com/) - The Pico SDK for the Pico Neo 2*