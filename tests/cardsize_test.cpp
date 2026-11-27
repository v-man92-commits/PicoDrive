// Host test for fatVolume_cardSectors (size of the whole SD card for sharing)
#include <stdio.h>
#include <string.h>
#include "common.h"
#include "FatVolume.h"

static u8 gSector0[512];
static bool gReadOk = true;
extern "C" bool dldi_readSectors(void* buffer, u32 sector, u32 count)
{
    if (!gReadOk || sector != 0 || count != 1) return false;
    memcpy(buffer, gSector0, 512);
    return true;
}
extern "C" bool dldi_writeSectors(const void*, u32, u32) { return false; }

sec_t sd_findPartitionStart(u8*) { return 0; }
static int gPasses, gFails;
#define CHECK(c, ...) do { if (c) gPasses++; else { gFails++; printf("  FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void w16(u8* p, u32 v) { p[0] = v; p[1] = v >> 8; }
static void w32(u8* p, u32 v) { w16(p, v); w16(p + 2, v >> 16); }
static void part(int i, u8 type, u32 start, u32 count)
{
    u8* e = gSector0 + 0x1BE + i * 16; e[4] = type; w32(e + 8, start); w32(e + 12, count);
}

int main()
{
    u8 buf[512];
    // typical SD card: one FAT32 partition at 8192
    memset(gSector0, 0, 512); gSector0[0x1FE] = 0x55; gSector0[0x1FF] = 0xAA;
    part(0, 0x0C, 8192, 62521344 - 8192);
    CHECK(fatVolume_cardSectors(buf) == 62521344, "single MBR partition");
    // two partitions, second ends last; empty slots ignored
    part(1, 0x07, 70000000, 1000000);
    CHECK(fatVolume_cardSectors(buf) == 71000000, "max partition end");
    // overflowing entry is skipped
    part(2, 0x0C, 0xFFFFFF00, 0x200);
    CHECK(fatVolume_cardSectors(buf) == 71000000, "overflowing entry ignored");
    // superfloppy FAT32 VBR (no MBR): total from BPB
    memset(gSector0, 0, 512); gSector0[0] = 0xEB; gSector0[0x1FE] = 0x55; gSector0[0x1FF] = 0xAA;
    memcpy(gSector0 + 0x52, "FAT32   ", 8); w32(gSector0 + 0x20, 15523840);
    CHECK(fatVolume_cardSectors(buf) == 15523840, "superfloppy FAT32");
    // FAT16 VBR with 16-bit total
    memset(gSector0, 0, 512); gSector0[0] = 0xE9; gSector0[0x1FE] = 0x55; gSector0[0x1FF] = 0xAA;
    memcpy(gSector0 + 0x36, "FAT16   ", 8); w16(gSector0 + 0x13, 60000);
    CHECK(fatVolume_cardSectors(buf) == 60000, "FAT16 16-bit total");
    // superfloppy exFAT: VolumeLength (64-bit) at 0x48
    memset(gSector0, 0, 512); gSector0[0] = 0xEB; gSector0[0x1FE] = 0x55; gSector0[0x1FF] = 0xAA;
    memcpy(gSector0 + 3, "EXFAT   ", 8); w32(gSector0 + 0x48, 124735488);
    CHECK(fatVolume_cardSectors(buf) == 124735488, "superfloppy exFAT");
    // exFAT inside an MBR partition: the partition table decides
    memset(gSector0, 0, 512); gSector0[0x1FE] = 0x55; gSector0[0x1FF] = 0xAA;
    part(0, 0x07, 32768, 124702720);
    CHECK(fatVolume_cardSectors(buf) == 124735488, "MBR with exFAT partition");
    // no signature / read error / empty table -> 0 (refuse to share)
    gSector0[0x1FE] = 0;
    CHECK(fatVolume_cardSectors(buf) == 0, "no boot signature");
    memset(gSector0, 0, 512); gSector0[0x1FE] = 0x55; gSector0[0x1FF] = 0xAA;
    CHECK(fatVolume_cardSectors(buf) == 0, "empty partition table");
    part(0, 0x0C, 8192, 1000); gReadOk = false;
    CHECK(fatVolume_cardSectors(buf) == 0, "read error");
    printf("cardsize: %d checks passed, %d failed\n", gPasses, gFails);
    return gFails ? 1 : 0;
}
