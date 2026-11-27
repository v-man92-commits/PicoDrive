#include "common.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "dldiIpc.h"
#include "sdDisc.h"
#include "FatMap.h"
#include "FatVolume.h"
#include "mountClient.h"
#include <unistd.h>
#ifdef IMAGE_MOUNTER_FATFS_IO
#include "ff.h"
#include "fsSyscalls.h"
#endif

static u32 mountClientFrameSeed() { return mountClient_status().frame * 2654435761u; }

namespace
{
    constexpr u32 CLUSTER_FIRST = 2;
    constexpr u32 FAT_CACHE_SECTORS = 8;

#ifndef IMAGE_MOUNTER_EXFAT
    alignas(32) u8 sFatCache[FAT_CACHE_SECTORS * 512];
    u32 sFatCacheStart = 0xFFFFFFFF;
#endif

    alignas(32) u8 sVerifyA[512];
    alignas(32) u8 sVerifyB[512];

#ifndef IMAGE_MOUNTER_EXFAT
    MapError loadVolume(FatVolume& vol)
    {
        sFatCacheStart = 0xFFFFFFFF;
        switch (fatVolume_load(vol))
        {
            case FatVolumeResult::Ok:          return MapError::None;
            case FatVolumeResult::IoError:     return MapError::IoError;
            case FatVolumeResult::Unsupported: break;
        }
        return MapError::UnsupportedVolume;
    }

    // Returns the next cluster, 0 for end of chain, 0xFFFFFFFF on error.
    u32 nextCluster(const FatVolume& vol, u32 cluster, bool& ioError)
    {
        u32 sector, offset;
        fatVolume_entryLocation(vol, cluster, sector, offset);
        if (sector < sFatCacheStart || sector >= sFatCacheStart + FAT_CACHE_SECTORS)
        {
            if (!dldi_readSectors(sFatCache, sector, FAT_CACHE_SECTORS))
            {
                ioError = true;
                return 0xFFFFFFFF;
            }
            sFatCacheStart = sector;
        }
        u32 next = fatVolume_readEntry(vol, sFatCache + (sector - sFatCacheStart) * 512 + offset);
        if (next == 0)
            return 0xFFFFFFFF; // free cluster inside a chain: damaged
        if (vol.fat32 ? next >= 0x0FFFFFF7 : next >= 0xFFF7)
            return 0;
        return next;
    }
#endif
}

const char* mapErrorString(MapError error)
{
    switch (error)
    {
        case MapError::None:              return "OK";
        case MapError::OpenFailed:        return "Can't open file";
        case MapError::EmptyFile:         return "File is empty";
#ifdef IMAGE_MOUNTER_EXFAT
        case MapError::UnsupportedVolume: return "Unsupported SD format";
#else
        case MapError::UnsupportedVolume: return "SD must be FAT16/FAT32";
#endif
        case MapError::TooLarge:          return "Image too large";
        case MapError::BrokenChain:       return "FAT chain is damaged";
        case MapError::TooFragmented:     return "File too fragmented";
        case MapError::IoError:           return "SD read error";
        case MapError::VerifyFailed:      return "Map check failed";
    }
    return "?";
}

#ifndef IMAGE_MOUNTER_EXFAT // the exFAT build maps files with FatFs (fatfs/FatMapFatFs.cpp)
MapError fatMap_build(const char* path, ImageMap& map)
{
    map.extentCount = 0;
    map.totalSectors = 0;
    map.fileSize = 0;

    struct stat st;
    if (stat(path, &st) != 0)
        return MapError::OpenFailed;

    // off_t is 32 bit: treat as unsigned so 2-4 GiB images work
    u32 fileSize = (u32)st.st_size;
    u32 startCluster = (u32)st.st_ino; // libfat: st_ino is the start cluster
    map.fileSize = fileSize;
    map.totalSectors = map.roundUp ? (u32)((fileSize + MOUNT_SECTOR_SIZE - 1) / MOUNT_SECTOR_SIZE)
                                   : fileSize / MOUNT_SECTOR_SIZE;
    if (map.totalSectors == 0 || startCluster < CLUSTER_FIRST)
        return MapError::EmptyFile;

    FatVolume vol;
    MapError err = loadVolume(vol);
    if (err != MapError::None)
        return err;
    if (startCluster > vol.lastCluster)
        return MapError::BrokenChain;

    u32 clusterSectors = vol.sectorsPerCluster;
    u32 clustersNeeded = (map.totalSectors + clusterSectors - 1) / clusterSectors;

    bool storing = map.extents && map.maxExtents > 0;
    u32 runFileSector = 0;
    u32 runSdSector = fatVolume_clusterToSector(vol, startCluster);
    u32 runCount = 0;
    u32 extents = 0;
    u32 cluster = startCluster;
    bool ioError = false;

    for (u32 i = 0; i < clustersNeeded; i++)
    {
        if (cluster < CLUSTER_FIRST || cluster > vol.lastCluster)
            return MapError::BrokenChain;

        u32 sd = fatVolume_clusterToSector(vol, cluster);
        if (runCount > 0 && sd != runSdSector + runCount)
        {
            // close the current run
            if (storing)
            {
                if (extents >= map.maxExtents)
                    return MapError::TooFragmented;
                map.extents[extents] = { runFileSector, runSdSector, runCount };
            }
            extents++;
            runFileSector += runCount;
            runSdSector = sd;
            runCount = 0;
        }
        runCount += clusterSectors;

        if (i + 1 < clustersNeeded)
        {
            cluster = nextCluster(vol, cluster, ioError);
            if (ioError)
                return MapError::IoError;
            if (cluster == 0 || cluster == 0xFFFFFFFF)
                return MapError::BrokenChain; // chain ended early
        }
    }

    // Final run: clip to the image size (the last cluster may be partly unused)
    u32 remaining = map.totalSectors - runFileSector;
    if (runCount > remaining)
        runCount = remaining;
    if (storing)
    {
        if (extents >= map.maxExtents)
            return MapError::TooFragmented;
        map.extents[extents] = { runFileSector, runSdSector, runCount };
    }
    extents++;

    map.extentCount = extents;
    return MapError::None;
}

