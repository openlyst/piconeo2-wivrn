# RC6 (unreleased)

 - Remove non-functional high power mode option from settings
 - Fix USB connection failing due to duplicate connect intents tearing down in-progress connections
 - Fix streaming UI not appearing when connecting from wivrn-dashboard due to JNI callback refs being set too late

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
