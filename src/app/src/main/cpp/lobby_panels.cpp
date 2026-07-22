#include "lobby_panels.h"
#include "app_state.h"   // gVid*/g*MsX10 diag publish
#include "input.h"       // gCtrl/gCtrlMutex (controller battery)
#include "log.h"         // nowNs
#include "wivrn/core/latency_tracker.h"  // g_latency
#include "streaming/streaming_client.h"  // g_stream
#include "cjk_text.h"    // gCjkText
#include <cstdio>
#include <cstring>

// Diagnostics overlay builders. Emit pos.xyz+rgb geometry in panel-local metres.
// ALVR pipeline numbers are throttled to ~3/sec so they don't flicker.
// System telemetry (page 2): CPU/GPU utilisation + temperatures from sysfs/procfs.
// All nodes are world-readable on the Neo 2 (SD845); failures read as "--".
namespace {

// read a single whole-number value from a sysfs/proc file; returns false on miss.
bool readLong(const char *path, long *out) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    long v = 0; int n = fscanf(f, "%ld", &v); fclose(f);
    if (n != 1) return false;
    *out = v; return true;
}

// max thermal_zone temp (milli-C) whose type contains needle. -1 if none.
// Zone->type mapping is fixed after boot, so cache matching zone indices per needle.
float zoneTempC(const char *needle) {
    struct Cache { char needle[16]; int zones[8]; int n; };
    static Cache sCache[6];
    static int   sCacheN = 0;
    Cache *c = nullptr;
    for (int i = 0; i < sCacheN; i++)
        if (strncmp(sCache[i].needle, needle, sizeof(sCache[i].needle) - 1) == 0) { c = &sCache[i]; break; }
    if (!c && sCacheN < (int)(sizeof(sCache) / sizeof(sCache[0]))) {
        c = &sCache[sCacheN++];
        snprintf(c->needle, sizeof(c->needle), "%s", needle);
        c->n = 0;
        for (int z = 0; z < 40 && c->n < 8; z++) {
            char p[128], type[64] = {0};
            snprintf(p, sizeof(p), "/sys/class/thermal/thermal_zone%d/type", z);
            FILE *f = fopen(p, "r");
            if (!f) continue;             // gap in numbering -> keep scanning
            if (!fgets(type, sizeof(type), f)) { fclose(f); continue; }
            fclose(f);
            if (strstr(type, needle)) c->zones[c->n++] = z;
        }
    }
    if (!c) return -1.0f;
    long best = -1;
    for (int i = 0; i < c->n; i++) {
        char p[128];
        snprintf(p, sizeof(p), "/sys/class/thermal/thermal_zone%d/temp", c->zones[i]);
        long t = 0; if (readLong(p, &t) && t > best) best = t;
    }
    return best < 0 ? -1.0f : (float)best / 1000.0f;
}

// average scaling_cur_freq/cpuinfo_max_freq (%) over a cpu index range.
float cpuClockPct(int lo, int hi) {
    static long sMaxFreq[8] = {0};   // cpuinfo_max_freq is fixed per core -> read once
    float sum = 0; int n = 0;
    for (int c = lo; c <= hi && c < 8; c++) {
        char p[128]; long cur = 0;
        if (sMaxFreq[c] == 0) {
            snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c);
            if (!readLong(p, &sMaxFreq[c]) || sMaxFreq[c] <= 0) { sMaxFreq[c] = -1; }
        }
        if (sMaxFreq[c] <= 0) continue;
        snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", c);
        if (!readLong(p, &cur)) continue;
        sum += 100.0f * (float)cur / (float)sMaxFreq[c]; n++;
    }
    return n ? sum / n : -1.0f;
}

// aggregate CPU busy% from /proc/stat deltas between calls (stateful).
float cpuBusyPct() {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1.0f;
    char cpu[8]; unsigned long long u=0,ni=0,s=0,id=0,io=0,irq=0,sirq=0,st=0;
    int n = fscanf(f, "%7s %llu %llu %llu %llu %llu %llu %llu %llu",
                   cpu,&u,&ni,&s,&id,&io,&irq,&sirq,&st);
    fclose(f);
    if (n < 5) return -1.0f;
    unsigned long long idle = id + io;
    unsigned long long total = u+ni+s+id+io+irq+sirq+st;
    static unsigned long long pIdle = 0, pTotal = 0;
    unsigned long long dT = total - pTotal, dI = idle - pIdle;
    pIdle = idle; pTotal = total;
    if (dT == 0) return -1.0f;
    float pct = 100.0f * (float)(dT - dI) / (float)dT;
    return pct < 0 ? 0 : (pct > 100 ? 100 : pct);
}

} // namespace

