#include "common.h"
#include "ImageDisk.h"
#include "MountIpcService.h"
#include "exitToMenu7.h"

void MountIpcService::HandleMessage(u32 data)
{
    auto cmd = reinterpret_cast<const mount_ipc_cmd_t*>(data << 2);
    u32 result = MOUNT_RESULT_BAD_ARGS;
    switch (cmd->cmd)
    {
        case MOUNT_IPC_CMD_INIT:
            imageDisk_setStatus(static_cast<mount_status_t*>(cmd->buffer));
            result = MOUNT_RESULT_OK;
            break;

        case MOUNT_IPC_CMD_MOUNT:
            result = imageDisk_mount(cmd);
            break;

        case MOUNT_IPC_CMD_UNMOUNT:
            result = imageDisk_unmount();
            break;

        case MOUNT_IPC_CMD_RELOAD:
            result = imageDisk_reload();
            break;

        case MOUNT_IPC_CMD_EXIT:
        {
            u32 mode = cmd->mediaType;
            u32 entry = (u32)cmd->buffer;
            if (mode != MOUNT_EXIT_BOOTSTUB && mode != MOUNT_EXIT_PICO_LOADER)
                break;
            imageDisk_shutdown();
            SendResponseMessage(MOUNT_RESULT_OK); // the ARM9 goes on once it sees this
            exit7_quiesce();
            if (mode == MOUNT_EXIT_PICO_LOADER)
                exit7_jump(entry);
            exit7_bootstubHandoff();
        }

    }
    SendResponseMessage(result);
}
