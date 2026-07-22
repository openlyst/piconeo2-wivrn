#include "pin_pad.h"
#include "ui_kit.h"
#include <mutex>

static std::mutex gPinMutex;
static std::string gPinBuffer;
static int gHoverKey = -2;

std::atomic<bool> gPinEntryRequested{false};
std::function<void(const std::string &)> gOnPinSubmit;

void requestPinEntryUI() {
    std::lock_guard<std::mutex> lk(gPinMutex);
    gPinBuffer.clear();
    gPinEntryRequested.store(true);
}

void submitPin(const std::string &pin) {
    gPinEntryRequested.store(false);
    if (gOnPinSubmit)
        gOnPinSubmit(pin);
}

// Numpad layout: 4 columns x 4 rows
// Row 0: 1, 2, 3, X (cancel pairing)
// Row 1: 4, 5, 6, < (backspace)
// Row 2: 7, 8, 9, (empty)
// Row 3: C (clear), 0, OK
static const char *kKeyLabels[4][4] = {
    {"1", "2", "3", "X"},
    {"4", "5", "6", "<"},
    {"7", "8", "9", ""},
    {"C", "0", "OK", ""},
};
static const int kKeyIds[4][4] = {
    {1, 2, 3, -3},   // -3 = cancel (X)
    {4, 5, 6, -1},   // -1 = backspace
    {7, 8, 9, -2},   // -2 = empty
    {-5, 0, -4, -2}, // -5 = clear buffer, -4 = submit
};

static constexpr float kKeySize = 0.08f;
static constexpr float kKeyGap = 0.015f;
static constexpr float kPadW = 4 * kKeySize + 3 * kKeyGap;
static constexpr float kPadH = 4 * kKeySize + 3 * kKeyGap;
static constexpr float kTitleH = 0.06f;
static constexpr float kDisplayH = 0.10f;
static constexpr float kTotalH = kTitleH + kDisplayH + kPadH + 0.04f;
static constexpr float kTotalW = kPadW + 0.06f;

static UiRect keyRect(int row, int col) {
    float x0 = -kPadW * 0.5f;
    float y0 = kPadH * 0.5f - kTitleH - kDisplayH;
    float cx = x0 + col * (kKeySize + kKeyGap) + kKeySize * 0.5f;
    float cy = y0 - row * (kKeySize + kKeyGap) - kKeySize * 0.5f;
    return { cx, cy, kKeySize, kKeySize };
}

bool buildPinPad(std::vector<float> &v, float cursorLx, float cursorLy,
                 bool clickEdge, bool cursorOnPanel) {
    if (!gPinEntryRequested.load()) return false;

    // Background panel
    float bgR = kTotalW * 0.5f;
    float bgT = kTotalH * 0.5f;
    appendQuad(v, -bgR, bgT, bgR, -bgT, 0.05f, 0.06f, 0.10f);

    // Title
    uiTextC(v, "Enter PIN", 0, bgT - 0.015f, kUiText * 0.9f, 0.7f, 0.75f, 0.85f);

    // PIN display - show actual digits typed
    std::string display;
    {
        std::lock_guard<std::mutex> lk(gPinMutex);
        display = gPinBuffer;
    }
    const char *dispStr = display.empty() ? "-" : display.c_str();
    float dispY = bgT - kTitleH - kDisplayH * 0.5f;
    // Display background
    appendQuad(v, -kPadW * 0.5f, dispY + kDisplayH * 0.5f,
               kPadW * 0.5f, dispY - kDisplayH * 0.5f,
               0.08f, 0.09f, 0.13f);
    uiTextC(v, dispStr, 0, dispY + kDisplayH * 0.5f - 0.015f,
            kUiText * 1.4f, 1.0f, 1.0f, 1.0f);

    // Numpad keys
    gHoverKey = -2;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            int id = kKeyIds[r][c];
            if (id == -2) continue;
            UiRect kr = keyRect(r, c);
            const char *label = kKeyLabels[r][c];
            bool hot = cursorOnPanel && uiHit(kr, cursorLx, cursorLy);
            if (hot) gHoverKey = id;

            float col[3] = {0.12f, 0.14f, 0.18f};
            if (hot) { col[0] = 0.2f; col[1] = 0.25f; col[2] = 0.35f; }
            // OK = green
            if (id == -4) { col[0] = 0.1f; col[1] = 0.35f; col[2] = 0.15f; if (hot) { col[0] = 0.15f; col[1] = 0.5f; col[2] = 0.2f; } }
            // X (cancel) = red
            if (id == -3) { col[0] = 0.35f; col[1] = 0.1f; col[2] = 0.1f; if (hot) { col[0] = 0.55f; col[1] = 0.15f; col[2] = 0.15f; } }
            // Backspace = orange/brown
            if (id == -1) { col[0] = 0.25f; col[1] = 0.18f; col[2] = 0.08f; if (hot) { col[0] = 0.4f; col[1] = 0.28f; col[2] = 0.12f; } }
            // Clear = yellow/brown
            if (id == -5) { col[0] = 0.2f; col[1] = 0.18f; col[2] = 0.08f; if (hot) { col[0] = 0.35f; col[1] = 0.3f; col[2] = 0.12f; } }

            appendQuad(v, kr.cx - kr.w*0.5f, kr.cy + kr.h*0.5f,
                       kr.cx + kr.w*0.5f, kr.cy - kr.h*0.5f,
                       col[0], col[1], col[2]);
            uiTextC(v, label, kr.cx, kr.cy + baselineOffset(kUiText * 0.9f),
                    kUiText * 0.9f, 1, 1, 1);
        }
    }

    // Handle clicks
    if (clickEdge && cursorOnPanel && gHoverKey != -2) {
        std::string pinToSubmit;
        bool doSubmit = false;
        {
            std::lock_guard<std::mutex> lk(gPinMutex);
            if (gHoverKey >= 0 && gHoverKey <= 9) {
                if (gPinBuffer.size() < 8) gPinBuffer += ('0' + gHoverKey);
            } else if (gHoverKey == -1) {
                // backspace
                if (!gPinBuffer.empty()) gPinBuffer.pop_back();
            } else if (gHoverKey == -5) {
                // clear - just wipe the buffer, keep pad open
                gPinBuffer.clear();
            } else if (gHoverKey == -3) {
                // cancel - submit default and dismiss
                pinToSubmit = "000000";
                gPinBuffer.clear();
                gPinEntryRequested.store(false);
                doSubmit = true;
            } else if (gHoverKey == -4) {
                // submit
                pinToSubmit = gPinBuffer.empty() ? "000000" : gPinBuffer;
                gPinBuffer.clear();
                gPinEntryRequested.store(false);
                doSubmit = true;
            }
        }
        if (doSubmit && gOnPinSubmit) gOnPinSubmit(pinToSubmit);
    }

    return true;
}