#endif

static bool readViaMap(const ImageMap& map, u32 fileSector, u8* out)
{
    for (u32 i = 0; i < map.extentCount; i++)
    {
        const mount_extent_t& e = map.extents[i];
        if (fileSector >= e.fileSector && fileSector < e.fileSector + e.count)
            return dldi_readSectors(out, e.sdSector + (fileSector - e.fileSector), 1);
    }
    return false;
}

#ifdef IMAGE_MOUNTER_FATFS_IO
// BlocksDS build: picolibc's off_t is 32-bit (2 GiB), so the verify reads
// and writes go straight to FatFs, which uses 64-bit offsets.
typedef FIL VerifyFile;

static bool verifyOpen(VerifyFile* f, const char* path, bool writable)
{
    return f_open(f, fs_ffPath(path), FA_READ | (writable ? FA_WRITE : 0)) == FR_OK;
}

static void verifyClose(VerifyFile* f)
{
    f_close(f);
}

static bool readViaLibfat(VerifyFile* f, u32 fileSector, u8* out)
{
    UINT br = 0;
    return f_lseek(f, (FSIZE_t)fileSector * MOUNT_SECTOR_SIZE) == FR_OK
        && f_read(f, out, MOUNT_SECTOR_SIZE, &br) == FR_OK && br == MOUNT_SECTOR_SIZE;
}
#else
typedef FILE VerifyFile;

static bool readViaLibfat(FILE* f, u32 fileSector, u8* out)
{
    // off_t is 64-bit in devkitARM, so fseeko reaches the whole 4 GiB
    if (fseeko(f, (off_t)fileSector * MOUNT_SECTOR_SIZE, SEEK_SET) != 0)
        return false;
    return fread(out, 1, MOUNT_SECTOR_SIZE, f) == MOUNT_SECTOR_SIZE;
}
#endif

static bool isUniform(const u8* p)
{
    for (u32 i = 1; i < MOUNT_SECTOR_SIZE; i++)
        if (p[i] != p[0])
            return false;
    return true;
}

#ifdef IMAGE_MOUNTER_FATFS_IO
static bool writeViaLibfat(VerifyFile* f, u32 fileSector, const u8* data)
{
    UINT bw = 0;
    return f_lseek(f, (FSIZE_t)fileSector * MOUNT_SECTOR_SIZE) == FR_OK
        && f_write(f, data, MOUNT_SECTOR_SIZE, &bw) == FR_OK && bw == MOUNT_SECTOR_SIZE
        && f_sync(f) == FR_OK;
}
#else
static bool writeViaLibfat(FILE* f, u32 fileSector, const u8* data)
{
    if (fseeko(f, (off_t)fileSector * MOUNT_SECTOR_SIZE, SEEK_SET) != 0)
        return false;
    if (fwrite(data, 1, MOUNT_SECTOR_SIZE, f) != MOUNT_SECTOR_SIZE)
        return false;
    fflush(f);
    return fsync(fileno(f)) == 0; // libfat: flush its cache to the SD card
}
#endif

static u32 sPatternSeed = 0x9E3779B9;

