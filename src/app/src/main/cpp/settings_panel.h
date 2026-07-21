#pragma once
// Data-driven lobby SETTINGS window. Window chrome (panel, sidebar tabs, header,
// close, scroll/clip/scrollbar) lives here; each category's content is a list of
// MenuItems laid out generically. Panel-local metres, +x right / +y up.
#include <vector>
#include "ui_kit.h"
#include "menu_model.h"

// ---- session state (not persisted) ----------------------------------------
extern bool gSettingsOpen;   // panel shown
extern int  gSettingsCat;    // active category = index into settingsModel()

// The menu model (core categories + extras). Built once on first use; item
// values are read live, so it never needs rebuilding.
MenuModel &settingsModel();
int  settingsNumCats();                  // settingsModel().size()
void settingsClampCat();                 // keep gSettingsCat in range

// Per-category scroll offset (metres). Sized to the model; index by gSettingsCat.
extern std::vector<float> gSettingsScroll;
float &settingsScroll();                  // scroll for the active category

// ---- panel geometry (panel-local metres) ----------------------------------
constexpr float kSetPanelL = -0.80f, kSetPanelR =  0.62f;
constexpr float kSetPanelTop = 0.50f, kSetPanelBot = -0.50f;
constexpr float kSetHdrY   =  0.40f;
constexpr UiRect kSetClose = { 0.575f, 0.40f, 0.07f, 0.07f };
// sidebar width: wider to fit text
constexpr float kSidebarR = -0.42f;   // sidebar right edge
// content viewport (the scrolling region)
constexpr float kCtX0 = -0.36f, kCtX1 = 0.60f;
constexpr float kCtTop = 0.38f, kCtBot = -0.46f;
constexpr float kSetContentOffX = 0.09f;
constexpr float kSetViewportH = kCtTop - kCtBot;
// reference scrollbar (drawn only, never interactive)
constexpr float kSbX = 0.585f, kSbW = 0.012f;
// sidebar tabs: auto-stacked on the left, one per category.
UiRect settingsTabRect(int i);

// ---- measure / build ------------------------------------------------------
// Measure the active category, clamp scroll, return the builder-local -> panel
// offset and total content height.
void settingsMeasure(float &offX, float &offY, float &contentH);
// Emit the whole panel (bg + sidebar + header + close + scroll-clipped content +
// scrollbar).
void buildSettingsPanel(std::vector<float> &v, float offX, float offY, float contentH,
                        const MenuHover &content, int tabHover, bool closeHover);
