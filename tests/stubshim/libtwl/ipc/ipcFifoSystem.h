#pragma once
#include <nds/ndstypes.h>
#define IPC_FIFO_MSG_CHANNEL_BITS 5
#define IPC_FIFO_MSG_CHANNEL_MASK 31
#define IPC_FIFO_MSG(channel, data) ((channel) | ((data) << IPC_FIFO_MSG_CHANNEL_BITS))
typedef void (*ipc_fifo_ch_handler_func_t)(u32 channel, u32 data, void* arg);
#ifdef __cplusplus
extern "C" {
#endif
void ipc_setChannelHandler(u32 channel, ipc_fifo_ch_handler_func_t handler, void* arg);
#ifdef __cplusplus
}
#endif