/// prove: a blank sample may be proven with a test pattern (writable mode).
static bool checkSector(VerifyFile* f, const ImageMap& map, u32 fileSector, bool writable, bool prove, bool& compared)
{
    if (fileSector >= map.totalSectors)
        return false;
    if (!readViaLibfat(f, fileSector, sVerifyA))
        return false;
    if (!readViaMap(map, fileSector, sVerifyB))
        return false;
    if (memcmp(sVerifyA, sVerifyB, MOUNT_SECTOR_SIZE) != 0)
        return false;

    if (isUniform(sVerifyA))
    {
        // A blank sector would match any other blank area of the card, so it
        // proves nothing on its own.
        if (!writable || !prove)
            return true;
        // Prove the location: write a unique pattern through libfat, read it
        // back through the map, then restore the original contents.
        alignas(32) static u8 pattern[MOUNT_SECTOR_SIZE];
        for (u32 i = 0; i < MOUNT_SECTOR_SIZE; i += 4)
        {
            sPatternSeed = sPatternSeed * 1664525 + 1013904223;
            u32 v = sPatternSeed ^ fileSector;
            memcpy(pattern + i, &v, 4);
        }
        bool ok = writeViaLibfat(f, fileSector, pattern)
            && readViaMap(map, fileSector, sVerifyB)
            && memcmp(pattern, sVerifyB, MOUNT_SECTOR_SIZE) == 0;
        // always try to restore, even if the check failed
        bool restored = writeViaLibfat(f, fileSector, sVerifyA);
        if (!ok || !restored)
            return false;
    }
    compared = true;
    return true;
}

namespace
{
    constexpr u32 STRIDE_SAMPLES = 64;
    constexpr u32 PROVE_FLAG = 0x80000000u;   // sample may be pattern-proven
    u32 sSamples[MOUNT_MAX_EXTENTS * 2 + STRIDE_SAMPLES + 8];
}

MapError fatMap_verify(const char* path, const ImageMap& map, bool writable,
    void (*progress)(u32 done, u32 total))
{
    if (!map.extents || map.extentCount == 0 || map.totalSectors == 0)
        return MapError::VerifyFailed;

    // libfat truncates in-place writes to files within 512 bytes of 4 GiB
    // (it treats them as appends), so the pattern test can't be used there.
#ifndef IMAGE_MOUNTER_EXFAT
    if ((u64)map.fileSize + MOUNT_SECTOR_SIZE > 0xFFFFFFFFull)
        writable = false;
#endif

    // Samples, in increasing order so libfat only ever seeks forwards:
    //  - first and last sector of every run (catches a wrong start/length);
    //  - sectors spread evenly through the image (catches a wrong map using
    //    real data, even when the run edges are blank - e.g. ISO images,
    //    which begin with 32 KiB of zeros).
    u32 count = 0;
    u32 stride = map.totalSectors / STRIDE_SAMPLES;
    if (stride == 0)
        stride = 1;
    u32 nextStride = stride / 2;
    auto addStrideUpTo = [&](u32 limit)
    {
        while (nextStride < map.totalSectors && nextStride < limit)
        {
            if (count == 0 || (sSamples[count - 1] & ~PROVE_FLAG) != nextStride)
                sSamples[count++] = nextStride;
            nextStride += stride;
        }
    };
    auto addEdge = [&](u32 sector)
    {
        // the extents can reach past totalSectors (a rounded-up map checked
        // on its whole sectors only): sample the last sector checked instead
        if (sector >= map.totalSectors)
            sector = map.totalSectors - 1;
        addStrideUpTo(sector);
        if (count > 0 && (sSamples[count - 1] & ~PROVE_FLAG) == sector)
            sSamples[count - 1] |= PROVE_FLAG;
        else
            sSamples[count++] = sector | PROVE_FLAG;
    };
    for (u32 i = 0; i < map.extentCount; i++)
    {
        const mount_extent_t& e = map.extents[i];
        addEdge(e.fileSector);
        if (e.count > 1)
            addEdge(e.fileSector + e.count - 1);
    }
    addStrideUpTo(map.totalSectors);

#ifdef IMAGE_MOUNTER_FATFS_IO
    static FIL sVerifyFile;
    VerifyFile* f = &sVerifyFile;
    if (!verifyOpen(f, path, writable))
        return MapError::OpenFailed;
#else
    FILE* f = fopen(path, writable ? "r+b" : "rb");
    if (!f)
        return MapError::OpenFailed;
    setvbuf(f, NULL, _IONBF, 0);
#endif
    sPatternSeed ^= mountClientFrameSeed();

    bool ok = true;
    bool compared = false;
    for (u32 i = 0; ok && i < count; i++)
    {
        u32 sector = sSamples[i] & ~PROVE_FLAG;
        bool prove = (sSamples[i] & PROVE_FLAG) != 0;
        ok = checkSector(f, map, sector, writable, prove, compared);
        if (progress && (i & 63) == 63)
            progress(i + 1, count);
    }

#ifdef IMAGE_MOUNTER_FATFS_IO
    verifyClose(f);
#else
    fclose(f);
#endif
    // Writable mounts must have proven at least one location. A read-only
    // mount of an entirely blank file is harmless even if unproven.
    return (ok && (compared || !writable)) ? MapError::None : MapError::VerifyFailed;
}
