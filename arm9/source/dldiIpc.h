#pragma once

bool dldi_init();

#ifdef __cplusplus
extern "C" {
#endif

/// Read/write SD sectors through the ARM7 (which owns the cartridge bus).
/// Returns false if the DLDI driver reported an error.
bool dldi_readSectors(void* buffer, u32 sector, u32 count);
bool dldi_writeSectors(const void* buffer, u32 sector, u32 count);

#ifdef __cplusplus
}
#endif
