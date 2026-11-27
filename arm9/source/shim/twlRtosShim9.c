// libtwl RTOS API for the ARM9 (interrupts, events, mutexes), implemented on
// libnds's interrupt dispatcher and cooperative threads.
//
// On the ARM9, libnds must own the IRQ vector: DSWiFi runs its network stack
// in a libnds cothread and uses libnds timers, and cothreads are woken by the
// libnds dispatcher. The image mounter's ARM9 code (and libtwl's IPC FIFO
// system) use libtwl's rtos_* API, so these functions provide it on top of
// libnds. Every rtos_* symbol the ARM9 needs is defined here, so the linker
// never pulls libtwl's own RTOS objects in.
//
// The ARM9 has a single application thread (plus DSWiFi's lwIP cothread), so
// events wait by yielding to other cothreads until an interrupt sets them,
// and mutexes only need to count recursive locks.
#include <nds/ndstypes.h>
#include <nds/interrupts.h>
#include <nds/cothread.h>
#include <libtwl/rtos/rtosIrq.h>
#include <libtwl/rtos/rtosEvent.h>
#include <libtwl/rtos/rtosMutex.h>

static rtos_irq_func_t sFuncs[32];

#define TRAMPOLINE(n) static void tramp##n(void) { if (sFuncs[n]) sFuncs[n](1u << n); }
TRAMPOLINE(0) TRAMPOLINE(1) TRAMPOLINE(2) TRAMPOLINE(3) TRAMPOLINE(4) TRAMPOLINE(5)
TRAMPOLINE(6) TRAMPOLINE(7) TRAMPOLINE(8) TRAMPOLINE(9) TRAMPOLINE(10) TRAMPOLINE(11)
TRAMPOLINE(12) TRAMPOLINE(13) TRAMPOLINE(14) TRAMPOLINE(15) TRAMPOLINE(16) TRAMPOLINE(17)
TRAMPOLINE(18) TRAMPOLINE(19) TRAMPOLINE(20) TRAMPOLINE(21) TRAMPOLINE(22) TRAMPOLINE(23)
TRAMPOLINE(24) TRAMPOLINE(25) TRAMPOLINE(26) TRAMPOLINE(27) TRAMPOLINE(28) TRAMPOLINE(29)
TRAMPOLINE(30) TRAMPOLINE(31)

static const VoidFn sTramps[32] = {
    tramp0, tramp1, tramp2, tramp3, tramp4, tramp5, tramp6, tramp7,
    tramp8, tramp9, tramp10, tramp11, tramp12, tramp13, tramp14, tramp15,
    tramp16, tramp17, tramp18, tramp19, tramp20, tramp21, tramp22, tramp23,
    tramp24, tramp25, tramp26, tramp27, tramp28, tramp29, tramp30, tramp31,
};

__attribute__((target("arm"))) u32 rtos_disableIrqs(void)
{
    u32 cpsr;
    __asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
    __asm__ volatile("msr cpsr_c, %0" ::"r"(cpsr | 0x80) : "memory");
    return cpsr;
}

__attribute__((target("arm"))) u32 rtos_enableIrqs(void)
{
    u32 cpsr;
    __asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
    __asm__ volatile("msr cpsr_c, %0" ::"r"(cpsr & ~0x80u) : "memory");
    return cpsr;
}

__attribute__((target("arm"))) void rtos_restoreIrqs(u32 value)
{
    __asm__ volatile("msr cpsr_c, %0" ::"r"(value) : "memory");
}

void rtos_halt(void)
{
    cothread_yield_irq(~0u);
}

void rtos_initIrq(void)
{
    // libnds initialised its dispatcher before main(); just make sure the
    // master enable is on.
    REG_IME = 1;
}

void rtos_startMainThread(void)
{
}

void rtos_setIrqFunc(u32 mask, rtos_irq_func_t func)
{
    for (int i = 0; i < 32; i++)
    {
        u32 bit = 1u << i;
        if (!(mask & bit))
            continue;
        sFuncs[i] = func;
        irqSet(bit, func ? sTramps[i] : NULL);
    }
}

// ---------------------------------------------------------------------------
// Events

void rtos_createEvent(rtos_event_t* event)
{
    event->flag = false;
    event->queue.head = NULL;
}

void rtos_signalEvent(rtos_event_t* event)
{
    event->flag = true;
}

void rtos_waitEvent(rtos_event_t* event, bool waitNew, bool clearAfter)
{
    u32 irq = rtos_disableIrqs();
    if (!waitNew && event->flag)
    {
        if (clearAfter)
            event->flag = false;
        rtos_restoreIrqs(irq);
        return;
    }
    event->flag = false;
    rtos_restoreIrqs(irq);

    // Events are signalled from interrupt handlers: let other cothreads (the
    // WiFi stack) run until an interrupt arrives, then look again.
    while (!*(volatile bool*)&event->flag)
        cothread_yield_irq(~0u);

    if (clearAfter)
        event->flag = false;
}

// ---------------------------------------------------------------------------
// Mutexes (one application thread: just count)

void rtos_createMutex(rtos_mutex_t* mutex)
{
    mutex->obj.prev = NULL;
    mutex->obj.next = NULL;
    mutex->lockCount = 0;
    mutex->owner = NULL;
    mutex->queue.head = NULL;
}

void rtos_lockMutex(rtos_mutex_t* mutex)
{
    mutex->lockCount++;
}

bool rtos_tryLockMutex(rtos_mutex_t* mutex)
{
    mutex->lockCount++;
    return true;
}

void rtos_unlockMutex(rtos_mutex_t* mutex)
{
    if (mutex->lockCount)
        mutex->lockCount--;
}
