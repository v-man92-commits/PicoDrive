#ifndef IMAGE_MOUNTER_EXFAT // the exFAT build uses fatfs/QuickAllocFatFs.cpp
#include "common.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "dldiIpc.h"
#include "FatVolume.h"
#include "mountIpc.h"
#include "QuickAlloc.h"

#ifndef QUICKALLOC_NO_LIBFAT
#include "sdDisc.h"
#endif

namespace
{
    constexpr u32 CHUNK_SECTORS = 64;          // 32 KiB FAT read/write chunks
    constexpr u32 MAX_RUNS = MOUNT_MAX_EXTENTS;

    alignas(32) u8 sChunk[CHUNK_SECTORS * 512];
    alignas(32) u8 sSector[512];

    struct Run
    {
        u32 start;
        u32 count;
    };
    Run sRuns[MAX_RUNS];

    inline u32 rd32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
    inline void wr32(u8* p, u32 v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

    u32 entriesPerSector(const FatVolume& vol) { return vol.fat32 ? 128 : 256; }

    /// Writes a FAT region (read from the copy libfat uses) to every FAT copy.
    bool writeFatSectors(const FatVolume& vol, u32 sector, u32 count, const u8* data)
    {
        u32 rel = (sector - vol.fatStart);
        u32 firstFat = vol.fatStart - vol.activeFat * vol.sectorsPerFat;
        for (u32 copy = 0; copy < vol.numFats; copy++)
        {
            if (!dldi_writeSectors(data, firstFat + copy * vol.sectorsPerFat + rel, count))
                return false;
        }
        return true;
    }

    /// Sets FAT entries [first, first + count): each points to the next
    /// cluster, the last one gets `lastValue`. With clear = true every entry
    /// is set to 0 (free) instead. Read-modify-write in chunks.
    bool writeChain(const FatVolume& vol, u32 first, u32 count, u32 lastValue, bool clear = false)
    {
        u32 eps = entriesPerSector(vol);
        u32 entrySize = vol.fat32 ? 4 : 2;
        u32 c = first;
        u32 end = first + count;
        while (c < end)
        {
            u32 sector, offset;
            fatVolume_entryLocation(vol, c, sector, offset);
            u32 lastSector, lastOffset;
            fatVolume_entryLocation(vol, end - 1, lastSector, lastOffset);
            u32 n = lastSector - sector + 1;
            if (n > CHUNK_SECTORS)
                n = CHUNK_SECTORS;
            if (!dldi_readSectors(sChunk, sector, n))
                return false;
            u32 chunkEnd = (sector - vol.fatStart + n) * eps; // first cluster after this chunk
            for (; c < end && c < chunkEnd; c++)
            {
                u32 byte = (c - (sector - vol.fatStart) * eps) * entrySize;
                fatVolume_writeEntry(vol, sChunk + byte, clear ? 0 : (c + 1 < end ? c + 1 : lastValue));
            }
            if (!writeFatSectors(vol, sector, n, sChunk))
                return false;
        }
        return true;
    }

    bool setEntry(const FatVolume& vol, u32 cluster, u32 value)
    {
        u32 sector, offset;
        fatVolume_entryLocation(vol, cluster, sector, offset);
        if (!dldi_readSectors(sSector, sector, 1))
            return false;
        fatVolume_writeEntry(vol, sSector + offset, value);
        return writeFatSectors(vol, sector, 1, sSector);
    }

    u32 readEntry(const FatVolume& vol, u32 cluster, bool& ok)
    {
        u32 sector, offset;
        fatVolume_entryLocation(vol, cluster, sector, offset);
        ok = dldi_readSectors(sSector, sector, 1);
        return ok ? fatVolume_readEntry(vol, sSector + offset) : 0;
    }

    /// Scans the FAT for free space. Prefers the first free run that is big
    /// enough on its own; otherwise collects the first free runs in order.
    AllocError findFreeRuns(const FatVolume& vol, u32 needed, u32& runCount, AllocProgressFn progress)
    {
        u32 eps = entriesPerSector(vol);
        u32 entrySize = vol.fat32 ? 4 : 2;
        u32 totalEntries = vol.lastCluster + 1;
        u32 fatSectorsUsed = (totalEntries + eps - 1) / eps;

        u32 greedyCount = 0;     // runs collected for the fragmented fallback
        u32 greedyTotal = 0;
        bool greedyOverflow = false;
        u32 runStart = 0;
        u32 runLen = 0;

        auto closeRun = [&]() -> bool // returns true if a single big run was found
        {
            if (runLen == 0)
                return false;
            if (runLen >= needed)
            {
                sRuns[0] = { runStart, needed };
                runCount = 1;
                return true;
            }
            if (greedyTotal < needed)
            {
                if (greedyCount >= MAX_RUNS)
                {
                    greedyOverflow = true;
                }
                else
                {
                    u32 take = runLen;
                    if (take > needed - greedyTotal)
                        take = needed - greedyTotal;
                    sRuns[greedyCount++] = { runStart, take };
                    greedyTotal += take;
                }
            }
            runLen = 0;
            return false;
        };

        for (u32 s = 0; s < fatSectorsUsed; s += CHUNK_SECTORS)
        {
            u32 n = fatSectorsUsed - s;
            if (n > CHUNK_SECTORS)
                n = CHUNK_SECTORS;
            if (!dldi_readSectors(sChunk, vol.fatStart + s, n))
                return AllocError::IoError;
            u32 firstCluster = s * eps;
            u32 lastCluster = firstCluster + n * eps; // exclusive
            if (lastCluster > totalEntries)
                lastCluster = totalEntries;
            for (u32 c = firstCluster < 2 ? 2 : firstCluster; c < lastCluster; c++)
            {
                u32 value = fatVolume_readEntry(vol, sChunk + (c - firstCluster) * entrySize);
                if (value == 0)
                {
                    if (runLen == 0)
                        runStart = c;
                    runLen++;
                    if (runLen >= needed && closeRun())
                        return AllocError::None;
                }
                else if (closeRun())
                {
                    return AllocError::None;
                }
            }
            if (progress)
                progress("Finding free space", s + n, fatSectorsUsed);
        }
        if (closeRun())
            return AllocError::None;

        if (greedyTotal < needed)
            return greedyOverflow ? AllocError::TooFragmented : AllocError::NoSpace;
        runCount = greedyCount;
        return AllocError::None;
    }

    /// Finds the placeholder's 32-byte directory entry (by its start
    /// cluster and size) and points it at the new chain.
    AllocError updateDirEntry(const FatVolume& vol, u32 dirCluster, u32 placeholder, u32 newFirst, u32 newSize)
    {
        u32 cluster = dirCluster;
        for (u32 guard = 0; guard < 65536; guard++)
        {
            if (cluster < 2 || cluster > vol.lastCluster)
                return AllocError::EntryNotFound;
            u32 base = fatVolume_clusterToSector(vol, cluster);
            for (u32 s = 0; s < vol.sectorsPerCluster; s++)
            {
                if (!dldi_readSectors(sSector, base + s, 1))
                    return AllocError::IoError;
                for (u32 e = 0; e < 16; e++)
                {
                    u8* d = sSector + e * 32;
                    if (d[0] == 0x00)
                        return AllocError::EntryNotFound; // end of directory
                    if (d[0] == 0xE5 || d[11] == 0x0F || (d[11] & 0x18))
                        continue; // deleted, long name, volume label or directory
                    u32 start = d[26] | (d[27] << 8);
                    if (vol.fat32)
                        start |= (u32)(d[20] | (d[21] << 8)) << 16;
                    if (start != placeholder || rd32(d + 28) != 1)
                        continue;
                    d[26] = newFirst;
                    d[27] = newFirst >> 8;
                    if (vol.fat32)
                    {
                        d[20] = newFirst >> 16;
                        d[21] = newFirst >> 24;
                    }
                    wr32(d + 28, newSize);
                    return dldi_writeSectors(sSector, base + s, 1) ? AllocError::None : AllocError::IoError;
                }
            }
            bool ok;
            u32 next = readEntry(vol, cluster, ok);
            if (!ok)
                return AllocError::IoError;
            if (vol.fat32 ? next >= 0x0FFFFFF8 : next >= 0xFFF8)
                return AllocError::EntryNotFound;
            cluster = next;
        }
        return AllocError::EntryNotFound;
    }

    /// Keeps the FAT32 FSInfo free-cluster count exact.
    bool updateFsInfo(const FatVolume& vol, s32 delta)
    {
        if (!vol.fat32 || vol.fsInfoSector == 0)
            return true;
        if (!dldi_readSectors(sSector, vol.fsInfoSector, 1))
            return false;
        if (rd32(sSector) != 0x41615252 || rd32(sSector + 484) != 0x61417272)
            return true; // no valid FSInfo: nothing to keep in sync
        u32 freeCount = rd32(sSector + 488);
        if (freeCount == 0xFFFFFFFF)
            return true; // already "unknown"
        s64 updated = (s64)freeCount + delta;
        wr32(sSector + 488, (updated < 0 || updated > (s64)vol.lastCluster) ? 0xFFFFFFFF : (u32)updated);
        return dldi_writeSectors(sSector, vol.fsInfoSector, 1);
    }
}

const char* allocErrorString(AllocError error)
{
    switch (error)
    {
        case AllocError::None:          return "OK";
        case AllocError::IoError:       return "SD card I/O error";
        case AllocError::Unsupported:   return "SD must be FAT16/FAT32";
        case AllocError::NoSpace:       return "Not enough free space";
        case AllocError::TooFragmented: return "Free space too fragmented";
        case AllocError::CreateFailed:  return "Couldn't create the file";
        case AllocError::EntryNotFound: return "Directory entry not found";
        case AllocError::VerifyFailed:  return "Check after create failed";
    }
    return "?";
}

AllocError quickAlloc_allocateRaw(u32 dirCluster, u32 placeholderCluster, u64 bytes,
    AllocResult& result, AllocProgressFn progress)
{
    memset(&result, 0, sizeof(result));
    if (bytes == 0 || bytes > 0xFFFFFFFFull)
        return AllocError::Unsupported;

    FatVolume vol;
    switch (fatVolume_load(vol))
    {
        case FatVolumeResult::Ok: break;
        case FatVolumeResult::IoError: return AllocError::IoError;
        case FatVolumeResult::Unsupported: return AllocError::Unsupported;
    }
    if (!vol.mirrored)
        return AllocError::Unsupported; // unusual single-FAT layout: don't guess
    if (placeholderCluster < 2 || placeholderCluster > vol.lastCluster ||
        dirCluster < 2 || dirCluster > vol.lastCluster)
    {
        return AllocError::Unsupported;
    }

    u32 clusterBytes = vol.sectorsPerCluster * 512;
    u32 needed = (u32)((bytes + clusterBytes - 1) / clusterBytes);

    // Sanity: the placeholder must currently be a one-cluster chain
    bool ok;
    u32 placeholderEntry = readEntry(vol, placeholderCluster, ok);
    if (!ok)
        return AllocError::IoError;
    if (vol.fat32 ? placeholderEntry < 0x0FFFFFF8 : placeholderEntry < 0xFFF8)
        return AllocError::EntryNotFound;

    u32 runCount = 0;
    AllocError err = findFreeRuns(vol, needed, runCount, progress);
    if (err != AllocError::None)
        return err;

    // 1. Write the new chain. Until the directory entry points at it these
    //    clusters are merely "lost", never shared with another file.
    u32 done = 0;
    for (u32 i = 0; i < runCount; i++)
    {
        u32 lastValue = (i + 1 < runCount) ? sRuns[i + 1].start : fatVolume_endOfChain(vol);
        if (!writeChain(vol, sRuns[i].start, sRuns[i].count, lastValue))
            return AllocError::IoError;
        done += sRuns[i].count;
        if (progress)
            progress("Reserving clusters", done, needed);
    }

    // 2. Point the file at the new chain (the placeholder cluster becomes lost)
    err = updateDirEntry(vol, dirCluster, placeholderCluster, sRuns[0].start, (u32)bytes);
    if (err != AllocError::None)
    {
        // Nothing references the new chain yet: give the clusters back
        for (u32 i = 0; i < runCount; i++)
            writeChain(vol, sRuns[i].start, sRuns[i].count, 0, true);
        return err;
    }

    // 3. Release the placeholder cluster
    if (!setEntry(vol, placeholderCluster, 0))
        return AllocError::IoError;

    // 4. Free space bookkeeping: `needed` clusters used, placeholder released
    if (!updateFsInfo(vol, 1 - (s32)needed))
        return AllocError::IoError;

    result.firstCluster = sRuns[0].start;
    result.clusters = needed;
    result.runs = runCount;
    return AllocError::None;
}

#ifndef QUICKALLOC_NO_LIBFAT

alignas(32) static u8 sZero[32 * 1024];

static bool zeroRange(FILE* f, u64 offset, u64 length)
{
    if (fseeko(f, (off_t)offset, SEEK_SET) != 0)
        return false;
    while (length > 0)
    {
        u32 n = length > sizeof(sZero) ? sizeof(sZero) : (u32)length;
        if (fwrite(sZero, 1, n, f) != n)
            return false;
        length -= n;
    }
    return true;
}

AllocError quickAlloc_create(const char* dirPath, const char* path, u64 bytes,
    AllocResult& result, AllocProgressFn progress)
{
    // Zeroing the edges writes through libfat, which truncates in-place writes
    // to files near 4 GiB; the UI caps images at 4095 MiB.
    if (bytes == 0 || bytes + sizeof(sZero) > 0xFFFFFFFFull)
        return AllocError::Unsupported;

    // Placeholder: a 1-byte file, so libfat allocates exactly one cluster
    // and writes a normal directory entry (long file name included).
    FILE* f = fopen(path, "wb");
    if (!f)
        return AllocError::CreateFailed;
    bool wrote = fputc(0, f) == 0;
    if (fclose(f) != 0 || !wrote)
    {
        unlink(path);
        sd_remount();
        return AllocError::CreateFailed;
    }

    struct stat fileStat, dirStat;
    if (stat(path, &fileStat) != 0 || stat(dirPath, &dirStat) != 0 || fileStat.st_size != 1)
    {
        unlink(path);
        sd_remount();
        return AllocError::CreateFailed;
    }

    // libfat must not hold any cached FAT/directory sectors while we edit
    // them directly: unmounting flushes and drops everything. This is a
    // hard interlock - if libfat is somehow still mounted, nothing is
    // written behind its back.
    if (!sd_unmount())
    {
        sd_mount();
        unlink(path);
        sd_remount();
        return AllocError::Unsupported;
    }
    AllocError err = quickAlloc_allocateRaw((u32)dirStat.st_ino, (u32)fileStat.st_ino, bytes, result, progress);
    if (!sd_mount())
        return AllocError::IoError; // can't remount: leave the card alone

    if (err == AllocError::None)
    {
        // libfat must now see exactly the file we built
        if (stat(path, &fileStat) != 0 || (u64)(u32)fileStat.st_size != bytes ||
            (u32)fileStat.st_ino != result.firstCluster)
        {
            err = AllocError::VerifyFailed;
        }
    }

    if (err == AllocError::None)
    {
        // Zero the first and last megabyte so hosts see a blank drive (no
        // stale partition table or boot sector from deleted files).
        if (progress)
            progress("Clearing start and end", 0, 1);
        const u64 edge = 1024 * 1024;
        FILE* img = fopen(path, "r+b");
        bool zeroed = img != nullptr;
        if (img)
        {
            setvbuf(img, NULL, _IONBF, 0);
            if (bytes <= edge * 2)
                zeroed = zeroRange(img, 0, bytes);
            else
                zeroed = zeroRange(img, 0, edge) && zeroRange(img, bytes - edge, edge);
            if (fclose(img) != 0)
                zeroed = false;
        }
        if (!zeroed)
            err = AllocError::IoError;
    }

    if (err != AllocError::None)
        unlink(path);
    sd_remount();
    return err;
}

#endif

#endif