// Page 2: processor / GPU / heat. Same 3-column layout + bg quad as page 1.
static void buildSysOverlay(std::vector<float> &v) {
    // throttle the actual sysfs sampling to ~3/sec so values are readable.
    static float held[12]; static bool have = false; static uint64_t last = 0;
    uint64_t now = nowNs();
    if (!have || now - last > 333000000ULL) {
        held[0] = cpuBusyPct();          // overall CPU busy %
        held[1] = cpuClockPct(0, 3);     // little-cluster clock %
        held[2] = cpuClockPct(4, 7);     // big-cluster clock %
        long gl = 0;
        held[3] = readLong("/sys/class/kgsl/kgsl-3d0/devfreq/gpu_load", &gl) ? (float)gl : -1.0f; // GPU busy %
        long gc = 0, gm = 0;
        bool okc = readLong("/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq", &gc);
        bool okm = readLong("/sys/class/kgsl/kgsl-3d0/devfreq/max_freq", &gm);
        held[4] = (okc && okm && gm > 0) ? 100.0f * (float)gc / (float)gm : -1.0f; // GPU clock %
        held[5] = okc ? (float)gc / 1e6f : -1.0f;                                  // GPU MHz
        held[6] = zoneTempC("cpu");      // hottest CPU zone
        held[7] = zoneTempC("gpu");      // hottest GPU zone
        held[8] = zoneTempC("ddr");      // memory
        held[9] = zoneTempC("aoss");     // SoC / always-on
        // Controller battery % (ext[40], plumbed via keys[10]); -1 = not connected.
        held[10] = held[11] = -1.0f;
        { std::lock_guard<std::mutex> lk(gCtrlMutex);
          if (gCtrl[0].conn == 1 && gCtrl[0].keyCount > 10) held[10] = (float)gCtrl[0].keys[10];
          if (gCtrl[1].conn == 1 && gCtrl[1].keyCount > 10) held[11] = (float)gCtrl[1].keys[10]; }
        last = now; have = true;
    }

    const float px  = 0.0019f;
    const float dy  = 0.026f;
    const float hdrCol[3] = {kUiFill[0], kUiFill[1], kUiFill[2]};
    const float valCol[3] = {0.95f, 0.96f, 1.00f};
    const float hotCol[3] = {1.00f, 0.55f, 0.35f};   // warm tint for high temps
    char line[96];

    const float colL[3] = { -0.24f, -0.07f, 0.10f };
    const float yTop    = 0.090f;

    appendQuad(v, -0.28f, yTop + 0.030f, 0.28f, yTop - 6.0f*dy - 0.050f,
               kUiBg[0], kUiBg[1], kUiBg[2]);

    auto hdr = [&](int c, const char *s){ uiTextL(v, s, colL[c], yTop, px, hdrCol[0], hdrCol[1], hdrCol[2]); };
    auto row = [&](int c, int r, const char *s){ uiTextL(v, s, colL[c], yTop-(r+1)*dy, px, valCol[0], valCol[1], valCol[2]); };
    // temp row: tints orange past 80C so throttling is obvious at a glance.
    auto trow = [&](int c, int r, const char *lbl, float t){
        char b[96];
        if (t < 0) snprintf(b, sizeof(b), "%s --", lbl);
        else       snprintf(b, sizeof(b), "%s %.0f", lbl, t);
        const float *col = (t >= 80.0f) ? hotCol : valCol;
        uiTextL(v, b, colL[c], yTop-(r+1)*dy, px, col[0], col[1], col[2]);
    };
    auto pctStr = [](char *b, size_t n, const char *lbl, float p){
        if (p < 0) snprintf(b, n, "%s --", lbl); else snprintf(b, n, "%s %.0f", lbl, p);   // no '%' glyph; bare number in the dense grid
    };

    // --- Column 0: CPU ---
    hdr(0, "CPU");
    pctStr(line, sizeof(line), "Usage", held[0]); row(0,0,line);
    pctStr(line, sizeof(line), "Little", held[1]); row(0,1,line);
    pctStr(line, sizeof(line), "Big", held[2]);    row(0,2,line);
    trow(0,3,"Temp", held[6]);

    // --- Column 1: GPU ---
    hdr(1, "GPU");
    pctStr(line, sizeof(line), "Usage", held[3]); row(1,0,line);
    pctStr(line, sizeof(line), "Clock", held[4]); row(1,1,line);
    if (held[5] < 0) snprintf(line, sizeof(line), "MHz  --");
    else             snprintf(line, sizeof(line), "MHz  %.0f", held[5]);
    row(1,2,line);
    trow(1,3,"Temp", held[7]);

    // --- Column 2: heat (C) ---
    hdr(2, "Heat (C)");
    trow(2,0,"CPU", held[6]);
    trow(2,1,"GPU", held[7]);
    trow(2,2,"DDR", held[8]);
    trow(2,3,"SoC", held[9]);

    // --- Row 4: controller battery (L/R), warm tint under 20% ---
    auto batCell = [&](int c, const char *lbl, float p){
        char b[96];
        if (p < 0) snprintf(b, sizeof(b), "%s --", lbl);
        else       snprintf(b, sizeof(b), "%s %.0f", lbl, p);   // no '%' glyph in the font
        const float *col = (p >= 0.0f && p < 20.0f) ? hotCol : valCol;
        uiTextL(v, b, colL[c], yTop - 5.0f*dy, px, col[0], col[1], col[2]);
    };
    batCell(0, "Ctrl L", held[10]);
    batCell(1, "Ctrl R", held[11]);

    uiTextC(v, "System telemetry", 0.0f, yTop - 6.0f*dy - 0.010f, px*0.9f, 0.55f, 0.65f, 0.78f);
}

