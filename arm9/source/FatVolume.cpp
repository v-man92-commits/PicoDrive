#include "common.h"
#include <string.h>
#include "dldiIpc.h"
#include "sdDisc.h"
#include "FatVolume.h"

alignas(32) static u8 sSector[512];

static inline u16 rd16(const u8* p) { return p[0] | (p[1] << 8); }
static inline u32 rd32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }

// Same geometry calculation as libfat's _FAT_partition_constructor_buf
FatVolumeResult fatVolume_load(FatVolume& vol)
{
    memset(&vol, 0, sizeof(vol));
    sec_t start = sd_findPartitionStart(sSector);
    if (!dldi_readSectors(sSector, start, 1))
        return FatVolumeResult::IoError;
    if (sSector[0x1FE] != 0x55 || sSector[0x1FF] != 0xAA)
        return FatVolumeResult::Unsupported;
    if (memcmp(sSector + 0x36, "FAT", 3) && memcmp(sSector + 0x52, "FAT", 3))
        return FatVolumeResult::Unsupported;

    u32 bytesPerSector = rd16(sSector + 0x0B);
    if (bytesPerSector != 512)
        return FatVolumeResult::Unsupported;

    vol.partitionStart = start;
    vol.sectorsPerFat = rd16(sSector + 0x16);
    if (vol.sectorsPerFat == 0)
        vol.sectorsPerFat = rd32(sSector + 0x24);
    u32 numSectors = rd16(sSector + 0x13);
    if (numSectors == 0)
        numSectors = rd32(sSector + 0x20);

    vol.sectorsPerCluster = sSector[0x0D];
    vol.numFats = sSector[0x10];
    if (vol.sectorsPerCluster == 0 || vol.numFats == 0 || vol.sectorsPerFat == 0)
        return FatVolumeResult::Unsupported;

    vol.fatStart = start + rd16(sSector + 0x0E);
    u32 rootDirStart = vol.fatStart + vol.numFats * vol.sectorsPerFat;
    vol.dataStart = rootDirStart + (rd16(sSector + 0x11) * 32) / bytesPerSector;
    if (numSectors <= vol.dataStart - start)
        return FatVolumeResult::Unsupported;

    u32 clusterCount = (numSectors - (vol.dataStart - start)) / vol.sectorsPerCluster;
    vol.lastCluster = clusterCount + 1;

    if (clusterCount < 4085)
        return FatVolumeResult::Unsupported; // FAT12
    vol.fat32 = clusterCount >= 65525;
    vol.mirrored = true;

    if (vol.fat32)
    {
        u16 fsInfo = rd16(sSector + 0x30);
        vol.fsInfoSector = start + (fsInfo ? fsInfo : 1);
        u8 extFlags = sSector[0x28];
        if (!(extFlags & 0x80))
        {
            // Bit 7 clear = FATs mirrored (the normal case). libfat reads the
            // copy numbered in bits 0-3 (0 on every normal card); we match it.
            vol.activeFat = extFlags & 0x0F;
            if (vol.activeFat >= vol.numFats)
                return FatVolumeResult::Unsupported;
            vol.fatStart += vol.sectorsPerFat * vol.activeFat;
        }
        else
        {
            // Mirroring disabled: libfat still uses FAT 0 while the spec says
            // bits 0-3 pick the live copy. Reading follows libfat; writing is
            // refused (see QuickAlloc) because the two can disagree.
            vol.mirrored = false;
        }
    }

    // The FAT must be big enough to describe every cluster
    u32 entryBytes = vol.fat32 ? 4 : 2;
    if ((u64)(vol.lastCluster + 1) * entryBytes > (u64)vol.sectorsPerFat * 512)
        return FatVolumeResult::Unsupported;

    return FatVolumeResult::Ok;
}

void fatVolume_entryLocation(const FatVolume& vol, u32 cluster, u32& sector, u32& offset)
{
    u32 byteOffset = vol.fat32 ? cluster * 4 : cluster * 2;
    sector = vol.fatStart + byteOffset / 512;
    offset = byteOffset % 512;
}

u32 fatVolume_cardSectors(u8* buf)
{
    if (!dldi_readSectors(buf, 0, 1))
        return 0;
    if (buf[0x1FE] != 0x55 || buf[0x1FF] != 0xAA)
        return 0;
    bool vbr = (buf[0] == 0xEB || buf[0] == 0xE9) &&
        (!memcmp(buf + 0x36, "FAT", 3) || !memcmp(buf + 0x52, "FAT", 3));
    if (vbr)
    {
        u32 total = rd16(buf + 0x13);
        return total ? total : rd32(buf + 0x20);
    }
    if (!memcmp(buf + 3, "EXFAT   ", 8))
    {
        // exFAT without a partition table: PartitionOffset + VolumeLength
        u64 offset = (u64)rd32(buf + 0x44) << 32 | rd32(buf + 0x40);
        u64 length = (u64)rd32(buf + 0x4C) << 32 | rd32(buf + 0x48);
        u64 end = offset + length;
        return end > 0xFFFFFFFFull ? 0xFFFFFFFFu : (u32)end;
    }
    u32 end = 0;
    for (u32 i = 0; i < 4; i++)
    {
        const u8* e = buf + 0x1BE + i * 16;
        if (e[4] == 0)
            continue;
        u32 start = rd32(e + 8);
        u32 count = rd32(e + 12);
        if (start + count < start)
            continue; // corrupt entry
        if (start + count > end)
            end = start + count;
    }
    return end;
}
