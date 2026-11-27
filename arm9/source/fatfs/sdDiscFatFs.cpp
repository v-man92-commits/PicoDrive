// SD card mount/unmount: FatFs, reached through fsSyscalls.cpp ("sd:" paths).
#include "common.h"
#include <string.h>
#include "ff.h"
#include "sdDisc.h"

static FATFS sFs;
static bool sMounted;

bool sd_isMounted()
{
    return sMounted;
}

bool sd_mount()
{
    if (sd_isMounted())
        return true;
    memset(&sFs, 0, sizeof(sFs));
    if (f_mount(&sFs, "", 1) != FR_OK)
    {
        f_unmount("");
        return false;
    }
    sMounted = true;
    return true;
}

bool sd_unmount()
{
    // FatFs writes everything back when a file is closed or an operation
    // (unlink, mkdir, rename) finishes, so nothing is pending here.
    if (sMounted)
        f_unmount("");
    sMounted = false;
    return true;
}

bool sd_remount()
{
    if (!sd_unmount())
        return false;
    return sd_mount();
}

sec_t sd_findPartitionStart(u8*)
{
    return sMounted ? (sec_t)sFs.volbase : 0;
}

const char* sd_fsTypeName()
{
    if (!sMounted)
        return nullptr;
    switch (sFs.fs_type)
    {
        case FS_FAT12: return "FAT12";
        case FS_FAT16: return "FAT16";
        case FS_FAT32: return "FAT32";
        case FS_EXFAT: return "exFAT";
    }
    return nullptr;
}

u64 sd_maxImageBytes()
{
    if (sMounted && sFs.fs_type == FS_EXFAT)
        return 0xFFFFFFFFull * 512; // USB side addresses images with 32-bit sector numbers
    return 4095ull * 1024 * 1024;   // FAT: 4 GiB - 1 byte per file
}
