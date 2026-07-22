#include "i18n.h"
#include "log.h"
#include <cstring>

static Lang gLangSetting = Lang::System;
static Lang gActiveLang  = Lang::English;
static bool gLangDetected = false;

void setLanguage(Lang l) {
    gLangSetting = l;
    if (l == Lang::System) {
        if (!gLangDetected) gActiveLang = Lang::English;
    } else {
        gActiveLang = l;
    }
}

Lang getLanguageSetting() { return gLangSetting; }
Lang getActiveLanguage()  { return gActiveLang; }

void detectSystemLang(const char *locale) {
    gLangDetected = true;
    if (!locale) { gActiveLang = Lang::English; return; }
    // Android locale strings: "zh-CN", "zh_Hans_CN", "zh", etc.
    if (locale[0] == 'z' && locale[1] == 'h') gActiveLang = Lang::Chinese;
    else                                       gActiveLang = Lang::English;
    if (gLangSetting == Lang::System) {
        // already set gActiveLang above
    }
    LOGI("i18n: system locale=%s -> active=%d", locale ? locale : "(null)", (int)gActiveLang);
}

// ---- translation tables --------------------------------------------------

static const char *kEn[] = {
    // Tabs
    "Servers", "Settings", "About", "Licenses", "Exit",
    // Settings labels
    "Software IPD", "Brightness", "Field of view", "Resolution scale",
    "Bitrate", "Eye-tracked foveation", "Microphone", "Controller vibration",
    "Passthrough", "Recenter", "Eye debug", "Diagnostics HUD",
    // Dropdown options
    "Off", "Pipeline", "System",
    // Buttons
    "Connect", "...", "Auto", "Refresh", "Reset", "Quit WiVRn", "Disconnect",
    "Launch", "Launching...", "X", "C", "OK", "<",
    // Server list
    "Start a WiVRn server", "on your local network", "Discovered",
    // About
    "WiVRn for Pico Neo 2",
    "Stream PC VR games to your",
    "Pico Neo 2 over Wi-Fi or USB",
    "with low latency",
    "Upstream", "This client",
    "Licensed under AGPL v3",
    "See Licenses tab for details",
    "Third-party components",
    "ALVR client library - MIT License",
    "3D controller models - Pico Interactive",
    "Owned by Pico Interactive",
    // License panel
    "AGPL v3",
    "This project is licensed under",
    "the GNU Affero General",
    "Public License v3",
    "Full license text",
    // PIN pad
    "Enter PIN", "-",
    // EQ panel
    "Audio EQ", "Custom 1", "Custom 2",
    // Stream panel
    "Performance", "Latency breakdown", "Stream info",
    "Running apps", "No apps running",
    "Launch application", "Loading...", "No apps available",
    "overlay", "On", "On", "Off",
    // Latency breakdown labels
    "Encode", "Send", "Network", "Decode", "Render", "Blit",
    // Diagnostics overlay
    "Diagnostics", "System telemetry",
    "CPU", "GPU", "Usage", "Little", "Big", "Temp", "Clock",
    "Heat (C)", "DDR", "SoC", "Ctrl L", "Ctrl R",
    "Latency (ms)", "Video rate", "Time (ms)",
    "Total", "Queue", "FPS", "Decoded", "Submit", "Dropped",
    "Gap", "Enqueue", "Fencetmo", "Waiting...",
    // Battery warning
    "Low battery", "%d percent remaining",
    // Language selector
    "Language", "English", "Chinese", "System",
};

static const char *kZh[] = {
    // Tabs
    "服务器", "设置", "关于", "许可证", "退出",
    // Settings labels
    "软件瞳距", "亮度", "视场角", "分辨率缩放",
    "码率", "注视点渲染", "麦克风", "手柄震动",
    "透视", "重新居中", "眼睛调试", "诊断HUD",
    // Dropdown options
    "关闭", "管线", "系统",
    // Buttons
    "连接", "...", "自动", "刷新", "重置", "退出WiVRn", "断开",
    "启动", "启动中...", "X", "C", "确定", "<",
    // Server list
    "请启动一个WiVRn服务器", "在你的本地网络中", "已发现",
    // About
    "Pico Neo 2 版 WiVRn",
    "将PC VR游戏串流到你的",
    "Pico Neo 2 通过Wi-Fi或USB",
    "低延迟",
    "上游", "本客户端",
    "基于AGPL v3许可",
    "详见许可证标签页",
    "第三方组件",
    "ALVR客户端库 - MIT许可证",
    "3D手柄模型 - Pico Interactive",
    "Pico Interactive所有",
    // License panel
    "AGPL v3",
    "本项目基于",
    "GNU Affero通用",
    "公共许可证v3",
    "完整许可证文本",
    // PIN pad
    "输入PIN", "-",
    // EQ panel
    "音频均衡器", "自定义1", "自定义2",
    // Stream panel
    "性能", "延迟分解", "串流信息",
    "运行中的应用", "没有运行中的应用",
    "启动应用", "加载中...", "没有可用应用",
    "覆盖层", "开", "开", "关",
    // Latency breakdown labels
    "编码", "发送", "网络", "解码", "渲染", "合成",
    // Diagnostics overlay
    "诊断", "系统遥测",
    "CPU", "GPU", "使用率", "小核", "大核", "温度", "频率",
    "热量(C)", "DDR", "SoC", "左手柄", "右手柄",
    "延迟(ms)", "视频帧率", "时间(ms)",
    "总计", "队列", "FPS", "已解码", "提交", "丢帧",
    "间隔", "入队", "围栏超时", "等待中...",
    // Battery warning
    "电量低", "剩余 %d%%",
    // Language selector
    "语言", "English", "中文", "系统",
};

static_assert(sizeof(kEn)/sizeof(kEn[0]) == (int)Str::Count,
              "EN translation table size mismatch");
static_assert(sizeof(kZh)/sizeof(kZh[0]) == (int)Str::Count,
              "ZH translation table size mismatch");

const char * tr(Str key) {
    int idx = (int)key;
    if (idx < 0 || idx >= (int)Str::Count) return "???";
    if (gActiveLang == Lang::Chinese) return kZh[idx];
    return kEn[idx];
}

const char * trFmt(Str key) {
    return tr(key);
}
