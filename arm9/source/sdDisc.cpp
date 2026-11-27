#ifndef IMAGE_MOUNTER_EXFAT // the exFAT build uses fatfs/sdDiscFatFs.cpp
#include "common.h"
#include <string.h>
#include <fat.h>
#include <sys/iosupport.h>
#include "dldiIpc.h"
#include "sdDisc.h"

// Exported (but not declared) by libfat 1.1.5: the exact routine fatMount()
// uses to choose a partition when no start sector is given. Calling it
// guarantees that our sector maths matches the volume libfat mounted.
extern "C" sec_t FindFirstValidPartition(const DISC_INTERFACE* disc);

static bool startup() { return true; }
static bool isInserted() { return true; }
static bool clearStatus() { return true; }
static bool shutdown() { return true; }

static bool readSectors(sec_t sector, sec_t numSectors, void* buffer)
{
    return dldi_readSectors(buffer, sector, numSectors);
}

static bool writeSectors(sec_t sector, sec_t numSectors, const void* buffer)
{
    return dldi_writeSectors(buffer, sector, numSectors);
}

const DISC_INTERFACE gSdDisc =
{
    0x43504944, // 'DIPC'
    FEATURE_MEDIUM_CANREAD | FEATURE_MEDIUM_CANWRITE | FEATURE_SLOT_NDS,
    startup,
    isInserted,
    readSectors,
    writeSectors,
    clearStatus,
    shutdown
};

bool sd_isMounted()
{
    // devoptab lookups only work with the colon ("sd:"); without it the
    // device is never found.
    return FindDevice("sd:") >= 0;
}

bool sd_mount()
{
    // 16 pages of 8 sectors = 64 KiB of cache
    return fatMount("sd", &gSdDisc, 0, 16, 8) && sd_isMounted();
}

bool sd_unmount()
{
    // Writes the FSInfo sector, flushes and frees libfat's cache.
    // NOTE: must be "sd:" - fatUnmount("sd") silently does nothing.
    fatUnmount("sd:");
    return !sd_isMounted();
}

bool sd_remount()
{
    if (!sd_unmount())
        return false;
    return sd_mount();
}

sec_t sd_findPartitionStart(u8* sectorBuffer)
{
    // Mirrors _FAT_partition_constructor with startSector == 0
    if (!dldi_readSectors(sectorBuffer, 0, 1))
        return 0;
    if (!memcmp(sectorBuffer + 0x36, "FAT", 3) || !memcmp(sectorBuffer + 0x52, "FAT", 3))
        return 0;
    return FindFirstValidPartition(&gSdDisc);
}

const char* sd_fsTypeName()
{
    return nullptr;
}

// libfat treats an in-place write within 512 bytes of 4 GiB as an append and
// truncates it, so the libfat build stops at 4095 MiB.
u64 sd_maxImageBytes()
{
    return 4095ull * 1024 * 1024;
}
#endif
