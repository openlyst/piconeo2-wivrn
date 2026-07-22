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
static const char *kKeyLabels[4][4] = {
    {"1", "2", "3", "<"},
    {"4", "5", "6", ""},
    {"7", "8", "9", ""},
    {"C", "0", "OK", ""},
};
static const int kKeyIds[4][4] = {
    {1, 2, 3, -1},   // -1 = backspace
    {4, 5, 6, -2},   // -2 = empty
    {7, 8, 9, -2},
    {-3, 0, -4, -2}, // -3 = cancel, -4 = submit
};

static constexpr float kKeySize = 0.08f;
static constexpr float kKeyGap = 0.015f;
static constexpr float kPadW = 4 * kKeySize + 3 * kKeyGap;
static constexpr float kPadH = 4 * kKeySize + 3 * kKeyGap;
static constexpr float kTitleH = 0.10f;
static constexpr float kDisplayH = 0.08f;
static constexpr float kTotalH = kTitleH + kDisplayH + kPadH + 0.06f;
static constexpr float kTotalW = kPadW + 0.08f;

static UiRect keyRect(int row, int col) {
    float x0 = -kPadW * 0.5f;
    float y0 = kPadH * 0.5f;
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
    uiTextC(v, "Enter PIN", 0, bgT - 0.02f, kUiText * 1.2f, 0.8f, 0.85f, 1.0f);

    // PIN display (dots)
    std::string display;
    {
        std::lock_guard<std::mutex> lk(gPinMutex);
        display = gPinBuffer;
    }
    std::string dots;
    for (size_t i = 0; i < display.size(); i++) dots += "* ";
    if (dots.empty()) dots = "_";
    uiTextC(v, dots.c_str(), 0, bgT - kTitleH - 0.02f, kUiText * 1.5f, 1.0f, 1.0f, 1.0f);

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
            if (id == -4) { col[0] = 0.1f; col[1] = 0.35f; col[2] = 0.15f; if (hot) { col[0] = 0.15f; col[1] = 0.5f; col[2] = 0.2f; } }
            if (id == -3) { col[0] = 0.3f; col[1] = 0.1f; col[2] = 0.1f; if (hot) { col[0] = 0.5f; col[1] = 0.15f; col[2] = 0.15f; } }
            if (id == -1) { col[0] = 0.2f; col[1] = 0.15f; col[2] = 0.1f; if (hot) { col[0] = 0.35f; col[1] = 0.25f; col[2] = 0.15f; } }

            appendQuad(v, kr.cx - kr.w*0.5f, kr.cy + kr.h*0.5f,
                       kr.cx + kr.w*0.5f, kr.cy - kr.h*0.5f,
                       col[0], col[1], col[2]);
            uiTextC(v, label, kr.cx, kr.cy + 3.5f * kUiText * 0.9f,
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
                if (!gPinBuffer.empty()) gPinBuffer.pop_back();
            } else if (gHoverKey == -3) {
                pinToSubmit = "000000";
                gPinBuffer.clear();
                gPinEntryRequested.store(false);
                doSubmit = true;
            } else if (gHoverKey == -4) {
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
