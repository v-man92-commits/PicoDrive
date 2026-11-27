// Host test for the ARM7 half of the bootstub handover (exitToMenu7.cpp),
// played against a copy of libnds' ARM9 resetARM7() (source/common/reset.c),
// which is what a bootstub's arm9reboot uses. Two threads = two CPUs.
#include <stdio.h>
#include <thread>
#include <chrono>
#include <pthread.h>
#include <unistd.h>
#include "exit7_sim.h"
#include "common.h"
#include "exitToMenu7.h"

std::atomic<u32> gArm7Out, gArm9Out, gBootAddress;
std::mutex gFifoMutex;
std::deque<u32> gFifoTo7;
Sim7Sync gSim7Sync;
Sim7FifoCnt gSim7FifoCnt;
Sim7FifoRecv gSim7FifoRecv;

static std::atomic<u32> gJumpedTo;
static std::atomic<bool> gArm9Done;
void exit7_jump(u32 entry) { gJumpedTo = entry; pthread_exit(nullptr); }

static int gPasses, gFails;
#define CHECK(c, ...) do { if (c) gPasses++; else { gFails++; printf("  FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// libnds reset.c, ARM9 side: resetARM7(address)
static void libndsResetArm7(u32 address)
{
    gBootAddress = address;                                   // *(0x02FFFE34) = address
    { std::lock_guard<std::mutex> l(gFifoMutex); gFifoTo7.push_back(0x0c04000c); } // REG_IPC_FIFO_TX
    while ((gArm7Out.load() & 0x0f) != 1) ;                   // while((REG_IPC_SYNC & 0x0f) != 1);
    gArm9Out = 1;                                             // REG_IPC_SYNC = 0x100;
    while ((gArm7Out.load() & 0x0f) != 0) ;                   // while((REG_IPC_SYNC & 0x0f) != 0);
    gArm9Out = 0;                                             // REG_IPC_SYNC = 0;
    gArm9Done = true;
}

static bool runCase(const char* name, u32 arm7Start, u32 arm9Start, bool junk, bool arm9First)
{
    gArm7Out = arm7Start; gArm9Out = arm9Start; gJumpedTo = 0; gArm9Done = false; gBootAddress = 0;
    { std::lock_guard<std::mutex> l(gFifoMutex); gFifoTo7.clear(); if (junk) { gFifoTo7.push_back(0x00150123); gFifoTo7.push_back(0x12345678); } }
    std::thread arm9, arm7;
    if (arm9First) {
        arm9 = std::thread(libndsResetArm7, 0x06000000u);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        arm7 = std::thread([] { exit7_bootstubHandoff(); });
    } else {
        arm7 = std::thread([] { exit7_bootstubHandoff(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        arm9 = std::thread(libndsResetArm7, 0x06000000u);
    }
    for (int i = 0; i < 200 && !(gArm9Done && gJumpedTo); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    bool ok = gArm9Done && gJumpedTo == 0x06000000;
    CHECK(ok, "%s: handover %s (arm9 done %d, arm7 jumped to %#x)", name, ok ? "ok" : "HUNG", (int)gArm9Done.load(), gJumpedTo.load());
    if (!ok) { printf("exit7_test: deadlock\n"); _exit(1); }
    arm9.join(); arm7.join();
    CHECK(gArm7Out == 0 && gArm9Out == 0, "%s: both sync outputs back to 0", name);
    return ok;
}

int main()
{
    runCase("image mounter (ARM7 waits first)", 7, 6, false, false);
    runCase("mass-storage (sync 5/5, old FIFO words)", 5, 5, true, false);
    runCase("ARM9 asks before the ARM7 listens", 7, 6, true, true);
    printf("exit7_test: %d checks passed, %d failed\n", gPasses, gFails);
    return gFails ? 1 : 0;
}
