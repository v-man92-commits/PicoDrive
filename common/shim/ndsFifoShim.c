// libnds FIFO API (fifoSendValue32, fifoSetValue32Handler, ...) implemented on
// top of libtwl's IPC FIFO system, for both CPUs.
//
// The image mounter talks between the CPUs with libtwl; DSWiFi (and libnds
// itself) use the libnds FIFO API. Both can't own the hardware FIFO, so this
// file replaces libnds's fifosystem.c.o completely (every symbol it defines is
// defined here, so the linker never pulls libnds's version) and sends libnds
// messages through libtwl channel IPC_CHANNEL_NDSFIFO (wire format in
// ndsFifoProto.h).
//
// Receiving: complete messages are put in an inbox from the IPC IRQ. By
// default they are dispatched right there (like libnds). On the ARM7 the app
// can choose deferred dispatch so DSWiFi's handlers run in a thread instead.
#include <nds/ndstypes.h>
#include <nds/fifocommon.h>
#include <string.h>
#include <libtwl/rtos/rtosIrq.h>
#include <libtwl/ipc/ipcFifo.h>
#include <libtwl/ipc/ipcFifoSystem.h>
#include "ipcChannels.h"
#include "ndsFifoProto.h"
#include "ndsFifoShim.h"

#define QUEUE_LEN   16
#define INBOX_LEN   32

// Bulky queues live in main RAM on the ARM7 (IWRAM is needed by the USB
// path); they are cleared in ndsFifoShim_init().
#ifdef LIBTWL_ARM7
#define SHIM_BUFFER __attribute__((section(".ewram_bss")))
#else
#define SHIM_BUFFER
#endif

typedef struct
{
    uint8_t type;
    uint8_t channel;
    uint8_t length;
    uint8_t data[NDSFIFO_MAX_DATAMSG];
} shim_msg_t;

typedef struct
{
    uint32_t items[QUEUE_LEN];
    uint8_t head, count;
} word_queue_t;

typedef struct
{
    uint8_t length[4];
    uint8_t data[4][NDSFIFO_MAX_DATAMSG];
    uint8_t head, count;
} data_queue_t;

static FifoValue32HandlerFunc sValueHandler[NDSFIFO_CHANNELS];
static void* sValueArg[NDSFIFO_CHANNELS];
static FifoAddressHandlerFunc sAddressHandler[NDSFIFO_CHANNELS];
static void* sAddressArg[NDSFIFO_CHANNELS];
static FifoDatamsgHandlerFunc sDataHandler[NDSFIFO_CHANNELS];
static void* sDataArg[NDSFIFO_CHANNELS];

SHIM_BUFFER static word_queue_t sValues[NDSFIFO_CHANNELS];
SHIM_BUFFER static word_queue_t sAddresses[NDSFIFO_CHANNELS];
SHIM_BUFFER static data_queue_t sDatamsgs[NDSFIFO_CHANNELS];

SHIM_BUFFER static shim_msg_t sInbox[INBOX_LEN];
static volatile uint8_t sInboxHead, sInboxCount;

static ndsfifo_decoder_t sDecoder;
static volatile bool sReady;
static void (*sNotify)(void);
static volatile uint32_t sDropped;
static volatile uint32_t sReceived, sDispatched, sLast;

static volatile uint8_t sMutex[NDSFIFO_CHANNELS];

bool fifoMutexTryAcquire(u32 channel);

#define OUTBOX_LEN 32

typedef struct
{
    uint8_t count;
    uint32_t words[NDSFIFO_MAX_WORDS];
} out_msg_t;

SHIM_BUFFER static out_msg_t sOutbox[OUTBOX_LEN];
static volatile uint8_t sOutHead, sOutCount;
static volatile uint32_t sOutDropped;


// ---------------------------------------------------------------------------

static bool wqPush(word_queue_t* q, uint32_t v)
{
    if (q->count >= QUEUE_LEN)
        return false;
    q->items[(q->head + q->count) % QUEUE_LEN] = v;
    q->count++;
    return true;
}

static bool wqPop(word_queue_t* q, uint32_t* v)
{
    if (q->count == 0)
        return false;
    *v = q->items[q->head];
    q->head = (q->head + 1) % QUEUE_LEN;
    q->count--;
    return true;
}

// Takes one message from the inbox (IRQs must be disabled by the caller).
static bool inboxPop(shim_msg_t* out)
{
    if (sInboxCount == 0)
        return false;
    *out = sInbox[sInboxHead];
    sInboxHead = (sInboxHead + 1) % INBOX_LEN;
    sInboxCount--;
    return true;
}

