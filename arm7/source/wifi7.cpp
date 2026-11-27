// ARM7 side of WiFi (DSWiFi) inside the libtwl-based ARM7.
//
// DSWiFi's ARM7 code expects the libnds environment: its FIFO messages come
// through ndsFifoShim (over libtwl IPC) and its interrupts through
// ndsIrqShim7. Its message handlers and Wifi_Update() run here, in one libtwl
// thread, instead of in interrupt context: that keeps them from preempting
// other users of the SPI bus (the power-management chip) halfway through a
// transfer. gSpiMutex serialises those users.
#include "common.h"
#include <libtwl/rtos/rtosThread.h>
#include <libtwl/rtos/rtosEvent.h>
#include <libtwl/rtos/rtosMutex.h>
#include <dswifi7.h>
#include <nds/fifocommon.h>
#include "shim/ndsFifoShim.h"
#include "wifi7.h"
extern "C" void ndsIrqShim7_setStatus(volatile u32* irqIn, volatile u32* irq2In);
#include "ImageDisk.h"

rtos_mutex_t gSpiMutex;

static rtos_thread_t sWifiThread;
// Stack in main RAM (see .ewram_bss in the linker script); IWRAM is kept for USB.
// DSWiFi's DSi driver (WMI/ath6k) can use a lot of stack: 8 KB, and the
// never-touched part is reported so an overflow would show.
#define WIFI_STACK_WORDS 2048
__attribute__((section(".ewram_bss"))) static u32 sWifiThreadStack[WIFI_STACK_WORDS];
extern "C" volatile u32* gRtosIrqNowPtr;
extern "C" volatile u32* gRtosThreadNowPtr;
static_assert(offsetof(rtos_thread_t, priority) == 76, "libtwl patch reads the priority at 76");
static rtos_event_t sWifiEvent;

// libnds's ARM7 firmware service: the ARM9's readFirmware() (used to read the
// DSi's saved WiFi connections) sends its request on FIFO_FIRMWARE and waits
// for the reply. Without a handler here it would wait forever.
extern "C" void firmwareMsgHandler(int bytes, void* userdata);

static void notifyFromIrq()
{
    rtos_signalEvent(&sWifiEvent);
}

static u32 sUpdates;

// The status block only exists once the ARM9 has handed it over, which can
// be after wifi7_init(): point the IRQ/thread breadcrumbs at it as soon as
// it does.
static void linkBreadcrumbs(mount_status_t* st)
{
    if (gRtosIrqNowPtr != &st->arm7IrqNow)
    {
        ndsIrqShim7_setStatus(&st->arm7IrqIn, &st->arm7Irq2In);
        gRtosThreadNowPtr = &st->arm7Thread;
        gRtosIrqNowPtr = &st->arm7IrqNow;
    }
}

static void publish(u32 state)
{
    mount_status_t* st = imageDisk_status();
    if (!st)
        return;
    linkBreadcrumbs(st);
    st->wifiFifoRx = ndsFifoShim_received();
    st->wifiFifoDone = ndsFifoShim_dispatched();
    st->wifiFifoLast = ndsFifoShim_last();
    st->wifiUpdates = sUpdates;
    st->wifiThreadState = state;
    u32 untouched = 0;
    while (untouched < WIFI_STACK_WORDS && sWifiThreadStack[untouched] == 0xDEADBEEF)
        untouched++;
    st->wifiStackFree = untouched * 4;
}

static void wifiThreadMain(void*)
{
    while (true)
    {
        publish(1);
        rtos_waitEvent(&sWifiEvent, false, true);
        rtos_lockMutex(&gSpiMutex);
        publish(2);
        ndsFifoShim_dispatchPending();
        publish(3);
        Wifi_Update();
        ndsFifoShim_pump();
        sUpdates++;
        rtos_unlockMutex(&gSpiMutex);
    }
}

void wifi7_init()
{
    rtos_createMutex(&gSpiMutex);
    rtos_createEvent(&sWifiEvent);
    if (mount_status_t* st = imageDisk_status())
    {
        ndsIrqShim7_setStatus(&st->arm7IrqIn, &st->arm7Irq2In);
        gRtosIrqNowPtr = &st->arm7IrqNow;
        gRtosThreadNowPtr = &st->arm7Thread;
    }
    for (u32 i = 0; i < WIFI_STACK_WORDS; i++)
        sWifiThreadStack[i] = 0xDEADBEEF;
    ndsFifoShim_init(notifyFromIrq);
    installWifiFIFO();
    fifoSetDatamsgHandler(FIFO_FIRMWARE, firmwareMsgHandler, nullptr);
    // Below the USB, mount and DLDI threads (2-6), above the main loop (16).
    // Priority 1 (just above idle): USB (2/3), mount (5) and SD (6) threads
    // come first, so WiFi traffic can never delay USB enumeration or transfers.
    rtos_createThread(&sWifiThread, 1, wifiThreadMain, nullptr, sWifiThreadStack, sizeof(sWifiThreadStack));
    rtos_wakeupThread(&sWifiThread);
}

void wifi7_tick()
{
    ndsFifoShim_pump();
    rtos_signalEvent(&sWifiEvent);
}
