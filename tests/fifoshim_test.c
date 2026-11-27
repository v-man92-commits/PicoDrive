// Host test for common/shim/ndsFifoShim.c: the libnds FIFO API over a fake
// libtwl IPC channel looped back to itself (what one CPU sends, the same
// shim receives, as the other CPU would).
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <nds/fifocommon.h>
#include "ndsFifoShim.h"
#include "ipcChannels.h"
#include <libtwl/ipc/ipcFifoSystem.h>
static int pass, fails;
#define CHECK(c, ...) do { if (c) pass++; else { fails++; printf("  FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- fake libtwl ---------------------------------------------------------
static ipc_fifo_ch_handler_func_t sCh[32];
static void* sChArg[32];
static uint32_t sWire[4096]; static int sWireLen;
static int sIrqDepth, sMaxBurst, sBurst;
static int sOtherChannelEvery; // inject words from another libtwl channel between ours
void ipc_setChannelHandler(u32 ch, void (*h)(u32, u32, void*), void* a) { sCh[ch] = h; sChArg[ch] = a; }
static bool sFifoBusy; // the other CPU hasn't read yet
bool ipc_isSendFifoEmpty(void) { return !sFifoBusy; }
bool ipc_isSendFifoFull(void) { return false; }
void ipc_sendWordDirect(u32 w) { sWire[sWireLen++] = w; sBurst++; if (sBurst > sMaxBurst) sMaxBurst = sBurst; }
u32 rtos_disableIrqs(void) { sIrqDepth++; sBurst = 0; return 0; }
void rtos_restoreIrqs(u32 v) { (void)v; sIrqDepth--; }
static int sOther;
static void otherChannel(u32 c, u32 d, void* a) { (void)c; (void)d; (void)a; sOther++; }
// deliver everything on the wire, like the receiving CPU's IPC IRQ
static void pump(void)
{
    for (int i = 0; i < sWireLen; i++) {
        if (sOtherChannelEvery && i % sOtherChannelEvery == 1) sCh[IPC_CHANNEL_MOUNT](IPC_CHANNEL_MOUNT, 123, NULL);
        uint32_t w = sWire[i];
        uint32_t ch = w & 31;
        if (sCh[ch]) sCh[ch](ch, w >> 5, sChArg[ch]);
    }
    sWireLen = 0;
}

// ---- handlers --------------------------------------------------------------
static uint32_t gVals[64]; static int gNVals; static void* gValArg;
static void onValue(u32 v, void* ud) { gVals[gNVals++] = v; gValArg = ud; }
static uint32_t gAddr; static void onAddr(void* a, void* ud) { (void)ud; gAddr = (uint32_t)(uintptr_t)a; }
static uint8_t gData[32]; static int gDataLen;
static void onData(int n, void* ud) { (void)ud; gDataLen = fifoGetDatamsg(5, sizeof(gData), gData); (void)n; }
static int gNotified;
static void notify(void) { gNotified++; }

