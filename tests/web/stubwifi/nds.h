#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
typedef uint32_t u32; typedef uint16_t u16; typedef uint8_t u8; typedef int16_t s16; typedef int32_t s32;
static inline bool isDSiMode(void) { return true; }
#ifdef __cplusplus
extern "C"
#endif
int readFirmware(u32 addr, void* buf, u32 len);
