#include "common.h"
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include "exitToMenu.h"
#ifdef __BLOCKSDS__
#include <nds/system.h>
#endif

// pload_header7_t from Pico Loader's include/picoLoader7.h (API version 3)
namespace
{
    struct PloadParams
    {
        char romPath[256];
        char savePath[256];
        u32 argumentsLength;
        char arguments[256];
    };

    struct PloadHeader7
    {
        u32 entryPoint;         // read-only
        u32 dldiDriver;
        u16 bootDrive;          // 0 = flashcard through DLDI
        u16 apiVersion;         // read-only
        PloadParams loadParams;
        char launcherPath[256]; // API >= 2: rom to return to (bootstub)
        u32 cheats;             // API >= 3: pointer, 0 = none
    };

    static_assert(offsetof(PloadHeader7, dldiDriver) == 4);
    static_assert(offsetof(PloadHeader7, bootDrive) == 8);
    static_assert(offsetof(PloadHeader7, apiVersion) == 10);
    static_assert(offsetof(PloadHeader7, loadParams) == 12);
    static_assert(offsetof(PloadHeader7, launcherPath) == 12 + 256 + 256 + 4 + 256);
    static_assert(offsetof(PloadHeader7, cheats) == 1040);
    static_assert(sizeof(PloadHeader7) == 1044);

    constexpr u16 PLOAD_BOOT_DRIVE_DLDI = 0;
    constexpr u32 ARM7_VRAM_START = 0x06000000;
    constexpr u32 ARM7_VRAM_END = 0x06040000;   // VRAM C + D as mapped for the ARM7
}

bool exit9_isBootstub(const void* p)
{
    u32 a = (u32)(uintptr_t)p;
    if (a < 0x02000000 || a >= 0x03000000 || (a & 3))
        return false;
    const ExitBootstub* b = (const ExitBootstub*)p;
    u32 lo = ((const vu32*)p)[0], hi = ((const vu32*)p)[1];
    if ((((u64)hi << 32) | lo) != EXIT_BOOTSIG)
        return false;
    u32 entry = (u32)(uintptr_t)b->arm9reboot;
    return entry >= 0x02000000 && entry < 0x03000000;
}

#ifndef EXIT_TO_MENU_HOST_TEST
extern "C" char* fake_heap_end;

const ExitBootstub* exit9_findBootstub()
{
    if (exit9_isBootstub(fake_heap_end))
        return (const ExitBootstub*)fake_heap_end;
#ifdef __BLOCKSDS__
    // BlocksDS keeps the bootstub at an uncached mirror (0x0CFF4000 on DSi).
    if (exit9_isBootstub((const void*)__system_bootstub))
        return (const ExitBootstub*)__system_bootstub;
#endif
    if (exit9_isBootstub((const void*)0x02FF4000))
        return (const ExitBootstub*)0x02FF4000;
    return nullptr;
}
#endif

const char* exit9_picoLoaderErrorString(PicoLoaderError e)
{
    switch (e)
    {
        case PicoLoaderError::None:          return "OK";
        case PicoLoaderError::TooSmall:      return "picoLoader7.bin is invalid";
        case PicoLoaderError::BadEntryPoint: return "picoLoader7.bin is invalid";
        case PicoLoaderError::BadApiVersion: return "Unsupported Pico Loader";
        case PicoLoaderError::PathTooLong:   return "Path too long";
    }
    return "?";
}

PicoLoaderError exit9_patchPicoLoader7(u8* image, u32 size, u32 dldiDriver,
    const char* romPath, const char* argv0, u32& entryPoint, u16& apiVersion)
{
    if (size < sizeof(PloadHeader7))
        return PicoLoaderError::TooSmall;
    PloadHeader7 h;
    memcpy(&h, image, sizeof(h));
    if (h.entryPoint < ARM7_VRAM_START || h.entryPoint >= ARM7_VRAM_END)
        return PicoLoaderError::BadEntryPoint;
    if (h.apiVersion < 1 || h.apiVersion > 64)
        return PicoLoaderError::BadApiVersion;
    size_t romLen = strlen(romPath), argLen = strlen(argv0);
    if (romLen >= sizeof(h.loadParams.romPath) || argLen + 1 > sizeof(h.loadParams.arguments))
        return PicoLoaderError::PathTooLong;

    h.dldiDriver = dldiDriver;
    h.bootDrive = PLOAD_BOOT_DRIVE_DLDI;
    memset(&h.loadParams, 0, sizeof(h.loadParams));
    memcpy(h.loadParams.romPath, romPath, romLen);       // no save file: homebrew
    memcpy(h.loadParams.arguments, argv0, argLen);       // argv[0], NUL-terminated
    h.loadParams.argumentsLength = argLen + 1;

    // Later API sections only exist if the loader is new enough; older
    // loaders keep code/data there, so leave those bytes alone.
    size_t patchLen = offsetof(PloadHeader7, launcherPath);
    if (h.apiVersion >= 2)
    {
        memset(h.launcherPath, 0, sizeof(h.launcherPath));
        memcpy(h.launcherPath, romPath, romLen);          // exiting the launcher reloads it
        patchLen = offsetof(PloadHeader7, cheats);
    }
    if (h.apiVersion >= 3)
    {
        h.cheats = 0;
        patchLen = sizeof(PloadHeader7);
    }
    memcpy(image, &h, patchLen);
    entryPoint = h.entryPoint;
    apiVersion = h.apiVersion;
    return PicoLoaderError::None;
}