static void deliver(const shim_msg_t* m)
{
    uint32_t ch = m->channel;
    sDispatched++;
    if (m->type == NDSFIFO_TYPE_VALUE32)
    {
        uint32_t v = ndsfifo_u32(m->data);
        if (sValueHandler[ch])
            sValueHandler[ch](v, sValueArg[ch]);
        else
        {
            u32 irq = rtos_disableIrqs();
            if (!wqPush(&sValues[ch], v))
                sDropped++;
            rtos_restoreIrqs(irq);
        }
    }
    else if (m->type == NDSFIFO_TYPE_ADDRESS)
    {
        uint32_t v = ndsfifo_u32(m->data);
        if (sAddressHandler[ch])
            sAddressHandler[ch]((void*)v, sAddressArg[ch]);
        else
        {
            u32 irq = rtos_disableIrqs();
            if (!wqPush(&sAddresses[ch], v))
                sDropped++;
            rtos_restoreIrqs(irq);
        }
    }
    else
    {
        u32 irq = rtos_disableIrqs();
        data_queue_t* q = &sDatamsgs[ch];
        bool stored = false;
        if (q->count < 4)
        {
            int slot = (q->head + q->count) % 4;
            q->length[slot] = m->length;
            memcpy(q->data[slot], m->data, m->length);
            q->count++;
            stored = true;
        }
        else
            sDropped++;
        rtos_restoreIrqs(irq);
        // libnds calls the handler, which reads the message with fifoGetDatamsg()
        if (stored && sDataHandler[ch])
            sDataHandler[ch](m->length, sDataArg[ch]);
    }
}

void ndsFifoShim_dispatchPending(void)
{
    while (1)
    {
        shim_msg_t m;
        u32 irq = rtos_disableIrqs();
        bool got = inboxPop(&m);
        rtos_restoreIrqs(irq);
        if (!got)
            break;
        deliver(&m);
    }
}

static void flushOutboxLocked(void);
static void ipcHandler(u32 channel, u32 data, void* arg)
{
    (void)channel;
    (void)arg;
    flushOutboxLocked(); // IRQ context: IRQs already disabled
    if (!ndsfifo_decode(&sDecoder, data))
        return;
    if (sInboxCount >= INBOX_LEN)
    {
        sDropped++;
        return;
    }
    sReceived++;
    sLast = ((uint32_t)sDecoder.type << 8) | sDecoder.channel;
    shim_msg_t* m = &sInbox[(sInboxHead + sInboxCount) % INBOX_LEN];
    m->type = sDecoder.type;
    m->channel = sDecoder.channel;
    m->length = (uint8_t)sDecoder.length;
    memcpy(m->data, sDecoder.data, sDecoder.length);
    sInboxCount++;
    if (sNotify)
        sNotify();
    else
        ndsFifoShim_dispatchPending();
}

void ndsFifoShim_init(void (*notify)(void))
{
    memset(sValues, 0, sizeof(sValues));
    memset(sAddresses, 0, sizeof(sAddresses));
    memset(sDatamsgs, 0, sizeof(sDatamsgs));
    memset(sInbox, 0, sizeof(sInbox));
    sInboxHead = sInboxCount = 0;
    memset(sOutbox, 0, sizeof(sOutbox));
    sOutHead = sOutCount = 0;
    ndsfifo_decoderReset(&sDecoder);
    sNotify = notify;
    ipc_setChannelHandler(IPC_CHANNEL_NDSFIFO, ipcHandler, NULL);
    sReady = true;
}

uint32_t ndsFifoShim_dropped(void)
{
    return sDropped;
}

uint32_t ndsFifoShim_received(void)
{
    return sReceived;
}

uint32_t ndsFifoShim_dispatched(void)
{
    return sDispatched;
}

uint32_t ndsFifoShim_last(void)
{
    return sLast;
}

// Sending.
//
// A message is pushed whole (so it can't interleave with another libnds
// message from this CPU) once the hardware send FIFO is empty; at most
// NDSFIFO_MAX_WORDS (< 16) words, so the push never blocks.
//
// Never busy-wait for the FIFO from an interrupt handler: DSWiFi sends from
// interrupts on both CPUs, and if both CPUs sit in an interrupt waiting for
// the other to empty its FIFO, neither ever reads (both hang). Messages that
// can't go out at once wait in an outbox, which is pushed out on the next
// send, on every message received, and from ndsFifoShim_pump() (VBlank).

