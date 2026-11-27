// Host test for the exFAT build's filesystem code: real FatFs (with exFAT)
// on a disk image file, driving the image mounter's own sdDiscFatFs.cpp,
// FatMapFatFs.cpp, QuickAllocFatFs.cpp and FatMap.cpp (verify).
// Usage: exfat_test exfat|fat32 disk.img sizeMiB clusterBytes report.txt
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <random>
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include "common.h"
#include "mountIpc.h"
#include "ff.h"
#include "diskio.h"
#include "FatMap.h"
#include "QuickAlloc.h"
#include "sdDisc.h"
#include "mountClient.h"
#undef stat
#undef fopen
#undef fsync

static int gDisk = -1;
static int gPasses, gFails;
#define CHECK(c, ...) do { if (c) gPasses++; else { gFails++; printf("  FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- FatFs disk I/O on the image file ------------------------------------------
static u64 gDiskSectors;
extern "C" {
DSTATUS disk_initialize(BYTE) { return 0; }
DSTATUS disk_status(BYTE) { return 0; }
DRESULT disk_read(BYTE, BYTE* b, LBA_t s, UINT n) { return pread(gDisk, b, n * 512, (off_t)s * 512) == (ssize_t)(n * 512) ? RES_OK : RES_ERROR; }
DRESULT disk_write(BYTE, const BYTE* b, LBA_t s, UINT n) { return pwrite(gDisk, b, n * 512, (off_t)s * 512) == (ssize_t)(n * 512) ? RES_OK : RES_ERROR; }
DRESULT disk_ioctl(BYTE, BYTE cmd, void* buf)
{
    switch (cmd) {
        case CTRL_SYNC: return RES_OK;
        case GET_SECTOR_COUNT: *(LBA_t*)buf = (LBA_t)gDiskSectors; return RES_OK;
        case GET_BLOCK_SIZE: *(DWORD*)buf = 8192; return RES_OK; // 4 MiB alignment like SD cards
    }
    return RES_PARERR;
}
DWORD get_fattime(void) { return ((DWORD)(2026 - 1980) << 25) | (11 << 21) | (26 << 16); }

// raw SD access used by FatMap.cpp's verify (readViaMap)
bool dldi_readSectors(void* b, u32 s, u32 n) { return pread(gDisk, b, n * 512, (off_t)s * 512) == (ssize_t)(n * 512); }
bool dldi_writeSectors(const void* b, u32 s, u32 n) { return pwrite(gDisk, b, n * 512, (off_t)s * 512) == (ssize_t)(n * 512); }

// path helper from fsSyscalls.cpp (the full syscall layer is tested in fs_syscalls_test)
const char* fs_ffPath(const char* path) { const char* c = strchr(path, ':'); return c ? c + 1 : path; }
}
static mount_status_t gStatus;
const mount_status_t& mountClient_status() { return gStatus; }

// ---- stdio for FatMap.cpp's verify: FILE* backed by FatFs via fopencookie --------
struct Cookie { FIL fil; };
static std::vector<Cookie*> gCookies;
static ssize_t ckRead(void* c, char* b, size_t n) { UINT br; return f_read(&((Cookie*)c)->fil, b, n, &br) == FR_OK ? (ssize_t)br : -1; }
static ssize_t ckWrite(void* c, const char* b, size_t n) { UINT bw; return f_write(&((Cookie*)c)->fil, b, n, &bw) == FR_OK ? (ssize_t)bw : -1; }
static int ckSeek(void* c, off64_t* pos, int whence)
{
    FIL* f = &((Cookie*)c)->fil;
    s64 base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (s64)f_tell(f) : (s64)f_size(f);
    if (f_lseek(f, (FSIZE_t)(base + *pos)) != FR_OK) return -1;
    *pos = (off64_t)f_tell(f);
    return 0;
}
static int ckClose(void* c) { FRESULT r = f_close(&((Cookie*)c)->fil); for (auto& p : gCookies) if (p == c) p = nullptr; delete (Cookie*)c; return r == FR_OK ? 0 : -1; }
extern "C" FILE* test_fopen(const char* path, const char* mode)
{
    Cookie* c = new Cookie;
    BYTE fm = strchr(mode, '+') ? (FA_READ | FA_WRITE) : (mode[0] == 'r' ? FA_READ : FA_WRITE | FA_CREATE_ALWAYS);
    if (f_open(&c->fil, fs_ffPath(path), fm) != FR_OK) { delete c; return nullptr; }
    cookie_io_functions_t io = { ckRead, ckWrite, ckSeek, ckClose };
    FILE* f = fopencookie(c, mode, io);
    gCookies.push_back(c);
    return f;
}
// FatMap.cpp calls fsync(fileno(f)); cookie streams have no fd, so sync every open file
extern "C" int test_fsync(int) { for (auto c : gCookies) if (c && f_sync(&c->fil) != FR_OK) return -1; return 0; }
extern "C" int test_stat(const char*, struct stat*) { return -1; }

