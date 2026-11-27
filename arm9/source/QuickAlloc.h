#pragma once
#include <nds/ndstypes.h>

/// "Quick create": reserves space for a new image by writing its cluster
/// chain straight into the FAT, instead of filling the file with zeros.
///
/// The image's space is a fresh chain of free clusters, contiguous whenever
/// the card has a free run large enough. Only the first and last megabyte
/// are zeroed (so hosts see a blank drive); the rest keeps whatever data the
/// clusters held before.
///
/// Every write is ordered so that an interruption at any point (power off,
/// battery dies) leaves, at worst, "lost clusters" that a disk check
/// reclaims. It never leaves two files sharing clusters.

enum class AllocError
{
    None,
    IoError,
    Unsupported,       // not a FAT16/FAT32 volume libfat understands
    NoSpace,
    TooFragmented,     // free space is split into too many pieces
    CreateFailed,      // libfat couldn't create the placeholder file
    EntryNotFound,     // couldn't find the new file's directory entry
    VerifyFailed       // libfat doesn't see the expected file afterwards
};

const char* allocErrorString(AllocError error);

typedef void (*AllocProgressFn)(const char* stage, u32 done, u32 total);

struct AllocResult
{
    u32 firstCluster;
    u32 clusters;
    u32 runs;          // 1 = contiguous
};

/// Raw part (libfat must NOT be mounted while this runs).
/// dirCluster: first cluster of the directory holding the placeholder file.
/// placeholderCluster: the 1-byte placeholder file's only cluster.
/// On success the placeholder's directory entry describes the new chain of
/// `bytes` bytes and the placeholder cluster is freed.
AllocError quickAlloc_allocateRaw(u32 dirCluster, u32 placeholderCluster, u64 bytes,
    AllocResult& result, AllocProgressFn progress);

/// Full operation through libfat: creates `path` inside `dirPath`,
/// reserves `bytes` bytes for it and zeroes the head and tail.
/// libfat is remounted afterwards. On failure the file is removed.
AllocError quickAlloc_create(const char* dirPath, const char* path, u64 bytes,
    AllocResult& result, AllocProgressFn progress);
