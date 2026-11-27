#pragma once
#include <nds/ndstypes.h>
#ifdef __cplusplus
extern "C" {
#endif
u32 rtos_disableIrqs(void);
void rtos_restoreIrqs(u32 v);
#ifdef __cplusplus
}
#endif
