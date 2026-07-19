#pragma once
// Lobby HUD device strings: LAN IPv4, client status, ALVR hostname, headset model.
#include <jni.h>

extern char gIpText[24];        // "CHECK WI-FI" until a LAN address is found
extern char gStatusText[32];    // DISCONNECTED / SEARCHING / CONNECTING / CONNECTED
extern char gModelText[32];     // e.g. "PICO NEO 2 EYE"
extern char gHostnameText[24];  // ALVR client hostname (parsed from the HUD msg)
extern bool gIsEyeHw;           // true on Neo 2 EYE (pxr.vendorhw.eye == 1)

void toUpperAscii(char *s);
void readHeadsetModel(JNIEnv *env);
void refreshDeviceIp();