void buildBatteryWarn(std::vector<float> &v, int pct) {
    // Severity tint: <=5% critical red, otherwise the low-battery amber.
    float bg[3], hd[3];
    if (pct <= 5) { bg[0]=0.26f; bg[1]=0.02f; bg[2]=0.02f; hd[0]=1.00f; hd[1]=0.32f; hd[2]=0.28f; }
    else          { bg[0]=0.20f; bg[1]=0.14f; bg[2]=0.01f; hd[0]=1.00f; hd[1]=0.82f; hd[2]=0.32f; }
    const float px = 0.0024f;
    appendQuad(v, -0.21f, 0.066f, 0.21f, -0.066f, bg[0], bg[1], bg[2]);   // card (drawn first)
    uiTextC(v, "Low battery", 0.0f, 0.030f, px, hd[0], hd[1], hd[2]);
    char line[32]; snprintf(line, sizeof(line), "%d percent remaining", pct);   // no '%' glyph in the font
    uiTextC(v, line, 0.0f, -0.022f, px*0.78f, 0.95f, 0.96f, 1.00f);
}

void buildDiagOverlay(std::vector<float> &v, int page) {
    if (page == 2) { buildSysOverlay(v); return; }
    // Throttle latency stats to ~3/sec (values jitter every frame otherwise).
    static float held[6] = {0,0,0,0,0,0};
    static bool  haveHeld = false;
    static uint64_t lastUpd = 0;
    uint64_t now = nowNs();
    if (now - lastUpd > 333000000ULL) {
        if (g_stream && g_stream->session) {
            auto bd = g_latency.get_avg_breakdown_ms();
            int64_t total_ns = g_latency.get_avg_total_latency_ns();
            held[0] = total_ns / 1e6f;  // total
            held[1] = bd[3];            // decode
            held[2] = 0;                // queue (not separately tracked)
            held[3] = bd[4] + bd[5];   // render + blit
            held[4] = 0;
            held[5] = g_stream->stats_fps > 0 ? 1000.0f / g_stream->stats_fps : 0;
            haveHeld = (total_ns > 0);
        } else {
            haveHeld = false;
        }
        lastUpd = now;
    }
    const float px  = 0.0019f;          // glyph size (metres)
    const float dy  = 0.026f;
    const float hdrCol[3] = {kUiFill[0], kUiFill[1], kUiFill[2]};
    const float valCol[3] = {0.95f, 0.96f, 1.00f};
    char line[96];

    const float colL[3] = { -0.24f, -0.07f, 0.10f };   // three columns centred on x=0
    const float colW    = 0.16f;
    const float yTop    = 0.090f;
    (void) colW;

    // Background plane (drawn first so text lands on top).
    appendQuad(v, -0.28f, yTop + 0.030f, 0.28f, yTop - 5.0f*dy - 0.050f,
               kUiBg[0], kUiBg[1], kUiBg[2]);

    auto colHdr = [&](int c, const char *s){
        uiTextL(v, s, colL[c], yTop, px, hdrCol[0], hdrCol[1], hdrCol[2]);
    };
    auto colRow = [&](int c, int r, const char *s){
        uiTextL(v, s, colL[c], yTop - (r+1)*dy, px, valCol[0], valCol[1], valCol[2]);
    };

    // --- Column 0: pipeline latency (ms) ---
    colHdr(0, "Latency (ms)");
    if (haveHeld) {
        snprintf(line, sizeof(line), "Total   %.1f", held[0]); colRow(0,0,line);
        snprintf(line, sizeof(line), "Decode  %.1f", held[1]); colRow(0,1,line);
        snprintf(line, sizeof(line), "Queue   %.1f", held[2]); colRow(0,2,line);
        snprintf(line, sizeof(line), "Render  %.1f", held[3]); colRow(0,3,line);
    } else {
        colRow(0,0,"Waiting...");
    }

    // --- Column 1: video rate (per second) ---
    colHdr(1, "Video rate");
    float fps = (haveHeld && held[5] > 0.01f) ? (1000.0f / held[5]) : (float) gVidSubmit.load();
    snprintf(line, sizeof(line), "FPS      %.0f", fps);              colRow(1,0,line);
    snprintf(line, sizeof(line), "Decoded  %d",  gVidDecoded.load()); colRow(1,1,line);
    snprintf(line, sizeof(line), "Submit   %d",  gVidSubmit.load());  colRow(1,2,line);
    snprintf(line, sizeof(line), "Dropped  %d",  gVidDropped.load()); colRow(1,3,line);

    // --- Column 2: per-stage frame timing (ms). enqueue = submit-to-SDK-warp
    // cost (incl vsync backpressure), not the warp's own compute. ---
    colHdr(2, "Time (ms)");
    snprintf(line, sizeof(line), "Gap     %.1f", gGapMsX10.load()/10.0f); colRow(2,0,line);
    snprintf(line, sizeof(line), "Encode  %.1f", gEncMsX10.load()/10.0f); colRow(2,1,line);
    snprintf(line, sizeof(line), "Enqueue %.1f", gEnqMsX10.load()/10.0f); colRow(2,2,line);
    // warp-submit fence timeouts/sec (slot not GPU-complete in budget -> tear). 0 = healthy.
    snprintf(line, sizeof(line), "Fencetmo %d",  gFenceTimeouts.load()); colRow(2,3,line);

    uiTextC(v, "Diagnostics", 0.0f, yTop - 5.0f*dy - 0.010f, px*0.9f, 0.55f, 0.65f, 0.78f);
}

