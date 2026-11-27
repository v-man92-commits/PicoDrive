#pragma once
// libnds FIFO API over libtwl IPC (see ndsFifoShim.c).
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Call after ipc_initFifoSystem(). notify == NULL: messages are dispatched in
// the IPC IRQ (like libnds). Otherwise notify() is called from the IRQ and the
// app must call ndsFifoShim_dispatchPending() from a thread.
void ndsFifoShim_init(void (*notify)(void));
void ndsFifoShim_dispatchPending(void);
uint32_t ndsFifoShim_dropped(void);
// Diagnostics: messages received / dispatched, last one (type << 8 | channel).
uint32_t ndsFifoShim_received(void);
uint32_t ndsFifoShim_dispatched(void);
uint32_t ndsFifoShim_last(void);
// Pushes out messages that couldn't be sent from an interrupt. Call often
// (e.g. every VBlank); safe in interrupt handlers.
void ndsFifoShim_pump(void);
uint32_t ndsFifoShim_outboxCount(void);
uint32_t ndsFifoShim_outboxDropped(void);
#ifdef __cplusplus
}
#endif