#if defined(__arm__) && !defined(NDSFIFO_HOST_TEST)
// True when IRQs are masked in the CPU (inside an interrupt handler, or code
// that disabled them), i.e. when waiting could never end.
__attribute__((target("arm"), noinline)) static bool irqsMasked(void)
{
    uint32_t cpsr;
    __asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
    return (cpsr & 0x80) != 0;
}
#else
bool ndsFifoShim_testIrqsMasked;
static bool irqsMasked(void)
{
    return ndsFifoShim_testIrqsMasked;
}
#endif

// Pushes queued messages while the FIFO is empty (IRQs disabled by caller).
static void flushOutboxLocked(void)
{
    while (sOutCount && ipc_isSendFifoEmpty())
    {
        const out_msg_t* m = &sOutbox[sOutHead];
        for (int i = 0; i < m->count; i++)
            ipc_sendWordDirect(IPC_FIFO_MSG(IPC_CHANNEL_NDSFIFO, m->words[i]));
        sOutHead = (sOutHead + 1) % OUTBOX_LEN;
        sOutCount--;
    }
}

void ndsFifoShim_pump(void)
{
    if (!sReady)
        return;
    u32 irq = rtos_disableIrqs();
    flushOutboxLocked();
    rtos_restoreIrqs(irq);
}

uint32_t ndsFifoShim_outboxDropped(void)
{
    return sOutDropped;
}

uint32_t ndsFifoShim_outboxCount(void)
{
    return sOutCount;
}

static bool sendMessage(int type, u32 channel, const uint8_t* payload, u32 length)
{
    uint32_t words[NDSFIFO_MAX_WORDS];
    int n = ndsfifo_encode(type, channel, payload, length, words);
    if (n == 0)
        return false;
    if (!sReady)
        return true; // before the IPC system is up: nobody to talk to yet
    bool canWait = !irqsMasked();
    while (1)
    {
        u32 irq = rtos_disableIrqs();
        flushOutboxLocked();
        if (sOutCount == 0 && ipc_isSendFifoEmpty())
        {
            for (int i = 0; i < n; i++)
                ipc_sendWordDirect(IPC_FIFO_MSG(IPC_CHANNEL_NDSFIFO, words[i]));
            rtos_restoreIrqs(irq);
            return true;
        }
        if (sOutCount < OUTBOX_LEN)
        {
            out_msg_t* m = &sOutbox[(sOutHead + sOutCount) % OUTBOX_LEN];
            m->count = (uint8_t)n;
            memcpy(m->words, words, n * sizeof(uint32_t));
            sOutCount++;
            rtos_restoreIrqs(irq);
            return true;
        }
        rtos_restoreIrqs(irq);
        if (!canWait)
        {
            sOutDropped++; // outbox full inside an interrupt: can't wait
            return false;
        }
        // outbox full: wait (IRQs enabled, so the other CPU's messages are
        // still read) for the FIFO to drain
    }
}

// ---------------------------------------------------------------------------
// libnds API

bool fifoInit(void)
{
    return true; // ndsFifoShim_init() is called by the app after ipc_initFifoSystem()
}

bool fifoSendValue32(u32 channel, u32 value32)
{
    uint8_t b[4] = { (uint8_t)value32, (uint8_t)(value32 >> 8), (uint8_t)(value32 >> 16), (uint8_t)(value32 >> 24) };
    return sendMessage(NDSFIFO_TYPE_VALUE32, channel, b, 4);
}

bool fifoSendAddress(u32 channel, void* address)
{
    u32 a = (u32)address;
    uint8_t b[4] = { (uint8_t)a, (uint8_t)(a >> 8), (uint8_t)(a >> 16), (uint8_t)(a >> 24) };
    return sendMessage(NDSFIFO_TYPE_ADDRESS, channel, b, 4);
}

bool fifoSendDatamsg(u32 channel, u32 num_bytes, u8* data_array)
{
    return sendMessage(NDSFIFO_TYPE_DATAMSG, channel, data_array, num_bytes);
}

bool fifoSendSpecialCommand(u32 cmd)
{
    // libnds special commands are raw words (e.g. the reset handshake).
    u32 irq = rtos_disableIrqs();
    while (ipc_isSendFifoFull())
        ;
    ipc_sendWordDirect(cmd);
    rtos_restoreIrqs(irq);
    return true;
}

