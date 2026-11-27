#pragma once
#include <nds/ndstypes.h>

/// ARM7 side of "return to the DSpico menu". Shared by the image mounter and
/// the mass-storage example (identical copies in both).

/// Stops interrupts, DMA, timers and sound. Call it once USB is disconnected
/// and nothing else uses the card bus any more.
void exit7_quiesce();

/// Homebrew bootstub handover (the loader's exit path, as used by libnds
/// exit()): waits for the ARM9's "reset ARM7" request - IPC FIFO word
/// 0x0c04000c followed by the IPCSYNC handshake, sent by the bootstub's
/// arm9reboot - and jumps to the ARM7 entry the bootstub left at 0x02FFFE34.
/// This is exactly what libnds' ARM7 does; this ARM7 uses libtwl instead, so
/// it has to answer itself. Never returns.
[[noreturn]] void exit7_bootstubHandoff();

/// Jumps to an ARM7 entry point (Pico Loader's picoLoader7). Never returns.
[[noreturn]] void exit7_jump(u32 entry);
