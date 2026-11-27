#pragma once
#include <nds/ndstypes.h>

/// ARM9 side of "return to the DSpico menu".
///
/// Two ways back, tried in this order:
///  1. The homebrew bootstub. When Pico Launcher (v1.1+) starts homebrew
///     through Pico Loader (v1.5+) it sets "launcherPath", and Pico Loader then
///     leaves a bootstub in memory that reloads the launcher. libnds' exit()
///     uses the same mechanism: bootstub->arm9reboot() on the ARM9 while the
///     ARM7 answers the libnds reset handshake (exitToMenu7.h).
///  2. Pico Loader directly ("How to use Pico Loader from homebrew" in its
///     README): picoLoader9.bin/picoLoader7.bin from /_pico are loaded into
///     VRAM and asked to boot the launcher, /_picoboot.nds.

/// libnds / devkitPro bootstub layout (nds/system.h: struct __bootstub)
struct ExitBootstub
{
    u64 bootsig;            // 'bootstub'
    void (*arm9reboot)();
    void (*arm7reboot)();
    u32 bootsize;
};

static constexpr u64 EXIT_BOOTSIG = 0x62757473746F6F62ULL; // 'bootstub'

/// The bootstub left by the loader, or null. Checks where libnds looks
/// (fake_heap_end) and the conventional 0x02FF4000.
const ExitBootstub* exit9_findBootstub();

/// Same check on a given address (host-testable).
bool exit9_isBootstub(const void* p);

enum class PicoLoaderError
{
    None,
    TooSmall,       // not a picoLoader7.bin (header doesn't fit)
    BadEntryPoint,  // entry point not in ARM7 VRAM
    BadApiVersion,
    PathTooLong
};

const char* exit9_picoLoaderErrorString(PicoLoaderError e);

/// Fills in the pload_header7_t at the start of a picoLoader7.bin image (in
/// normal RAM: VRAM doesn't take byte writes) to boot `romPath` from the
/// flashcard (DLDI). argv0 becomes the rom's argv[0]. On success returns the
/// ARM7 entry point and API version.
PicoLoaderError exit9_patchPicoLoader7(u8* image, u32 size, u32 dldiDriver,
    const char* romPath, const char* argv0, u32& entryPoint, u16& apiVersion);
