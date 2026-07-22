#pragma once
// Internationalisation. String keys map to EN/ZH translations. Language
// can be forced or set to follow the Android system locale.
#include <cstdint>

enum class Lang : int {
    English = 0,
    Chinese = 1,
    System  = 2,
};

// Persisted choice. System resolves to EN or ZH at init via detectSystemLang().
void setLanguage(Lang l);
Lang getLanguageSetting();
Lang getActiveLanguage();   // never System; resolves to EN or ZH

// Detect system language from Android locale. Called once at startup.
void detectSystemLang(const char *locale);

// String keys. Add new ones at the end; indices match the translation arrays.
enum class Str : int {
    // Tabs
    Servers,
    Settings,
    About,
    Licenses,
    Exit,

    // Settings labels
    SoftwareIPD,
    Brightness,
    FieldOfView,
    ResolutionScale,
    Bitrate,
    EyeTrackedFoveation,
    Microphone,
    ControllerVibration,
    Passthrough,
    Recenter,
    EyeDebug,
    DiagnosticsHUD,

    // Dropdown options
    Off,
    Pipeline,
    System_,   // "System" clashes with Lang::System

    // Buttons
    Connect,
    Connecting,
    Auto,
    Refresh,
    Reset,
    QuitWiVRn,
    Disconnect,
    Launch,
    Launching,
    Close,       // "X"
    Clear,       // "C"
    OK,
    Backspace,   // "<"

    // Server list
    StartServer,
    OnLocalNetwork,
    Discovered,

    // About
    AppName,
    AboutDesc1,
    AboutDesc2,
    AboutDesc3,
    Upstream,
    ThisClient,
    LicensedAGPL,
    SeeLicensesTab,
    ThirdPartyComponents,
    ALVRLicense,
    ControllerModels,
    OwnedByPico,

    // License panel
    AGPLv3,
    LicenseText1,
    LicenseText2,
    LicenseText3,
    FullLicenseText,

    // PIN pad
    EnterPIN,
    PINDash,

    // EQ panel
    AudioEQ,
    Custom1,
    Custom2,

    // Stream panel
    Performance,
    LatencyBreakdown,
    StreamInfo,
    RunningApps,
    NoAppsRunning,
    LaunchApplication,
    Loading,
    NoAppsAvailable,
    Overlay,
    On,
    On_,    // duplicate for "On" state
    Off_,   // "Off" state for mic

    // Latency breakdown labels
    Encode,
    Send,
    Network,
    Decode,
    Render,
    Blit,

    // Diagnostics overlay
    Diagnostics,
    SystemTelemetry,
    CPU,
    GPU,
    Usage,
    Little,
    Big,
    Temp,
    Clock,
    HeatC,
    DDR,
    SoC,
    CtrlL,
    CtrlR,
    LatencyMs,
    VideoRate,
    TimeMs,
    Total,
    Queue,
    FPS,
    Decoded,
    Submit,
    Dropped,
    Gap,
    Enqueue,
    Fencetmo,
    Waiting,

    // Battery warning
    LowBattery,
    PercentRemaining,

    // Language selector
    Language,
    English_,
    Chinese_,
    SystemLang,

    // Format string fragments (used with snprintf, not tr() directly)
    // These stay in English; only the labels get translated.

    Count,
};

// Look up a translated string. Returns English fallback if missing.
const char * tr(Str key);

// Format helpers: trFmt returns a format string suitable for snprintf.
const char * trFmt(Str key);
