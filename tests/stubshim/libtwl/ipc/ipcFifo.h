#pragma once
#include <nds/ndstypes.h>
#ifdef __cplusplus
extern "C" {
#endif
bool ipc_isSendFifoEmpty(void);
bool ipc_isSendFifoFull(void);
void ipc_sendWordDirect(u32 word);
#ifdef __cplusplus
}
#endif
