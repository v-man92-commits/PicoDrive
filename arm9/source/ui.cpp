#include "common.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <nds/arm9/console.h>
#include <nds/arm9/video.h>
#include <nds/arm9/background.h>
#include "ui.h"

struct Line
{
    char text[UI_COLS + 1];
    u8 color;
};

static PrintConsole sConsoles[2];
static Line sLines[2][UI_ROWS];
static Line sShown[2][UI_ROWS];

// What the console wrote for each colour: tile number of char 0 and palette
// bits. Learned from normal printf drawing so ui_flushDirect() can draw the
// same way without printf (which takes stdio locks and so can't run in an
// interrupt handler).
static s32 sCharBase[2] = { -1, -1 };
static u16 sPalBits[2][6];
static bool sPalKnown[2][6];

static const char* const sColorCodes[] =
{
    "\x1b[37;1m", // white
    "\x1b[37;0m", // grey
    "\x1b[32;1m", // green
    "\x1b[33;1m", // yellow
    "\x1b[31;1m", // red
    "\x1b[36;1m"  // cyan
};

void ui_init()
{
    videoSetMode(MODE_0_2D);
    videoSetModeSub(MODE_0_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    vramSetBankC(VRAM_C_SUB_BG);
    consoleInit(&sConsoles[UI_TOP], 0, BgType_Text4bpp, BgSize_T_256x256, 31, 0, true, true);
    consoleInit(&sConsoles[UI_BOTTOM], 0, BgType_Text4bpp, BgSize_T_256x256, 31, 0, false, true);
    for (int s = 0; s < 2; s++)
    {
        ui_clear((UiScreen)s);
        for (int r = 0; r < UI_ROWS; r++)
        {
            memset(sShown[s][r].text, 0, sizeof(sShown[s][r].text)); // force first draw
            sShown[s][r].color = 0xFF;
        }
    }
}

void ui_clear(UiScreen screen)
{
    for (int r = 0; r < UI_ROWS; r++)
    {
        memset(sLines[screen][r].text, ' ', UI_COLS);
        sLines[screen][r].text[UI_COLS] = 0;
        sLines[screen][r].color = UI_WHITE;
    }
}

void ui_line(UiScreen screen, int row, UiColor color, const char* fmt, ...)
{
    if (row < 0 || row >= UI_ROWS)
        return;
    char buf[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    Line& line = sLines[screen][row];
    size_t len = strlen(buf);
    if (len > UI_COLS)
        len = UI_COLS;
    memcpy(line.text, buf, len);
    memset(line.text + len, ' ', UI_COLS - len);
    line.text[UI_COLS] = 0;
    line.color = color;
}

void ui_flush()
{
    for (int s = 0; s < 2; s++)
    {
        bool selected = false;
        for (int r = 0; r < UI_ROWS; r++)
        {
            Line& want = sLines[s][r];
            Line& have = sShown[s][r];
            if (want.color == have.color && !memcmp(want.text, have.text, UI_COLS))
                continue;
            if (!selected)
            {
                consoleSelect(&sConsoles[s]);
                selected = true;
            }
            // Writing exactly 32 columns never scrolls: the console only
            // wraps when a further character is printed.
            printf("\x1b[%d;0H%s%s", r, sColorCodes[want.color], want.text);
            have = want;
            // stdout may be buffered: make sure the text is in the map
            // before learning how the console encodes it
            fflush(stdout);
            const PrintConsole& c = sConsoles[s];
            for (int x = 0; x < UI_COLS; x++)
            {
                u8 ch = (u8)want.text[x];
                if (ch > ' ')
                {
                    u16 e = c.fontBgMap[(r + c.windowY) * c.consoleWidth + c.windowX + x];
                    if ((s32)(e & 0x3FF) - ch < 0)
                        break; // not written yet
                    sCharBase[s] = (s32)(e & 0x3FF) - ch;
                    sPalBits[s][want.color] = e & 0xF000;
                    sPalKnown[s][want.color] = true;
                    break;
                }
            }
        }
    }
}

void ui_flushDirect()
{
    for (int s = 0; s < 2; s++)
    {
        const PrintConsole& c = sConsoles[s];
        s32 base = sCharBase[s] >= 0 ? sCharBase[s] : (s32)c.fontCharOffset - (s32)c.font.asciiOffset;
        for (int r = 0; r < UI_ROWS; r++)
        {
            Line& want = sLines[s][r];
            u16 pal = sPalKnown[s][want.color] ? sPalBits[s][want.color]
                    : sPalKnown[s][UI_WHITE] ? sPalBits[s][UI_WHITE] : 0xF000;
            u16* map = &c.fontBgMap[(r + c.windowY) * c.consoleWidth + c.windowX];
            for (int x = 0; x < UI_COLS; x++)
                map[x] = (u16)(((base + (u8)want.text[x]) & 0x3FF) | pal);
            // printf's idea of the screen is now stale: make the next
            // ui_flush() redraw every line.
            sShown[s][r].color = 0xFF;
        }
    }
}

Buttons ui_readButtons(u32 keyXY)
{
    static u32 sPrevHeld = 0;
    static u32 sHoldFrames = 0;

    u32 held = (~*(vu16*)0x04000130) & 0x3FF;      // REG_KEYINPUT
    held |= ((~keyXY) & 3) << 10;                   // X, Y from the ARM7

    Buttons b;
    b.held = held;
    b.pressed = held & ~sPrevHeld;
    b.repeat = b.pressed;

    const u32 repeatMask = BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT | BTN_L | BTN_R;
    if (held & repeatMask)
    {
        if ((held & repeatMask) == (sPrevHeld & repeatMask))
        {
            sHoldFrames++;
            if (sHoldFrames >= 20 && (sHoldFrames % 4) == 0)
                b.repeat |= held & repeatMask;
        }
        else
        {
            sHoldFrames = 0;
        }
    }
    else
    {
        sHoldFrames = 0;
    }

    sPrevHeld = held;
    return b;
}

void ui_formatSize(char* out, u64 bytes)
{
    const u64 KiB = 1024, MiB = KiB * 1024, GiB = MiB * 1024;
    if (bytes >= GiB)
        sprintf(out, "%lu.%luG", (u32)(bytes / GiB), (u32)((bytes % GiB) * 10 / GiB));
    else if (bytes >= 100 * MiB)
        sprintf(out, "%luM", (u32)(bytes / MiB));
    else if (bytes >= MiB)
        sprintf(out, "%lu.%luM", (u32)(bytes / MiB), (u32)((bytes % MiB) * 10 / MiB));
    else if (bytes >= KiB)
        sprintf(out, "%luK", (u32)(bytes / KiB));
    else
        sprintf(out, "%luB", (u32)bytes);
}
