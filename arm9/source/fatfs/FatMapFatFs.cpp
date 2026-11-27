// exFAT build: resolves an image file into SD sector runs with FatFs' fast-seek
// cluster link map. Works the same on FAT12/16/32 and exFAT (including exFAT's
// contiguous "no FAT chain" files). fatMap_verify (FatMap.cpp) then checks the
// result against data read through the filesystem before anything is mounted.
#include "common.h"
#include <string.h>
#include "ff.h"
#include "fsSyscalls.h"
#include "FatMap.h"

namespace
{
    // (fragments + 1) * 2 entries are needed; one extra fragment of headroom
    // so "too fragmented" can still report the real count.
    DWORD sLinkMap[(MOUNT_MAX_EXTENTS + 2) * 2];
}

MapError fatMap_build(const char* path, ImageMap& map)
{
    map.extentCount = 0;
    map.totalSectors = 0;
    map.fileSize = 0;

    FIL f;
    if (f_open(&f, fs_ffPath(path), FA_READ) != FR_OK)
        return MapError::OpenFailed;

    MapError result = MapError::None;
    u64 size = f_size(&f);
    map.fileSize = size;
    u64 sectors = map.roundUp ? (size + MOUNT_SECTOR_SIZE - 1) / MOUNT_SECTOR_SIZE : size / MOUNT_SECTOR_SIZE;
    if (sectors > 0xFFFFFFFFull)
    {
        f_close(&f);
        return MapError::TooLarge;
    }
    map.totalSectors = (u32)sectors;
    if (map.totalSectors == 0 || f.obj.sclust < 2)
    {
        f_close(&f);
        return MapError::EmptyFile;
    }

    sLinkMap[0] = sizeof(sLinkMap) / sizeof(sLinkMap[0]);
    f.cltbl = sLinkMap;
    FRESULT res = f_lseek(&f, CREATE_LINKMAP);
    if (res == FR_NOT_ENOUGH_CORE)
    {
        // sLinkMap[0] now holds the size that would have been needed
        map.extentCount = sLinkMap[0] / 2 - 1;
        f_close(&f);
        return MapError::TooFragmented;
    }
    if (res != FR_OK)
    {
        f_close(&f);
        return res == FR_INT_ERR ? MapError::BrokenChain : MapError::IoError;
    }

    const FATFS* fs = f.obj.fs;
    const u32 clusterSectors = fs->csize;
    const bool storing = map.extents && map.maxExtents > 0;
    u32 fileSector = 0;
    u32 extents = 0;
    const DWORD* t = sLinkMap + 1;
    while (fileSector < map.totalSectors)
    {
        DWORD clusters = *t++;
        if (clusters == 0)
        {
            result = MapError::BrokenChain; // allocation shorter than the file
            break;
        }
        DWORD cluster = *t++;
        if (cluster < 2 || cluster >= fs->n_fatent)
        {
            result = MapError::BrokenChain;
            break;
        }
        u32 sd = (u32)(fs->database + (LBA_t)(cluster - 2) * clusterSectors);
        u64 count = (u64)clusters * clusterSectors;
        if (count > map.totalSectors - fileSector)
            count = map.totalSectors - fileSector; // last cluster partly used
        if (storing)
        {
            if (extents >= map.maxExtents)
            {
                result = MapError::TooFragmented;
                break;
            }
            map.extents[extents] = { fileSector, sd, (u32)count };
        }
        extents++;
        fileSector += (u32)count;
    }

    map.extentCount = extents;
    f_close(&f);
    return result;
}