// ---- helpers ----------------------------------------------------------------------
static std::mt19937 gRng(1234);
static FILE* gReport;
alignas(32) static mount_extent_t gExt[MOUNT_MAX_EXTENTS];

static bool writeRandom(const char* path, u64 bytes)
{
    FIL f;
    if (f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return false;
    std::vector<u8> buf(64 * 1024);
    while (bytes) {
        for (auto& b : buf) b = gRng();
        UINT n = bytes > buf.size() ? buf.size() : (UINT)bytes, bw;
        if (f_write(&f, buf.data(), n, &bw) != FR_OK || bw != n) { f_close(&f); return false; }
        bytes -= n;
    }
    return f_close(&f) == FR_OK;
}

// Overwrites `sectors` sectors at file sector `at` with random data through FatFs
static bool scribble(const char* path, u32 at, u32 sectors)
{
    FIL f;
    if (f_open(&f, path, FA_READ | FA_WRITE) != FR_OK) return false;
    std::vector<u8> buf(sectors * 512);
    for (auto& b : buf) b = gRng();
    UINT bw;
    bool ok = f_lseek(&f, (FSIZE_t)at * 512) == FR_OK && f_write(&f, buf.data(), buf.size(), &bw) == FR_OK && bw == buf.size();
    return f_close(&f) == FR_OK && ok;
}

static bool mapRead(const ImageMap& m, u32 fileSector, u8* out)
{
    for (u32 i = 0; i < m.extentCount; i++)
        if (fileSector >= m.extents[i].fileSector && fileSector < m.extents[i].fileSector + m.extents[i].count)
            return dldi_readSectors(out, m.extents[i].sdSector + fileSector - m.extents[i].fileSector, 1);
    return false;
}

// Compares sectors read through FatFs with sectors read through the map:
// all of them for small files, a sample for big ones.
static u32 compareFile(const char* path, const ImageMap& m)
{
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return 1;
    u32 bad = 0;
    u32 step = m.totalSectors > 65536 ? m.totalSectors / 4096 : 1;
    u8 a[512], b[512];
    for (u32 s = 0; s < m.totalSectors; s += step) {
        UINT br;
        u32 sec = s;
        if (step > 1 && s + step >= m.totalSectors) sec = m.totalSectors - 1;
        if (f_lseek(&f, (FSIZE_t)sec * 512) != FR_OK || f_read(&f, a, 512, &br) != FR_OK || br != 512 || !mapRead(m, sec, b) || memcmp(a, b, 512)) bad++;
    }
    f_close(&f);
    return bad;
}

static bool rangeIsZero(const ImageMap& m, u32 first, u32 count)
{
    u8 b[512];
    for (u32 s = first; s < first + count && s < m.totalSectors; s++) {
        if (!mapRead(m, s, b)) return false;
        for (int i = 0; i < 512; i++) if (b[i]) return false;
    }
    return true;
}

static void report(const char* name, const ImageMap& m)
{
    fprintf(gReport, "%s %llu %u", name, (unsigned long long)m.fileSize, m.extentCount);
    for (u32 i = 0; i < m.extentCount; i++)
        fprintf(gReport, " %u:%u:%u", m.extents[i].fileSector, m.extents[i].sdSector, m.extents[i].count);
    fprintf(gReport, "\n");
}

static ImageMap mapOf(const char* path, MapError* err)
{
    ImageMap m = {};
    m.extents = gExt;
    m.maxExtents = MOUNT_MAX_EXTENTS;
    *err = fatMap_build(path, m);
    return m;
}

static void progress(const char*, u32, u32) {}

int main(int argc, char** argv)
{
    if (argc < 6) { printf("usage\n"); return 2; }
    bool exfat = !strcmp(argv[1], "exfat");
    u64 sizeMiB = strtoull(argv[3], 0, 0);
    u32 clusterBytes = strtoul(argv[4], 0, 0);
    gReport = fopen(argv[5], "w");
    gDisk = open(argv[2], O_RDWR | O_CREAT | O_TRUNC, 0644);
    ftruncate(gDisk, (off_t)(sizeMiB << 20));
    gDiskSectors = (sizeMiB << 20) / 512;

    // format like an SD card (MBR, aligned partition)
    {
        static BYTE work[64 * 1024];
        MKFS_PARM opt = { (BYTE)(exfat ? FM_EXFAT : FM_FAT32), 1, 0, 0, clusterBytes };
        FRESULT r = f_mkfs("", &opt, work, sizeof(work));
        CHECK(r == FR_OK, "f_mkfs %d", r);
        if (r != FR_OK) return 1;
    }

    CHECK(sd_mount(), "sd_mount");
    CHECK(sd_isMounted(), "mounted");
    const char* type = sd_fsTypeName();
    CHECK(type && !strcmp(type, exfat ? "exFAT" : "FAT32"), "fs type %s", type ? type : "null");
    CHECK(sd_maxImageBytes() == (exfat ? 0xFFFFFFFFull * 512 : 4095ull << 20), "max image bytes");
    CHECK(f_mkdir("/images") == FR_OK, "mkdir");
    AllocResult res;
    MapError me;

    // ---- big contiguous image (exFAT only: over 4 GiB) --------------------------------
    if (exfat && sizeMiB > 6000)
    {
        u64 big = 5ull << 30;
        CHECK(quickAlloc_create("sd:/images", "sd:/images/BIG.IMG", big, res, progress) == AllocError::None, "quick create 5 GiB");
        CHECK(res.runs == 1, "5 GiB image contiguous (%u runs)", res.runs);
        ImageMap m = mapOf("sd:/images/BIG.IMG", &me);
        CHECK(me == MapError::None && m.extentCount == 1 && m.totalSectors == big / 512 && m.fileSize == big,
            "5 GiB map (err %d, %u extents, %u sectors)", (int)me, m.extentCount, m.totalSectors);
        CHECK(rangeIsZero(m, 0, 2048) && rangeIsZero(m, m.totalSectors - 2048, 2048), "5 GiB head/tail zeroed");
        CHECK(scribble("/images/BIG.IMG", 0, 64) && scribble("/images/BIG.IMG", m.totalSectors - 64, 64) &&
            scribble("/images/BIG.IMG", m.totalSectors / 2, 64), "write into 5 GiB image");
        CHECK(compareFile("/images/BIG.IMG", m) == 0, "5 GiB image: FatFs vs map");
        CHECK(fatMap_verify("sd:/images/BIG.IMG", m, true) == MapError::None, "verify 5 GiB (writable)");
        report("BIG.IMG", m);
    }

    // ---- small quick-created image ----------------------------------------------------
    {
        CHECK(quickAlloc_create("sd:/images", "sd:/images/A.IMG", 8 << 20, res, progress) == AllocError::None, "quick create 8 MiB");
        CHECK(res.runs == 1, "8 MiB contiguous");
        ImageMap m = mapOf("sd:/images/A.IMG", &me);
        CHECK(me == MapError::None && m.extentCount == 1 && m.totalSectors == 16384, "8 MiB map");
        CHECK(rangeIsZero(m, 0, m.totalSectors), "8 MiB image all zero (head+tail cover it)");
        CHECK(writeRandom("/images/A.IMG", 8 << 20), "fill A");
        m = mapOf("sd:/images/A.IMG", &me);
        CHECK(me == MapError::None && compareFile("/images/A.IMG", m) == 0, "A: every sector matches");
        CHECK(fatMap_verify("sd:/images/A.IMG", m, true) == MapError::None, "verify A writable");
        CHECK(fatMap_verify("sd:/images/A.IMG", m, false) == MapError::None, "verify A read-only");
        ImageMap wrong = m;
        static mount_extent_t wext[1];
        wext[0] = m.extents[0];
        wext[0].sdSector += clusterBytes / 512;
        wrong.extents = wext;
        CHECK(fatMap_verify("sd:/images/A.IMG", wrong, false) == MapError::VerifyFailed, "wrong map rejected");
        CHECK(compareFile("/images/A.IMG", m) == 0, "A unchanged by verification");
        report("A.IMG", m);
    }

    // ---- BIN/CUE .bin: size not a multiple of 512, mapped rounded up ---------------------
    {
        u64 binSize = 777ull * 2352; // 1828904 bytes: 296 bytes into its last sector
        CHECK(writeRandom("/images/TRACK.BIN", binSize), "write TRACK.BIN");
        ImageMap m = {};
        m.extents = gExt; m.maxExtents = MOUNT_MAX_EXTENTS; m.roundUp = true;
        CHECK(fatMap_build("sd:/images/TRACK.BIN", m) == MapError::None && m.totalSectors == (binSize + 511) / 512, "round-up map: %u sectors", m.totalSectors);
        // the bytes of the partial last sector come out of the mapped SD sector
        FIL f; u8 want[512], got[512]; UINT br;
        f_open(&f, "/images/TRACK.BIN", FA_READ);
        f_lseek(&f, (FSIZE_t)(m.totalSectors - 1) * 512);
        f_read(&f, want, 512, &br);
        f_close(&f);
        CHECK(br == binSize % 512 && mapRead(m, m.totalSectors - 1, got) && !memcmp(want, got, br), "partial last sector maps right (%u bytes)", br);
        ImageMap check = m; check.totalSectors = (u32)(binSize / 512);
        CHECK(fatMap_verify("sd:/images/TRACK.BIN", check, false) == MapError::None, "whole sectors verify");
        f_unlink("/images/TRACK.BIN");
    }

    // ---- errors: too big for FAT32, more than the free space ----------------------------
    if (!exfat)
    {
        CHECK(quickAlloc_create("sd:/images", "sd:/images/HUGE.IMG", 5ull << 30, res, progress) == AllocError::Unsupported, "FAT32: 5 GiB refused");
        FILINFO fno;
        CHECK(f_stat("/images/HUGE.IMG", &fno) == FR_NO_FILE, "refused image left nothing behind");
    }
    {
        DWORD freeCl; FATFS* fs;
        f_getfree("", &freeCl, &fs);
        u64 tooMuch = (u64)freeCl * clusterBytes + (64ull << 20);
        CHECK(quickAlloc_create("sd:/images", "sd:/images/NOSPACE.IMG", tooMuch, res, progress) == AllocError::NoSpace, "bigger than free space refused");
        FILINFO fno;
        CHECK(f_stat("/images/NOSPACE.IMG", &fno) == FR_NO_FILE, "no-space attempt left nothing behind");
        DWORD freeAfter;
        f_getfree("", &freeAfter, &fs);
        CHECK(freeAfter == freeCl, "free space restored (%u vs %u)", freeAfter, freeCl);
    }

    // ---- fragmented free space: quick create has to allocate in pieces ----------------------
    {
        char name[64];
        for (int i = 0; i < 24; i++) {
            snprintf(name, sizeof(name), "/F%02d.BIN", i);
            CHECK(writeRandom(name, 1 << 20), "write %s", name);
        }
        // fill all remaining space except 2 MiB with one contiguous file
        DWORD freeCl; FATFS* fs;
        f_getfree("", &freeCl, &fs);
        FIL fill;
        CHECK(f_open(&fill, "/FILLER.BIN", FA_CREATE_NEW | FA_WRITE) == FR_OK, "filler open");
        FRESULT fr = f_expand(&fill, (FSIZE_t)freeCl * clusterBytes - (2ull << 20), 1);
        CHECK(fr == FR_OK, "filler expand %d", fr);
        f_close(&fill);
        for (int i = 0; i < 24; i += 2) {
            snprintf(name, sizeof(name), "/F%02d.BIN", i);
            CHECK(f_unlink(name) == FR_OK, "unlink %s", name);
        }
        // 12 holes of 1 MiB + 2 MiB at the end: 10 MiB needs at least 5 pieces
        CHECK(quickAlloc_create("sd:/images", "sd:/images/PIECES.IMG", 10 << 20, res, progress) == AllocError::None, "quick create in pieces");
        CHECK(res.runs >= 5, "PIECES in %u runs", res.runs);
        ImageMap m = mapOf("sd:/images/PIECES.IMG", &me);
        CHECK(me == MapError::None && m.extentCount == res.runs && m.totalSectors == 20480, "PIECES map (%u extents)", m.extentCount);
        CHECK(rangeIsZero(m, 0, 2048) && rangeIsZero(m, m.totalSectors - 2048, 2048), "PIECES head/tail zeroed");
        CHECK(writeRandom("/images/PIECES.IMG", 10 << 20), "fill PIECES");
        m = mapOf("sd:/images/PIECES.IMG", &me);
        CHECK(compareFile("/images/PIECES.IMG", m) == 0, "PIECES: every sector matches");
        CHECK(fatMap_verify("sd:/images/PIECES.IMG", m, true) == MapError::None, "verify PIECES");
        report("PIECES.IMG", m);
        // count-only mode (info panel)
        ImageMap c = {};
        CHECK(fatMap_build("sd:/images/PIECES.IMG", c) == MapError::None && c.extentCount == m.extentCount, "count-only mode");
        CHECK(fatMap_build("sd:/images/MISSING.IMG", c) == MapError::OpenFailed, "missing file");
    }

    // ---- remount keeps everything --------------------------------------------------------
    CHECK(sd_unmount() && !sd_isMounted(), "unmount");
    CHECK(sd_remount(), "remount");
    {
        ImageMap m = mapOf("sd:/images/A.IMG", &me);
        CHECK(me == MapError::None && compareFile("/images/A.IMG", m) == 0, "A intact after remount");
    }
    sd_unmount();
    fclose(gReport);
    printf("exfat_test %s: %d checks passed, %d failed\n", argv[1], gPasses, gFails);
    return gFails ? 1 : 0;
}
