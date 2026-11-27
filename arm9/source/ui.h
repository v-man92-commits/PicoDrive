#pragma once
#include <nds/ndstypes.h>

/// Minimal flicker-free text UI on both screens (32x24 characters each).
/// Lines are composed into a buffer and only changed lines are redrawn.

enum UiColor
{
    UI_WHITE,
    UI_GREY,
    UI_GREEN,
    UI_YELLOW,
    UI_RED,
    UI_CYAN
};

enum UiScreen
{
    UI_TOP,
    UI_BOTTOM
};

constexpr int UI_COLS = 32;
constexpr int UI_ROWS = 24;

void ui_init();
void ui_clear(UiScreen screen);
void ui_line(UiScreen screen, int row, UiColor color, const char* fmt, ...)
    __attribute__((format(printf, 4, 5)));
void ui_flush();
// Draws the current lines without printf; safe in an interrupt handler.
void ui_flushDirect();

/// Button bits (libnds layout: A=0 ... L=9, X=10, Y=11)
enum
{
    BTN_A = 1 << 0, BTN_B = 1 << 1, BTN_SELECT = 1 << 2, BTN_START = 1 << 3,
    BTN_RIGHT = 1 << 4, BTN_LEFT = 1 << 5, BTN_UP = 1 << 6, BTN_DOWN = 1 << 7,
    BTN_R = 1 << 8, BTN_L = 1 << 9, BTN_X = 1 << 10, BTN_Y = 1 << 11
};

struct Buttons
{
    u32 held;
    u32 pressed;   // newly pressed this frame
    u32 repeat;    // pressed, or held long enough to auto-repeat
};

/// keyXY is REG_KEYXY as published by the ARM7.
Buttons ui_readButtons(u32 keyXY);

/// Formats a byte count compactly, e.g. "8.0M", "512M", "3.9G".
void ui_formatSize(char* out, u64 bytes);
