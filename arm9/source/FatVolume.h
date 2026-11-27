#pragma once
#include <nds/ndstypes.h>

/// Geometry of the SD card's FAT volume, computed with the same rules as
/// libfat (so it always describes the volume libfat mounted).
struct FatVolume
{
    bool fat32;
    u32 partitionStart;
    u32 sectorsPerCluster;
    u32 sectorsPerFat;
    u32 numFats;
    u32 fatStart;          // the FAT copy libfat reads
    u32 activeFat;         // index of the copy at fatStart
    bool mirrored;         // FATs are mirrored (normal); false = unusual single-FAT mode
    u32 dataStart;
    u32 lastCluster;       // highest valid cluster number
    u32 fsInfoSector;      // FAT32 only, 0 if none
};

enum class FatVolumeResult
{
    Ok,
    IoError,
    Unsupported         // FAT12, non-512 byte sectors, not FAT...
};

FatVolumeResult fatVolume_load(FatVolume& vol);

/// Sector of the first FAT copy that holds the entry for a cluster,
/// and the byte offset of the entry inside that sector.
void fatVolume_entryLocation(const FatVolume& vol, u32 cluster, u32& sector, u32& offset);

inline u32 fatVolume_clusterToSector(const FatVolume& vol, u32 cluster)
{
    return vol.dataStart + (cluster - 2) * vol.sectorsPerCluster;
}

inline u32 fatVolume_endOfChain(const FatVolume& vol)
{
    return vol.fat32 ? 0x0FFFFFFF : 0xFFFF;
}

/// Reads one FAT entry from a buffer holding the entry's sector.
inline u32 fatVolume_readEntry(const FatVolume& vol, const u8* p)
{
    if (vol.fat32)
        return (p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24)) & 0x0FFFFFFF;
    return p[0] | (p[1] << 8);
}

/// Writes one FAT entry (keeps the reserved top 4 bits on FAT32).
inline void fatVolume_writeEntry(const FatVolume& vol, u8* p, u32 value)
{
    if (vol.fat32)
    {
        u32 v = (value & 0x0FFFFFFF) | ((u32)(p[3] & 0xF0) << 24);
        p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
    }
    else
    {
        p[0] = value; p[1] = value >> 8;
    }
}

/// Size of the whole SD card as best we can tell (the DSpico doesn't report
/// its capacity): the end of the last MBR partition, or the sector count of a
/// partitionless ("superfloppy") FAT volume. 0 if unknown.
/// sectorBuffer must be 512 bytes, 32-byte aligned.
u32 fatVolume_cardSectors(u8* sectorBuffer);
