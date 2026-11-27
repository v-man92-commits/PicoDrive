#pragma once
#include <nds/disc_io.h>
#include <nds/ndstypes.h>

/// libfat disc interface that reaches the SD card through the ARM7.
extern const DISC_INTERFACE gSdDisc;

/// Mounts the SD card as "sd:". Returns false on failure.
bool sd_mount();

/// Unmounts "sd:" (flushing all of libfat's caches). Returns true only if
/// the device is really gone afterwards.
bool sd_unmount();

/// True while libfat has "sd:" mounted.
bool sd_isMounted();

/// Flushes and drops libfat's caches by unmounting and remounting "sd:".
/// Used after creating/deleting files and after a USB session, so that
/// nothing stale or unwritten is left in the ARM9 cache.
bool sd_remount();

/// Absolute SD sector where libfat's FAT volume starts.
sec_t sd_findPartitionStart(u8* sectorBuffer);

/// Name of the SD card's filesystem ("FAT16", "FAT32", "exFAT"), or null if unknown.
const char* sd_fsTypeName();

/// Largest image file the SD card's filesystem can hold.
u64 sd_maxImageBytes();
