// Simulated IPC hardware shared by two threads standing in for the two CPUs.
#pragma once
#include <atomic>
#include <deque>
#include <mutex>
#include <stdint.h>
typedef uint32_t u32;
extern std::atomic<u32> gArm7Out, gArm9Out;   // each CPU's IPCSYNC output nibble
extern std::mutex gFifoMutex;
extern std::deque<u32> gFifoTo7;
extern std::atomic<u32> gBootAddress;         // 0x02FFFE34
#define EXIT7_ARM7_BOOT_ADDRESS (gBootAddress.load())
struct Sim7Sync
{
    operator u32() const { return (gArm7Out.load() << 8) | gArm9Out.load(); }
    Sim7Sync& operator=(u32 v) { gArm7Out = (v >> 8) & 0xF; return *this; }
};
struct Sim7FifoCnt
{
    operator u32() const { std::lock_guard<std::mutex> l(gFifoMutex); return gFifoTo7.empty() ? (1u << 8) : 0u; }
    Sim7FifoCnt& operator|=(u32) { return *this; }
};
struct Sim7FifoRecv
{
    operator u32() const { std::lock_guard<std::mutex> l(gFifoMutex); u32 v = gFifoTo7.front(); gFifoTo7.pop_front(); return v; }
};
extern Sim7Sync gSim7Sync;
extern Sim7FifoCnt gSim7FifoCnt;
extern Sim7FifoRecv gSim7FifoRecv;
