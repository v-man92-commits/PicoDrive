// exFAT build: "quick create" with FatFs. Space is reserved without writing
// the image's data:
//  - f_expand() first: one contiguous run (on exFAT a "no FAT chain" file,
//    on FAT a single cluster chain);
//  - if no free run is big enough, seeking past the end allocates the space
//    cluster by cluster wherever it is free.
// Then the first and last megabyte are zeroed so hosts see a blank drive.
// FatFs writes the allocation before the directory entry, so a power cut
// leaves at worst lost clusters for a disk check to reclaim.
#include "common.h"
#include <string.h>
#include "ff.h"
#include "fsSyscalls.h"
#include "FatMap.h"
#include "QuickAlloc.h"

namespace
{
    constexpr u32 ZERO_CHUNK = 32 * 1024;
    constexpr u64 ZERO_BYTES = 1024 * 1024;
    alignas(32) u8 sZero[ZERO_CHUNK];

    bool zeroRange(FIL& f, u64 offset, u64 length, AllocProgressFn progress, u32& done, u32 total)
    {
        if (f_lseek(&f, (FSIZE_t)offset) != FR_OK)
            return false;
        while (length)
        {
            UINT n = length > ZERO_CHUNK ? ZERO_CHUNK : (UINT)length;
            UINT bw;
            if (f_write(&f, sZero, n, &bw) != FR_OK || bw != n)
                return false;
            length -= n;
            done++;
            if (progress && (done & 7) == 0)
                progress("Zeroing start and end", done, total);
        }
        return true;
    }
}

const char* allocErrorString(AllocError error)
{
    switch (error)
    {
        case AllocError::None:          return "OK";
        case AllocError::IoError:       return "SD card I/O error";
        case AllocError::Unsupported:   return "File too big for this SD";
        case AllocError::NoSpace:       return "Not enough free space";
        case AllocError::TooFragmented: return "Free space too fragmented";
        case AllocError::CreateFailed:  return "Couldn't create the file";
        case AllocError::EntryNotFound: return "Directory entry not found";
        case AllocError::VerifyFailed:  return "Check after create failed";
    }
    return "?";
}

AllocError quickAlloc_create(const char* dirPath, const char* path, u64 bytes,
    AllocResult& result, AllocProgressFn progress)
{
    (void)dirPath;
    memset(&result, 0, sizeof(result));
    const char* p = fs_ffPath(path);

    FIL f;
    FRESULT res = f_open(&f, p, FA_CREATE_NEW | FA_WRITE);
    if (res != FR_OK)
        return res == FR_DENIED || res == FR_EXIST || res == FR_INVALID_NAME ? AllocError::CreateFailed : AllocError::IoError;

    const FATFS* fs = f.obj.fs;
    if (fs->fs_type != FS_EXFAT && bytes > 0xFFFFFFFFull)
    {
        f_close(&f);
        f_unlink(p);
        return AllocError::Unsupported;
    }

    if (progress)
        progress("Reserving space", 0, 1);
    res = f_expand(&f, (FSIZE_t)bytes, 1);
    if (res == FR_DENIED)
    {
        // No single free run that big: allocate in pieces.
        if (progress)
            progress("Reserving space (in pieces)", 0, 1);
        res = f_lseek(&f, (FSIZE_t)bytes);
        if (res == FR_OK && f_tell(&f) != (FSIZE_t)bytes)
            res = FR_DENIED; // the volume filled up
    }

    AllocError err = AllocError::None;
    if (res == FR_DENIED)
        err = AllocError::NoSpace;
    else if (res != FR_OK)
        err = AllocError::IoError;

    if (err == AllocError::None)
    {
        u64 head = bytes < ZERO_BYTES ? bytes : ZERO_BYTES;
        u64 tailStart = bytes > ZERO_BYTES ? bytes - ZERO_BYTES : 0;
        if (tailStart < head)
            tailStart = head;
        u32 total = (u32)((head + (bytes - tailStart) + ZERO_CHUNK - 1) / ZERO_CHUNK);
        u32 done = 0;
        if (!zeroRange(f, 0, head, progress, done, total) ||
            !zeroRange(f, tailStart, bytes - tailStart, progress, done, total))
            err = AllocError::IoError;
    }

    result.firstCluster = f.obj.sclust;
    u32 clusterBytes = fs->csize * 512;
    result.clusters = (u32)((bytes + clusterBytes - 1) / clusterBytes);

    if (f_close(&f) != FR_OK && err == AllocError::None)
        err = AllocError::IoError;

    if (err == AllocError::None)
    {
        FILINFO fno;
        if (f_stat(p, &fno) != FR_OK || fno.fsize != (FSIZE_t)bytes)
            err = AllocError::VerifyFailed;
    }
    if (err == AllocError::None)
    {
        ImageMap map = {};
        MapError me = fatMap_build(path, map); // count-only: how many pieces
        result.runs = map.extentCount;
        if (me == MapError::TooFragmented)
            err = AllocError::TooFragmented; // couldn't be mounted anyway
        else if (me != MapError::None)
            err = AllocError::VerifyFailed;
    }

    if (err != AllocError::None)
        f_unlink(p);
    return err;
}
