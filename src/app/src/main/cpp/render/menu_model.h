#pragma once
// Data-driven lobby menu model. A category is a list of items; this engine
// auto-stacks them and provides generic build / hit-test / input-apply.
// MK_CUSTOM items own their own sub-layout (used for the EQ and input playground).
// All coords are panel-local metres, centred on x=0, laid out from a top Y downward.
#include <vector>
#include <functional>
#include "ui_kit.h"

enum MenuKind { MK_TOGGLE, MK_STEPPER, MK_FADER, MK_BUTTON, MK_DROPDOWN, MK_CUSTOM };

// Which sub-part of an item the pointer is over. grab = arms a press/drag.
struct MenuHover { int item = -1; int part = 0; bool grab = false; };

struct MenuItem {
    MenuKind kind = MK_TOGGLE;
    const char *label = "";

    // Live value binding. toggle: 0/1. fader: 0..1 fraction. stepper: raw value.
    std::function<float()>     get;
    std::function<void(float)> set;     // store the (already-clamped) value

    // Stepper / fader range.
    float vmin = 0.0f, vmax = 1.0f, vstep = 0.1f;

    // Greyed-out: still drawn but ignores input.
    bool disabled = false;

    // MK_DROPDOWN: choices (get() returns selected index, set() picks one).
    std::vector<const char *> options;
    bool dropOpen = false;
    // Optional value readout (stepper shows it; fader may). Writes into buf.
    std::function<void(char *, int)> valueText;

    // Side-effect hooks (all optional).
    std::function<void()> onChange;     // after any committed value change (save / set a dirty flag)
    std::function<void()> onCommit;     // fader drag released (persist)
    std::function<void()> onClick;      // MK_BUTTON pressed

    // MK_CUSTOM: sole occupant of its category, positions itself in builder-local coords.
    float customH = 0.0f;
    std::function<void(std::vector<float> &, const MenuHover &)> cBuild;
    std::function<void(float cx, float cy, MenuHover &)>          cHit;
    std::function<void(const MenuHover &, bool click, bool grab, float cx, float cy)> cAct;
};

struct MenuCategory {
    const char *name = "";
    std::vector<MenuItem> items;
    bool custom = false;            // true => items[0] is a self-positioning MK_CUSTOM
    bool streamingOnly = false;     // only visible while streaming
    bool hideWhileStreaming = false; // hidden while streaming
};
using MenuModel = std::vector<MenuCategory>;

// ---- auto-layout ----------------------------------------------------------
// Top Y of the content stack (first row's top). Shared with the panel viewport.
constexpr float kMenuTopY = 0.30f;
float menuRowHeight(const MenuItem &it);
float menuRowTop(const MenuCategory &c, int i);   // top Y of row i (builder-local)
float menuContentH(const MenuCategory &c);         // total stack height

// ---- generic passes (all builder-local) -----------------------------------
void     menuBuild(std::vector<float> &v, const MenuCategory &c, const MenuHover &h);
MenuHover menuHit(const MenuCategory &c, float cx, float cy);
// Apply pointer input. Manages hold-to-repeat (steppers) and fader drag/commit.
// click = press edge, grab = held this frame.
void     menuApply(int catId, MenuCategory &c, const MenuHover &h,
                   bool click, bool grab, float cx, float cy);
// Quantized hash of every item's current value (for the render-on-demand sig).
unsigned menuValueSig(const MenuCategory &c);
