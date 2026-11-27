#include <nds/system.h>
#include "common.h"
#include <string.h>
#include <nds/arm9/cache.h>
#include <libtwl/rtos/rtosEvent.h>
#include <libtwl/rtos/rtosMutex.h>
#include <libtwl/ipc/ipcFifoSystem.h>
#include "ipcChannels.h"
#include "mountClient.h"

alignas(32) static mount_ipc_cmd_t sCmd;
alignas(32) static mount_status_t sStatus;      // written by the ARM7
alignas(32) static mount_status_t sStatusCopy;  // stable copy for the UI

static rtos_event_t sEvent;
static rtos_mutex_t sMutex;
static volatile u32 sResult;

static void ipcHandler(u32 channel, u32 data, void* arg)
{
    sResult = data;
    rtos_signalEvent(&sEvent);
}

static u32 sendCommand()
{
    rtos_clearEvent(&sEvent);
    DC_FlushRange(&sCmd, sizeof(sCmd));
    ipc_sendFifoMessage(IPC_CHANNEL_MOUNT, (u32)&sCmd >> 2);
    rtos_waitEvent(&sEvent, false, true);
    return sResult;
}

void mountClient_init()
{
    rtos_createEvent(&sEvent);
    rtos_createMutex(&sMutex);
    ipc_setChannelHandler(IPC_CHANNEL_MOUNT, ipcHandler, nullptr);

    memset(&sStatus, 0, sizeof(sStatus));
    DC_FlushRange(&sStatus, sizeof(sStatus));

    rtos_lockMutex(&sMutex);
    memset(&sCmd, 0, sizeof(sCmd));
    sCmd.cmd = MOUNT_IPC_CMD_INIT;
    sCmd.buffer = &sStatus;
    sendCommand();
    rtos_unlockMutex(&sMutex);
}

void mountClient_poll()
{
    DC_InvalidateRange(&sStatus, sizeof(sStatus));
    memcpy(&sStatusCopy, (const void*)&sStatus, sizeof(sStatus));
}

void mountClient_peekStatus(mount_status_t* out)
{
    // Reads through the uncached main RAM mirror: no cache maintenance, so it
    // is safe from an interrupt handler.
    memcpy(out, memUncached((void*)&sStatus), sizeof(*out));
}

const mount_status_t& mountClient_status()
{
    return sStatusCopy;
}

u32 mountClient_mount(const mount_extent_t* extents, u32 extentCount, u32 totalSectors,
    MountMediaType mediaType, bool readOnly, const cd_layout_t* cdLayout)
{
    DC_FlushRange(extents, extentCount * sizeof(mount_extent_t));
    if (cdLayout)
    {
        // the ARM7 reads these straight from main RAM
        DC_FlushRange(cdLayout, sizeof(cd_layout_t));
        if (cdLayout->cdText)
            DC_FlushRange(cdLayout->cdText, cdLayout->cdTextLength);
        DC_FlushRange(cdLayout->bounce, cdLayout->bounceSize); // and writes into this one
    }
    rtos_lockMutex(&sMutex);
    memset(&sCmd, 0, sizeof(sCmd));
    sCmd.cmd = MOUNT_IPC_CMD_MOUNT;
    sCmd.extents = extents;
    sCmd.extentCount = extentCount;
    sCmd.totalSectors = totalSectors;
    sCmd.mediaType = mediaType;
    sCmd.readOnly = readOnly ? 1 : 0;
    sCmd.cdLayout = cdLayout;
    u32 result = sendCommand();
    rtos_unlockMutex(&sMutex);
    return result;
}

u32 mountClient_exit(MountExitMode mode, u32 arm7Entry)
{
    rtos_lockMutex(&sMutex);
    memset(&sCmd, 0, sizeof(sCmd));
    sCmd.cmd = MOUNT_IPC_CMD_EXIT;
    sCmd.mediaType = mode;
    sCmd.buffer = (void*)arm7Entry;
    u32 result = sendCommand();
    rtos_unlockMutex(&sMutex);
    return result;
}

u32 mountClient_reload()
{
    rtos_lockMutex(&sMutex);
    memset(&sCmd, 0, sizeof(sCmd));
    sCmd.cmd = MOUNT_IPC_CMD_RELOAD;
    u32 result = sendCommand();
    rtos_unlockMutex(&sMutex);
    return result;
}

u32 mountClient_unmount()
{
    rtos_lockMutex(&sMutex);
    memset(&sCmd, 0, sizeof(sCmd));
    sCmd.cmd = MOUNT_IPC_CMD_UNMOUNT;
    u32 result = sendCommand();
    rtos_unlockMutex(&sMutex);
    return result;
}