int main(void)
{
    sCh[IPC_CHANNEL_MOUNT] = otherChannel;
    // before init: sends are accepted and dropped (nobody listening yet)
    CHECK(fifoSendValue32(1, 5) && sWireLen == 0, "pre-init send dropped");
    // libnds init registers handlers before the app calls ndsFifoShim_init
    CHECK(fifoSetValue32Handler(2, onValue, (void*)0x77), "set handler pre-init");
    ndsFifoShim_init(NULL);
    CHECK(sCh[IPC_CHANNEL_NDSFIFO] != NULL, "channel registered");

    // value32 with a handler: delivered immediately (IRQ dispatch)
    CHECK(fifoSendValue32(2, 0xDEADBEEF), "send");
    CHECK(sMaxBurst == 3 && sIrqDepth == 0, "one atomic burst of 3 words (%d)", sMaxBurst);
    pump();
    CHECK(gNVals == 1 && gVals[0] == 0xDEADBEEF && gValArg == (void*)0x77, "handler got value");

    // no handler: queued, fifoCheck/Get in order, interleaved with other channels
    sOtherChannelEvery = 2;
    for (uint32_t i = 0; i < 10; i++) fifoSendValue32(7, 0x01000000u * i + i);
    pump();
    sOtherChannelEvery = 0;
    CHECK(sOther > 0, "other channel words were interleaved");
    int n = 0; int inorder = 1;
    while (fifoCheckValue32(7)) { uint32_t v = fifoGetValue32(7); if (v != 0x01000000u * n + n) inorder = 0; n++; }
    CHECK(n == 10 && inorder, "queued values %d in order %d", n, inorder);
    CHECK(fifoGetValue32(7) == 0, "empty queue gives 0");

    // queued values flush to a handler set later
    gNVals = 0;
    fifoSendValue32(9, 1); fifoSendValue32(9, 2); pump();
    fifoSetValue32Handler(9, onValue, NULL);
    CHECK(gNVals == 2 && gVals[0] == 1 && gVals[1] == 2 && !fifoCheckValue32(9), "flush on set");

    // addresses
    fifoSendAddress(3, (void*)(uintptr_t)0x02FFFFFCu); pump();
    CHECK(fifoCheckAddress(3) && (uint32_t)(uintptr_t)fifoGetAddress(3) == 0x02FFFFFCu, "queued address");
    fifoSetAddressHandler(3, onAddr, NULL);
    fifoSendAddress(3, (void*)(uintptr_t)0x0380F000u); pump();
    CHECK(gAddr == 0x0380F000u && !fifoCheckAddress(3), "address handler");

    // datamsg: handler reads it with fifoGetDatamsg
    uint8_t msg[20]; for (int i = 0; i < 20; i++) msg[i] = (uint8_t)(i + 100);
    fifoSetDatamsgHandler(5, onData, NULL);
    CHECK(fifoSendDatamsg(5, 20, msg), "send datamsg");
    pump();
    CHECK(gDataLen == 20 && !memcmp(gData, msg, 20) && !fifoCheckDatamsg(5), "datamsg via handler");
    CHECK(!fifoSendDatamsg(5, 25, msg), "too long datamsg refused");
    fifoSendDatamsg(6, 3, msg); pump();
    CHECK(fifoCheckDatamsgLength(6) == 3, "datamsg length");
    uint8_t small[2];
    CHECK(fifoGetDatamsg(6, 2, small) == 3 && small[0] == 100 && small[1] == 101 && !fifoCheckDatamsg(6), "truncated read");

    // bad channels
    CHECK(!fifoSendValue32(16, 1) && !fifoSetValue32Handler(16, onValue, NULL) && !fifoCheckValue32(16), "channel 16 refused");

    // special command goes out raw
    fifoSendSpecialCommand(0x0C04000C);
    CHECK(sWireLen == 1 && sWire[0] == 0x0C04000C, "special raw");
    sWireLen = 0;

    // deferred mode: IRQ only notifies; dispatch happens later from a thread
    ndsFifoShim_init(notify);
    gNVals = 0;
    fifoSendValue32(2, 42); pump();
    CHECK(gNotified == 1 && gNVals == 0, "deferred: notified, not yet delivered");
    ndsFifoShim_dispatchPending();
    CHECK(gNVals == 1 && gVals[0] == 42, "deferred: delivered by dispatchPending");
    // inbox overflow is counted, not corrupting
    for (int i = 0; i < 40; i++) fifoSendValue32(2, (u32)i);
    pump();
    ndsFifoShim_dispatchPending();
    CHECK(ndsFifoShim_dropped() == 8 && gNVals == 1 + 32, "inbox holds 32, drops %lu", (unsigned long)ndsFifoShim_dropped());

    // mutex
    CHECK(fifoMutexTryAcquire(4) && !fifoMutexTryAcquire(4), "mutex");
    fifoMutexRelease(4);
    CHECK(fifoMutexTryAcquire(4), "mutex released");
    CHECK(sIrqDepth == 0, "irq depth balanced");

    // Deadlock guard: inside an interrupt with a busy FIFO, sends are queued
    // (never waited for), keep their order, and go out on the next pump or
    // received message.
    {
        extern bool ndsFifoShim_testIrqsMasked;
        ndsFifoShim_init(NULL);
        fifoSetValue32Handler(9, onValue, NULL);
        gNVals = 0;
        sWireLen = 0;
        sFifoBusy = true;
        ndsFifoShim_testIrqsMasked = true;
        for (u32 i = 0; i < 5; i++)
            CHECK(fifoSendValue32(9, 100 + i), "queued send %u returns", i);
        CHECK(sWireLen == 0 && ndsFifoShim_outboxCount() == 5, "nothing pushed while busy (%d)", sWireLen);
        ndsFifoShim_pump();
        CHECK(sWireLen == 0, "pump waits for empty FIFO");
        sFifoBusy = false;
        // a received message (ours, looped back) also flushes the outbox
        ndsFifoShim_pump();
        CHECK(ndsFifoShim_outboxCount() == 0, "pump flushes");
        pump();
        CHECK(gNVals == 5 && gVals[0] == 100 && gVals[4] == 104, "order kept (%d)", gNVals);
        // new send while outbox non-empty goes behind it
        sFifoBusy = true;
        fifoSendValue32(9, 1);
        sFifoBusy = false;
        ndsFifoShim_testIrqsMasked = false;
        fifoSendValue32(9, 2);
        pump();
        CHECK(gNVals == 7 && gVals[5] == 1 && gVals[6] == 2, "queued first, then new");
        // outbox full inside an interrupt: dropped, not hung
        sFifoBusy = true;
        ndsFifoShim_testIrqsMasked = true;
        for (int i = 0; i < 40; i++)
            fifoSendValue32(9, 7);
        CHECK(ndsFifoShim_outboxCount() == 32 && ndsFifoShim_outboxDropped() == 8, "full outbox drops in IRQ");
        sFifoBusy = false;
        ndsFifoShim_testIrqsMasked = false;
        ndsFifoShim_pump();
        CHECK(sIrqDepth == 0, "irq depth balanced after outbox");
    }

    printf("fifoshim: %d checks passed, %d failed\n", pass, fails);
    return fails ? 1 : 0;
}
