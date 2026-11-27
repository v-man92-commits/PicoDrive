#pragma once
#include "ipc/ThreadIpcService.h"
#include "mountIpc.h"
#include "ipcChannels.h"

class MountIpcService : public ThreadIpcService
{
    u32 _threadStack[192];

public:
    MountIpcService()
        : ThreadIpcService(IPC_CHANNEL_MOUNT, 5, _threadStack, sizeof(_threadStack)) { }

    void HandleMessage(u32 data) override;
};