bool fifoSetValue32Handler(u32 channel, FifoValue32HandlerFunc newhandler, void* userdata)
{
    if (channel >= NDSFIFO_CHANNELS)
        return false;
    u32 irq = rtos_disableIrqs();
    sValueHandler[channel] = newhandler;
    sValueArg[channel] = userdata;
    rtos_restoreIrqs(irq);
    if (newhandler)
    {
        uint32_t v;
        while (1)
        {
            irq = rtos_disableIrqs();
            bool got = wqPop(&sValues[channel], &v);
            rtos_restoreIrqs(irq);
            if (!got)
                break;
            newhandler(v, userdata);
        }
    }
    return true;
}

bool fifoSetAddressHandler(u32 channel, FifoAddressHandlerFunc newhandler, void* userdata)
{
    if (channel >= NDSFIFO_CHANNELS)
        return false;
    u32 irq = rtos_disableIrqs();
    sAddressHandler[channel] = newhandler;
    sAddressArg[channel] = userdata;
    rtos_restoreIrqs(irq);
    if (newhandler)
    {
        uint32_t v;
        while (1)
        {
            irq = rtos_disableIrqs();
            bool got = wqPop(&sAddresses[channel], &v);
            rtos_restoreIrqs(irq);
            if (!got)
                break;
            newhandler((void*)v, userdata);
        }
    }
    return true;
}

bool fifoSetDatamsgHandler(u32 channel, FifoDatamsgHandlerFunc newhandler, void* userdata)
{
    if (channel >= NDSFIFO_CHANNELS)
        return false;
    u32 irq = rtos_disableIrqs();
    sDataHandler[channel] = newhandler;
    sDataArg[channel] = userdata;
    rtos_restoreIrqs(irq);
    if (newhandler)
    {
        while (sDatamsgs[channel].count)
        {
            int len = sDatamsgs[channel].length[sDatamsgs[channel].head];
            int before = sDatamsgs[channel].count;
            newhandler(len, userdata);
            if (sDatamsgs[channel].count == before)
                break; // handler didn't read it; leave it queued
        }
    }
    return true;
}

bool fifoCheckValue32(u32 channel)
{
    return channel < NDSFIFO_CHANNELS && sValues[channel].count != 0;
}

bool fifoCheckAddress(u32 channel)
{
    return channel < NDSFIFO_CHANNELS && sAddresses[channel].count != 0;
}

bool fifoCheckDatamsg(u32 channel)
{
    return channel < NDSFIFO_CHANNELS && sDatamsgs[channel].count != 0;
}

int fifoCheckDatamsgLength(u32 channel)
{
    if (!fifoCheckDatamsg(channel))
        return -1;
    return sDatamsgs[channel].length[sDatamsgs[channel].head];
}

u32 fifoGetValue32(u32 channel)
{
    uint32_t v = 0;
    if (channel >= NDSFIFO_CHANNELS)
        return 0;
    u32 irq = rtos_disableIrqs();
    wqPop(&sValues[channel], &v);
    rtos_restoreIrqs(irq);
    return v;
}

void* fifoGetAddress(u32 channel)
{
    uint32_t v = 0;
    if (channel >= NDSFIFO_CHANNELS)
        return NULL;
    u32 irq = rtos_disableIrqs();
    wqPop(&sAddresses[channel], &v);
    rtos_restoreIrqs(irq);
    return (void*)v;
}

int fifoGetDatamsg(u32 channel, int buffersize, u8* destbuffer)
{
    if (channel >= NDSFIFO_CHANNELS)
        return -1;
    u32 irq = rtos_disableIrqs();
    data_queue_t* q = &sDatamsgs[channel];
    if (q->count == 0)
    {
        rtos_restoreIrqs(irq);
        return -1;
    }
    int len = q->length[q->head];
    int copy = len < buffersize ? len : buffersize;
    if (copy > 0)
        memcpy(destbuffer, q->data[q->head], copy);
    q->head = (q->head + 1) % 4;
    q->count--;
    rtos_restoreIrqs(irq);
    return len;
}

void fifoMutexAcquire(u32 channel)
{
    while (!fifoMutexTryAcquire(channel))
        ;
}

bool fifoMutexTryAcquire(u32 channel)
{
    if (channel >= NDSFIFO_CHANNELS)
        return false;
    u32 irq = rtos_disableIrqs();
    bool ok = sMutex[channel] == 0;
    if (ok)
        sMutex[channel] = 1;
    rtos_restoreIrqs(irq);
    return ok;
}

void fifoMutexRelease(u32 channel)
{
    if (channel < NDSFIFO_CHANNELS)
        sMutex[channel] = 0;
}
