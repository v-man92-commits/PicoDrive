#include "common.h"
#include <string.h>
#include <nds/disc_io.h>
#include <libtwl/rtos/rtosIrq.h>
#include <libtwl/rtos/rtosThread.h>
#include <libtwl/rtos/rtosEvent.h>
#include <libtwl/sound/soundChannel.h>
#include <libtwl/timer/timer.h>
#include <libtwl/sound/sound.h>
#include <libtwl/ipc/ipcSync.h>
#include <libtwl/ipc/ipcFifoSystem.h>
#include <libtwl/sys/sysPower.h>
#include <libtwl/sio/sioRtc.h>
#include <libtwl/sio/sio.h>
#include <libtwl/gfx/gfxStatus.h>
#include <libtwl/mem/memSwap.h>
#include <libtwl/i2c/i2cMcu.h>
#include <libtwl/spi/spiPmic.h>
#include "ipcServices/DldiIpcService.h"
#include "ipcServices/MountIpcService.h"
#include "ImageDisk.h"
#include "ExitMode.h"
#include "Arm7State.h"
#include "wifi7.h"

static DldiIpcService sDldiIpcService;
static MountIpcService sMountIpcService;

rtos_mutex_t gCardMutex;

static rtos_event_t sVBlankEvent;
static ExitMode sExitMode;
static Arm7State sState;
static volatile u8 sMcuIrqFlag = false;


static void vblankIrq(u32 irqMask)
{
    rtos_signalEvent(&sVBlankEvent);
    wifi7_tick();
}

static void mcuIrq(u32 irq2Mask)
{
    sMcuIrqFlag = true;
}

static void checkMcuIrq(void)
{
    // mcu only exists in DSi mode
    if (isDSiMode())
    {
        // check and ack the flag atomically
        if (mem_swapByte(false, &sMcuIrqFlag))
        {
            // check the irq mask (I2C: shared with DSWiFi, see wifi7.h)
            rtos_lockMutex(&gSpiMutex);
            u32 irqMask = mcu_getIrqMask();
            rtos_unlockMutex(&gSpiMutex);
            if (irqMask & MCU_IRQ_RESET)
            {
                // power button was released
                sExitMode = ExitMode::Reset;
                sState = Arm7State::ExitRequested;
            }
            else if (irqMask & MCU_IRQ_POWER_OFF)
            {
                // power button was held long to trigger a power off
                sExitMode = ExitMode::PowerOff;
                sState = Arm7State::ExitRequested;
            }
        }
    }
}

static void initializeVBlankIrq()
{
    rtos_createEvent(&sVBlankEvent);
    rtos_setIrqFunc(RTOS_IRQ_VBLANK, vblankIrq);
    rtos_enableIrqMask(RTOS_IRQ_VBLANK);
    gfx_setVBlankIrqEnabled(true);
}

static void initializeArm7()
{
    rtos_initIrq();
    rtos_startMainThread();
    ipc_initFifoSystem();
    wifi7_init();

    rtos_createMutex(&gCardMutex);

    // clear sound registers
    dmaFillWords(0, (void*)0x04000400, 0x100);

    pmic_setAmplifierEnable(true);
    sys_setSoundPower(true);

    readUserSettings();
    pmic_setPowerLedBlink(PMIC_CONTROL_POWER_LED_BLINK_NONE);

    sio_setGpioSiIrq(false);
    sio_setGpioMode(RCNT0_L_MODE_GPIO);

    rtc_init();

    sDldiIpcService.Start();
    imageDisk_init();
    sMountIpcService.Start();

    snd_setMasterVolume(127);
    snd_setMasterEnable(true);

    initializeVBlankIrq();

    if (isDSiMode())
    {
        rtos_setIrq2Func(RTOS_IRQ2_MCU, mcuIrq);
        rtos_enableIrq2Mask(RTOS_IRQ2_MCU);
    }

    ipc_setArm7SyncBits(7);

    while (ipc_getArm9SyncBits() != 6)
    {
        rtos_waitEvent(&sVBlankEvent, true, true);
    }

    // USB is started on the first mount (see ImageDisk.cpp)
}

// Closing the lid turns both backlights off: a long USB session (e.g. an OS
// installing from the virtual DVD drive) can then run with the DS closed
// without draining the battery on the screens. USB keeps working.
static bool sLidClosed;

static void updateLid()
{
    bool closed = ((*(vu16*)0x04000136) & (1 << 7)) != 0; // REG_KEYXY hinge bit
    if (closed == sLidClosed)
        return;
    sLidClosed = closed;
    rtos_lockMutex(&gSpiMutex);
    pmic_setTopBacklightEnable(!closed);
    pmic_setBottomBacklightEnable(!closed);
    rtos_unlockMutex(&gSpiMutex);
}

static void updateArm7IdleState()
{
    imageDisk_updateFrame();
    updateLid();
    checkMcuIrq();

    if (sState == Arm7State::ExitRequested)
    {
        snd_setMasterVolume(0); // mute sound
    }
}

static bool performExit(ExitMode exitMode)
{
    switch (exitMode)
    {
        case ExitMode::Reset:
        {
            rtos_lockMutex(&gSpiMutex);
            mcu_setWarmBootFlag(true);
            mcu_hardReset();
            break;
        }
        case ExitMode::PowerOff:
        {
            rtos_lockMutex(&gSpiMutex);
            pmic_shutdown();
            break;
        }
    }

    while (true); // wait infinitely for exit
}

static void updateArm7ExitRequestedState()
{
    performExit(sExitMode);
}

static void updateArm7()
{
    switch (sState)
    {
        case Arm7State::Idle:
        {
            updateArm7IdleState();
            break;
        }
        case Arm7State::ExitRequested:
        {
            updateArm7ExitRequestedState();
            break;
        }
    }
}

extern "C" u8 __ewram_bss_start[], __ewram_bss_end[];

int main()
{
    // .ewram_bss (main RAM buffers) isn't cleared by crt0
    memset(__ewram_bss_start, 0, __ewram_bss_end - __ewram_bss_start);
    sState = Arm7State::Idle;
    initializeArm7();

    while (true)
    {
        mount_status_t* st = imageDisk_status();
        if (st)
            st->arm7Step = 1;
        rtos_waitEvent(&sVBlankEvent, true, true);
        if (st)
        {
            st->arm7Step = 2;
            st->arm7IE = *(vu32*)0x04000210;
            st->arm7IE2 = *(vu32*)0x04000218;
            st->arm7IME = *(vu32*)0x04000208;
        }
        updateArm7();
    }

    return 0;
}