// CJK test panel: a small floating card with "Hello World" in English (bitmap font)
// and "你好世界" in Simplified Chinese (stb_truetype atlas). Proof of concept for
// i18n support. The background quad goes into bgV (6-float verts, existing shader);
// the CJK text goes into textV (8-float verts, textured shader).
int buildCjkTestPanel(std::vector<float> &bgV, std::vector<float> &textV) {
    // Background card: 0.5m wide, 0.2m tall, dark blue.
    appendQuad(bgV, -0.25f, 0.10f, 0.25f, -0.10f, 0.05f, 0.08f, 0.14f);

    // English line with the existing 5x7 bitmap font.
    uiTextC(bgV, "Hello World", 0.0f, 0.06f, 0.004f, 1.0f, 1.0f, 1.0f);

    // Chinese line: 你好世界. Atlas baked at 32px, so px=0.0015 gives ~48mm tall
    // glyphs. 4 CJK chars * ~32px advance = ~0.19m wide.
    int textVerts = 0;
    if (gCjkText.ready()) {
        float px = 0.0015f;
        const char *zh = "\xe4\xbd\xa0\xe5\xa5\xbd\xe4\xb8\x96\xe7\x95\x8c";  // 你好世界
        float charW = 32.0f * px;  // approx advance per CJK glyph
        float totalW = 4.0f * charW;
        float startX = -totalW * 0.5f;
        float y = -0.05f;
        textVerts = gCjkText.emitQuads(textV, zh, startX, y, px, 1.0f, 0.9f, 0.5f);
    }
    return textVerts;
}
