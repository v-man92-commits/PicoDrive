// Host test for arm9/source/exitToMenu.cpp: Pico Loader header patching and
// the bootstub signature check.
#include <stdio.h>
#include <string.h>
#include <vector>
#include <string>
#include "common.h"
#include "exitToMenu.h"

static int gPasses, gFails;
#define CHECK(c, ...) do { if (c) gPasses++; else { gFails++; printf("  FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static u32 rd32(const u8* p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static u16 rd16(const u8* p) { return p[0] | p[1] << 8; }
static void wr32(u8* p, u32 v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static std::vector<u8> fakeLoader(u32 entry, u16 api, u32 size = 48 * 1024)
{
    std::vector<u8> b(size, 0xCC);  // "code" everywhere
    wr32(&b[0], entry);
    b[10] = api; b[11] = api >> 8;
    return b;
}

int main()
{
    u32 entry; u16 api;
    // API v3: every field
    {
        auto b = fakeLoader(0x06000400, 3);
        PicoLoaderError e = exit9_patchPicoLoader7(b.data(), b.size(), 0x02012340, "/_picoboot.nds", "fat:/_picoboot.nds", entry, api);
        CHECK(e == PicoLoaderError::None && entry == 0x06000400 && api == 3, "v3 patch");
        CHECK(rd32(&b[0]) == 0x06000400 && rd16(&b[10]) == 3, "read-only fields untouched");
        CHECK(rd32(&b[4]) == 0x02012340, "dldiDriver");
        CHECK(rd16(&b[8]) == 0, "bootDrive = DLDI");
        CHECK(!strcmp((char*)&b[12], "/_picoboot.nds") && b[12 + 255] == 0, "romPath");
        CHECK(b[268] == 0 && b[268 + 255] == 0, "savePath empty");
        CHECK(rd32(&b[524]) == 19, "argumentsLength includes the NUL (%u)", rd32(&b[524]));
        CHECK(!strcmp((char*)&b[528], "fat:/_picoboot.nds"), "argv[0]");
        CHECK(!strcmp((char*)&b[784], "/_picoboot.nds"), "launcherPath (v2)");
        CHECK(rd32(&b[1040]) == 0, "cheats = none (v3)");
        CHECK(b[1044] == 0xCC && b[b.size() - 1] == 0xCC, "nothing after the header changed");
    }
    // API v1: the v2/v3 parts don't exist and must stay untouched
    {
        auto b = fakeLoader(0x06000100, 1);
        CHECK(exit9_patchPicoLoader7(b.data(), b.size(), 0x02000000, "/_picoboot.nds", "fat:/_picoboot.nds", entry, api) == PicoLoaderError::None, "v1 patch");
        CHECK(!strcmp((char*)&b[12], "/_picoboot.nds"), "v1 romPath");
        CHECK(b[784] == 0xCC && b[1040] == 0xCC, "v1: launcherPath/cheats area untouched");
    }
    // API v2: launcherPath yes, cheats untouched
    {
        auto b = fakeLoader(0x06000100, 2);
        CHECK(exit9_patchPicoLoader7(b.data(), b.size(), 0x02000000, "/_picoboot.nds", "fat:/_picoboot.nds", entry, api) == PicoLoaderError::None, "v2 patch");
        CHECK(!strcmp((char*)&b[784], "/_picoboot.nds") && b[1040] == 0xCC, "v2: launcherPath set, cheats untouched");
    }
    // refusals
    {
        auto b = fakeLoader(0x02000000, 3);
        CHECK(exit9_patchPicoLoader7(b.data(), b.size(), 0, "/x", "x", entry, api) == PicoLoaderError::BadEntryPoint, "entry outside ARM7 VRAM");
        auto c = fakeLoader(0x06000000, 0);
        CHECK(exit9_patchPicoLoader7(c.data(), c.size(), 0, "/x", "x", entry, api) == PicoLoaderError::BadApiVersion, "api 0");
        auto d = fakeLoader(0x06000000, 3, 512);
        CHECK(exit9_patchPicoLoader7(d.data(), d.size(), 0, "/x", "x", entry, api) == PicoLoaderError::TooSmall, "too small");
        auto e = fakeLoader(0x06000000, 3);
        std::string longPath(300, 'a');
        CHECK(exit9_patchPicoLoader7(e.data(), e.size(), 0, longPath.c_str(), "x", entry, api) == PicoLoaderError::PathTooLong, "long path");
        CHECK(e[12] == 0xCC, "refused patch changed nothing");
    }
    // bootstub signature: 'bootstub' + arm9reboot inside main RAM
    {
        CHECK(EXIT_BOOTSIG == 0x62757473746F6F62ULL && !memcmp("bootstub", "\x62\x6F\x6F\x74\x73\x74\x75\x62", 8), "signature spells 'bootstub' in memory order");
    }
    printf("exit_test: %d checks passed, %d failed\n", gPasses, gFails);
    return gFails ? 1 : 0;
}
