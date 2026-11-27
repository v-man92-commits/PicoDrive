#pragma once
#include "mountIpc.h"

/// Resolves an image file on the SD card into absolute SD sector runs,
/// by walking its FAT cluster chain (FAT16/FAT32, same geometry rules as
/// libfat). The result is verified against data read back through libfat
/// before it is ever used for writing.

enum class MapError
{
    None,
    OpenFailed,
    EmptyFile,
    UnsupportedVolume,  // FAT12, exFAT, non-512 byte sectors...
    BrokenChain,
    TooFragmented,
    IoError,
    VerifyFailed,
    TooLarge            // over 2^32 sectors (2 TiB)
};

const char* mapErrorString(MapError error);

struct ImageMap
{
    mount_extent_t* extents;  // caller-provided storage
    u32 maxExtents;
    u32 extentCount;
    u32 totalSectors;         // image size in 512-byte sectors (rounded down)
    u64 fileSize;             // bytes
    bool roundUp;             // map a partial last sector too (BIN/CUE .bin files)
};

/// Builds the map. If extents is null (or maxExtents is 0) only the
/// fragment count is computed (useful for the info panel).
MapError fatMap_build(const char* path, ImageMap& map);

/// Reads the first and last sector of every run both through libfat and
/// through the map and checks they match. Blank (uniform) sectors are
/// proven with a temporary test pattern written through libfat (and then
/// restored) when writable is true. Must succeed before mounting.
MapError fatMap_verify(const char* path, const ImageMap& map, bool writable,
    void (*progress)(u32 done, u32 total) = nullptr);
