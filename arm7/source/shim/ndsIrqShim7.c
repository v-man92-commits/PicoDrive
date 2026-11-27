// libnds ARM7 interrupt API (irqSet, irqEnable, irqSetAUX, ...) implemented on
// top of libtwl's RTOS interrupt dispatcher.
//
// libtwl owns the ARM7 IRQ vector (its scheduler switches threads on IRQ
// exit). DSWiFi registers its handlers with the libnds API, so this file
// replaces libnds's interrupts.c.o: every symbol that object defines is
// defined here, so the linker never pulls libnds's copy. Handlers are stored
// per IRQ bit and called from small libtwl-style trampolines.
#include <stddef.h>
#include <nds/ndstypes.h>
#include <nds/interrupts.h>
#include <libtwl/rtos/rtosIrq.h>

#define REG_DISPSTAT_16  (*(vu16*)0x04000004)
#define REG_IPCSYNC_16   (*(vu16*)0x04000180)

// libnds's dispatcher (interruptDispatcher.s.o) is still linked because other
// libnds objects reference it; it reads these tables but is never installed.
VoidFn irqTable[32];
VoidFn irqTableAUX[32] = { 0 };

static VoidFn sHandlers[32];
static VoidFn sHandlersAux[32];

static int bitIndex(u32 bit)
{
    int i = 0;
    while (bit > 1)
    {
        bit >>= 1;
        i++;
    }
    return i;
}

void ndsIrqShim7_callNested(VoidFn f, u32 bit, vu32* regIE);

// Which DSWiFi/libnds handlers are running right now (for the ARM9's stuck
// screen: if the ARM7 locks up inside one, this says which).
static volatile u32 sDummyIn, sDummyIn2;
static volatile u32* sIrqIn = &sDummyIn;
static volatile u32* sIrq2In = &sDummyIn2;

void ndsIrqShim7_setStatus(volatile u32* irqIn, volatile u32* irq2In)
{
    sIrqIn = irqIn;
    sIrq2In = irq2In;
}

static void callMain(u32 irqMask)
{
    u32 bit = irqMask & -irqMask;
    VoidFn f = sHandlers[bitIndex(bit)];
    if (!f)
        return;
    *sIrqIn |= bit;
    ndsIrqShim7_callNested(f, bit, (vu32*)0x04000210);
    *sIrqIn &= ~bit;
}

static void callAux(u32 irqMask)
{
    u32 bit = irqMask & -irqMask;
    VoidFn f = sHandlersAux[bitIndex(bit)];
    if (!f)
        return;
    *sIrq2In |= bit;
    ndsIrqShim7_callNested(f, bit, (vu32*)0x04000218);
    *sIrq2In &= ~bit;
}

void irqInit(void)
{
    // libtwl's rtos_initIrq() (called by the app) sets up the hardware.
}

void irqInitAUX(void)
{
}

void irqInitHandler(VoidFn handler)
{
    (void)handler; // the IRQ vector belongs to libtwl
}

void irqSet(u32 irq, VoidFn handler)
{
    u32 old = rtos_disableIrqs();
    for (int i = 0; i < 32; i++)
    {
        u32 bit = 1u << i;
        if (!(irq & bit))
            continue;
        sHandlers[i] = handler;
        irqTable[i] = handler;
        rtos_setIrqFunc(bit, handler ? callMain : NULL);
    }
    rtos_restoreIrqs(old);
}

void irqSetAUX(u32 irq, VoidFn handler)
{
    u32 old = rtos_disableIrqs();
    for (int i = 0; i < 32; i++)
    {
        u32 bit = 1u << i;
        if (!(irq & bit))
            continue;
        sHandlersAux[i] = handler;
        irqTableAUX[i] = handler;
        rtos_setIrq2Func(bit, handler ? callAux : NULL);
    }
    rtos_restoreIrqs(old);
}

void irqClear(u32 irq)
{
    irqSet(irq, NULL);
    irqDisable(irq);
}

void irqClearAUX(u32 irq)
{
    irqSetAUX(irq, NULL);
    irqDisableAUX(irq);
}

void irqEnable(u32 irq)
{
    u32 old = rtos_disableIrqs();
    if (irq & IRQ_VBLANK)
        REG_DISPSTAT_16 |= 1 << 3;
    if (irq & IRQ_HBLANK)
        REG_DISPSTAT_16 |= 1 << 4;
    if (irq & IRQ_VCOUNT)
        REG_DISPSTAT_16 |= 1 << 5;
    if (irq & IRQ_IPC_SYNC)
        REG_IPCSYNC_16 |= 1 << 14;
    rtos_enableIrqMask(irq);
    rtos_restoreIrqs(old);
}

void irqDisable(u32 irq)
{
    u32 old = rtos_disableIrqs();
    rtos_disableIrqMask(irq);
    rtos_restoreIrqs(old);
}

void irqEnableAUX(u32 irq)
{
    u32 old = rtos_disableIrqs();
    rtos_enableIrq2Mask(irq);
    rtos_restoreIrqs(old);
}

void irqDisableAUX(u32 irq)
{
    u32 old = rtos_disableIrqs();
    rtos_disableIrq2Mask(irq);
    rtos_restoreIrqs(old);
}

VoidFn setPowerButtonCB(VoidFn cb)
{
    (void)cb;
    return NULL;
}

void i2cIRQHandler(void)
{
}
