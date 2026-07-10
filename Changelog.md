# RC6 (unreleased)

 - Add eye tracking support for Pico Neo 2 EYE via libPvr_NativeSDK (gaze forwarded for server-side foveated encoding)
 - Add pupil dilation tracking (encoded in unused fb_face2 blendshape slots 6/7, mm/10 — OSC bridge should multiply by 10)
 - Fix eye tracking not re-registering after removing and putting the headset back on
 - Fix audio stutter by moving video shard processing and latency/stutter tracking off the network thread onto the decoder input thread
 - Add dynamic resolution support: swapchain is recreated on the fly when resolution changes, both pre-connect and during streaming
 - Add frame stall detection: if no new frames arrive for 2s the stream UI overlay appears, and after 5s an automatic reconnect is triggered
 - Fix floaty controller tracking: reduce velocity filter time constants (100ms/50ms → 20ms), increase prediction scale (0.4 → 0.7), and boost motion prediction by 20ms to cover full pipeline latency
 - Improve video quality: request 10-bit encoding to eliminate banding artifacts, raise bitrate cap from 100 to 200 Mbps, raise dynamic bitrate floor from 5 to 20 Mbps
 - Remove non-functional high power mode option from settings
 - Fix USB connection failing due to duplicate connect intents tearing down in-progress connections
 - Fix streaming UI not appearing when connecting from wivrn-dashboard due to JNI callback refs being set too late
 - Fix disconnect button not working on the connecting/reconnecting screen
 - Fix client showing reconnecting screen instead of disconnecting when server drops connection
 - Fix USB connect intent not overriding disconnected/reconnecting state
 - Lower default resolution to 1664x1756 to reduce GPU load and bandwidth
 - Swapchain resolution now reads from user settings instead of being hardcoded to 2048x2160
 - Add "Lower resolution for wireless" option to cap wireless streaming at 1280
 - Add dynamic bitrate adjustment that adapts to network conditions in real time
 - Add "Dynamic bitrate" toggle in settings (enabled by default)
 - Settings page is now the same either streaming or in lobby.

# [RC5](https://gitlab.com/HttpAnimations/piconeo2-wivrn/-/tree/9d99acc0dcf0d58008930e10f6eda1fcabde2896)

 - Fix lobby flickering if your battery was low
 - Fix lobby resolution scaling
 - Fix trigger clicking the wrong settings item (was offset 35px too high)
 - Fix trigger slightly too high when activating in settings.
 - Improve stuttering.
 - Set default bitrate to 50.
 - Add bitrate slider to settings.
 - Harden USB pairing and disconnection handling.
 - Fix disconnecting causing the app to hang/crash.

# [RC4](https://gitlab.com/HttpAnimations/piconeo2-wivrn/-/tree/13457b85ce3a656dd60ace3279cdf1a2d05f2313)

 - Harden rendering pipeline
 - Fix controller face buttons
 - Fix controller trigger buttons
 - Fix foveation causing apps to render upside down (sorry aus)
 - Doubles tracking frequency to 300Hz
 - Remove AV1
 - Adds the abilty to change software IPD
 - Fixed sliders in settings
 - Streams at native 2048x2160 resolution
 - Resolution slider is now based on res's not %'s
 - Adds WiVRn icon
 - Fix close button not working in the lobby for pairing 
 - Servers are now remembered after pairing with autoconnect support
 - Adds support for boundrys
 - Adds support for microphone input
 - Harden pairing/connecting over usb
 
# RC3

- Fixes all stuttering and jitters 
- Fix the memory leak 

# RC2 

### New
- Port to openxr
- Fix tracking
- Fix height
- Abilty to recnerter
- Audio support
- Remove lobby floour

### Known bugs
- Holding down buttons can get stuck
- Stream 1 has a stutter however way less then the PVR version
- Pointer in lobby doesnt work you have to pair from the PC
- Beat saber might be flipped upside down depding on your pc

# RC1 (PVR)
- Initial release
