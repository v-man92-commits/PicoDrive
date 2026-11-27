// Host test for FatMap.cpp: runs the real mapper against FAT images built by
// mkfat.py, with libfat replaced by a shim that knows each file's true layout.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <string>
#include <map>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "common.h"
#include "mountIpc.h"
#include "FatMap.h"
#include "FatVolume.h"
#include "mountClient.h"
#include "sdDisc.h"
#undef stat
#undef fopen
#undef fsync

struct Run { u32 fileSector, sd, count; };
struct TFile { std::string name; u32 size, start; std::vector<Run> runs; };
static std::map<std::string, TFile> gFiles;
static int gDisk = -1;
static int gFails = 0, gPasses = 0;
static u32 gPartStart;

#define CHECK(cond, ...) do { if (cond) gPasses++; else { gFails++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- shims ---------------------------------------------------------------
extern "C" bool dldi_readSectors(void* buffer, u32 sector, u32 count)
{
    return pread(gDisk, buffer, count * 512, (off_t)sector * 512) == (ssize_t)(count * 512);
}
extern "C" bool dldi_writeSectors(const void* buffer, u32 sector, u32 count)
{
    return pwrite(gDisk, buffer, count * 512, (off_t)sector * 512) == (ssize_t)(count * 512);
}
// libfat's FindFirstValidPartition for a single primary partition, and the
// sector-0 check from sdDisc.cpp
sec_t sd_findPartitionStart(u8* buf)
{
    dldi_readSectors(buf, 0, 1);
    if (!memcmp(buf + 0x36, "FAT", 3) || !memcmp(buf + 0x52, "FAT", 3)) return 0;
    for (int i = 0; i < 4; i++) {
        u32 lba = buf[0x1BE + i*16 + 8] | (buf[0x1BE + i*16 + 9] << 8) | (buf[0x1BE + i*16 + 10] << 16) | ((u32)buf[0x1BE + i*16 + 11] << 24);
        if (buf[0x1BE + i*16 + 4] == 0) continue;
        u8 tmp[512]; dldi_readSectors(tmp, lba, 1);
        if (!memcmp(tmp + 0x36, "FAT", 3) || !memcmp(tmp + 0x52, "FAT", 3)) return lba;
    }
    return 0;
}
static mount_status_t gStatus;
const mount_status_t& mountClient_status() { return gStatus; }

static const char* baseName(const char* path) { const char* s = strrchr(path, '/'); return s ? s + 1 : path; }

extern "C" int test_stat(const char* path, struct stat* st)
{
    auto it = gFiles.find(baseName(path));
    if (it == gFiles.end()) return -1;
    memset(st, 0, sizeof(*st));
    st->st_size = it->second.size;
    st->st_ino = it->second.start;   // libfat behaviour
    return 0;
}
extern "C" int test_fsync(int) { return 0; }

// FILE emulation that reads/writes through the TRUE layout (what libfat does)
struct Cookie { TFile* f; off_t pos; };
static bool trueSector(TFile* f, u32 fs, u32& sd)
{
    for (auto& r : f->runs) if (fs >= r.fileSector && fs < r.fileSector + r.count) { sd = r.sd + fs - r.fileSector; return true; }
    return false;
}
static ssize_t ck_rw(Cookie* c, char* buf, size_t n, bool write)
{
    size_t done = 0;
    while (done < n) {
        if ((u64)c->pos >= c->f->size) break;
        u32 fs = c->pos / 512, off = c->pos % 512, sd;
        size_t len = 512 - off; if (len > n - done) len = n - done;
        if ((u64)c->pos + len > c->f->size) len = c->f->size - c->pos;
        if (!trueSector(c->f, fs, sd)) break;
        ssize_t r = write ? pwrite(gDisk, buf + done, len, (off_t)sd * 512 + off) : pread(gDisk, buf + done, len, (off_t)sd * 512 + off);
        if (r != (ssize_t)len) return -1;
        done += len; c->pos += len;
    }
    return done;
}
static ssize_t ck_read(void* c, char* b, size_t n) { return ck_rw((Cookie*)c, b, n, false); }
static ssize_t ck_write(void* c, const char* b, size_t n) { return ck_rw((Cookie*)c, (char*)b, n, true); }
static int ck_seek(void* c, off64_t* off, int whence)
{
    Cookie* k = (Cookie*)c;
    off64_t np = whence == SEEK_SET ? *off : whence == SEEK_CUR ? k->pos + *off : k->f->size + *off;
    if (np < 0) return -1;
    k->pos = np; *off = np; return 0;
}
static int ck_close(void* c) { delete (Cookie*)c; return 0; }
extern "C" FILE* test_fopen(const char* path, const char* mode)
{
    auto it = gFiles.find(baseName(path));
    if (it == gFiles.end()) return nullptr;
    cookie_io_functions_t io = { ck_read, ck_write, ck_seek, ck_close };
    return fopencookie(new Cookie{ &it->second, 0 }, mode, io);
}

// ---- helpers -------------------------------------------------------------
static void loadTable(const char* txt)
{
    gFiles.clear();
    FILE* f = ::fopen(txt, "r");
    char name[64]; u32 size, start, nruns;
    fscanf(f, "%u", &gPartStart);
    while (fscanf(f, "%63s %u %u %u", name, &size, &start, &nruns) == 4) {
        TFile t{ name, size, start, {} };
        for (u32 i = 0; i < nruns; i++) { Run r; fscanf(f, "%u %u %u", &r.fileSector, &r.sd, &r.count); t.runs.push_back(r); }
        gFiles[name] = t;
    }
    fclose(f);
}
static std::vector<u8> diskSnapshot()
{
    off_t len = lseek(gDisk, 0, SEEK_END);
    std::vector<u8> v(len);
    pread(gDisk, v.data(), len, 0);
    return v;
}
static mount_extent_t gExt[MOUNT_MAX_EXTENTS];

static u32 mapLookup(const ImageMap& m, u32 fs, bool& ok)
{
    for (u32 i = 0; i < m.extentCount; i++) {
        auto& e = m.extents[i];
        if (fs >= e.fileSector && fs < e.fileSector + e.count) { ok = true; return e.sdSector + fs - e.fileSector; }
    }
    ok = false; return 0;
}

static void testReadOnly(const char* name)
{
    printf(" %s (read-only verify)\n", name);
    char path[128]; snprintf(path, sizeof(path), "sd:/images/%s", name);
    ImageMap m = { gExt, MOUNT_MAX_EXTENTS, 0, 0, 0 };
    CHECK(fatMap_build(path, m) == MapError::None, "build");
    auto before = diskSnapshot();
    MapError v = fatMap_verify(path, m, false);
    CHECK(v == MapError::None, "read-only verify of %s failed: %s", name, mapErrorString(v));
    CHECK(diskSnapshot() == before, "read-only verify wrote to the disk");
    // wrong map: every run shifted somewhere else on the card
    mount_extent_t saved[MOUNT_MAX_EXTENTS]; memcpy(saved, gExt, sizeof(gExt));
    for (u32 i = 0; i < m.extentCount; i++) gExt[i].sdSector += 200000;
    CHECK(fatMap_verify(path, m, false) == MapError::VerifyFailed, "read-only verify accepted a wrong map");
    memcpy(gExt, saved, sizeof(gExt));
    // wrong map in the middle only (edges blank): the spread samples must catch it
    if (m.extentCount >= 3) {
        gExt[m.extentCount / 2].sdSector += 200000;
        CHECK(fatMap_verify(path, m, false) == MapError::VerifyFailed, "a wrong middle run was missed");
        memcpy(gExt, saved, sizeof(gExt));
    }
    CHECK(diskSnapshot() == before, "rejected read-only verify wrote to the disk");
}

static void testFile(const char* name, MapError expect, u32 expectExtents)
{
    printf(" %s\n", name);
    char path[128]; snprintf(path, sizeof(path), "sd:/images/%s", name);
    TFile& t = gFiles[name];

    ImageMap m = { gExt, MOUNT_MAX_EXTENTS, 0, 0, 0 };
    MapError e = fatMap_build(path, m);
    CHECK(e == expect, "build: got %s, want %s", mapErrorString(e), mapErrorString(expect));
    if (e != MapError::None) return;

    CHECK(m.totalSectors == t.size / 512, "totalSectors %u want %u", m.totalSectors, t.size / 512);
    if (expectExtents) CHECK(m.extentCount == expectExtents, "extents %u want %u", m.extentCount, expectExtents);
    CHECK(m.extentCount == t.runs.size(), "extents %u, truth has %zu runs", m.extentCount, t.runs.size());

    // every single sector must map to the true location
    u32 bad = 0;
    for (u32 s = 0; s < m.totalSectors; s++) {
        bool ok; u32 sd = mapLookup(m, s, ok); u32 want;
        trueSector(&t, s, want);
        if (!ok || sd != want) bad++;
    }
    CHECK(bad == 0, "%u of %u sectors mis-mapped", bad, m.totalSectors);

    // extents must tile 0..totalSectors (what the ARM7 validates)
    u32 exp = 0; bool tiles = true;
    for (u32 i = 0; i < m.extentCount; i++) { if (gExt[i].fileSector != exp || !gExt[i].count) tiles = false; exp += gExt[i].count; }
    CHECK(tiles && exp == m.totalSectors, "extents don't tile the image exactly (%u vs %u)", exp, m.totalSectors);

    // count-only mode agrees
    ImageMap c = { nullptr, 0, 0, 0, 0 };
    CHECK(fatMap_build(path, c) == MapError::None && c.extentCount == m.extentCount, "count-only mode disagrees");

    // verify passes and leaves the disk byte-identical
    auto before = diskSnapshot();
    MapError v = fatMap_verify(path, m, true);
    CHECK(v == MapError::None, "verify failed: %s", mapErrorString(v));
    CHECK(diskSnapshot() == before, "verify modified the disk");

    // a deliberately wrong map must be rejected, without modifying the disk
    if (m.extentCount >= 2 || m.totalSectors > 16) {
        mount_extent_t saved[MOUNT_MAX_EXTENTS]; memcpy(saved, gExt, sizeof(gExt));
        u32 victim = m.extentCount >= 2 ? 1 : 0;
        // point the run at another location: far into the card (blank area)
        gExt[victim].sdSector += 200000;
        MapError w = fatMap_verify(path, m, true);
        CHECK(w == MapError::VerifyFailed, "corrupted map was accepted (%s)", mapErrorString(w));
        CHECK(diskSnapshot() == before, "rejecting a bad map modified the disk");
        memcpy(gExt, saved, sizeof(gExt));
    }
}

// BIN/CUE .bin files: the partial last sector is mapped too
static void testRoundUp(const char* name)
{
    printf(" %s (round up)\n", name);
    char path[128]; snprintf(path, sizeof(path), "sd:/images/%s", name);
    TFile& t = gFiles[name];
    ImageMap m = {};
    m.extents = gExt; m.maxExtents = MOUNT_MAX_EXTENTS; m.roundUp = true;
    CHECK(fatMap_build(path, m) == MapError::None, "build");
    CHECK(m.totalSectors == (t.size + 511) / 512, "rounded totalSectors %u want %u", m.totalSectors, (t.size + 511) / 512);
    // The truth table lists whole sectors only; within a cluster the partial
    // last sector must come right after the sector before it on the card.
    FatVolume vol;
    CHECK(fatVolume_load(vol) == FatVolumeResult::Ok, "volume");
    u32 last = m.totalSectors - 1, prevSd = 0;
    bool ok, okPrev;
    u32 sd = mapLookup(m, last, ok);
    CHECK(trueSector(&t, last - 1, prevSd), "truth for the sector before");
    sd = mapLookup(m, last, ok);
    (void)okPrev;
    if (last % vol.sectorsPerCluster != 0)
        CHECK(ok && sd == prevSd + 1, "partial last sector at %u (want %u)", sd, prevSd + 1);
    else
        CHECK(ok, "partial last sector mapped");
    u32 exp = 0; for (u32 i = 0; i < m.extentCount; i++) exp += gExt[i].count;
    CHECK(exp == m.totalSectors, "extents cover the rounded size");
}

int main(int argc, char** argv)
{
    for (int i = 1; i + 1 < argc; i += 2) {
        printf("== %s\n", argv[i]);
        gDisk = open(argv[i], O_RDWR);
        loadTable(argv[i + 1]);
        if (strstr(argv[i], "fat32")) {
            testFile("CONTIG.IMG", MapError::None, 1);
            testFile("PIECES.IMG", MapError::None, 7);
            testFile("SCATTER.IMG", MapError::None, 0);
            testFile("BLANK.IMG", MapError::None, 4);
            testFile("ODD.IMG", MapError::None, 0);
            testRoundUp("ODD.IMG");
            testFile("BROKEN.IMG", MapError::BrokenChain, 0);
            testFile("ZEROLEN.IMG", MapError::EmptyFile, 0);
            testFile("ISO1.ISO", MapError::None, 1);
            testFile("ISO2.ISO", MapError::None, 5);
            testReadOnly("ISO1.ISO");
            testReadOnly("ISO2.ISO");
            testReadOnly("PIECES.IMG");
            {
                // an entirely blank file mounted read-only: allowed (harmless)
                ImageMap m = { gExt, MOUNT_MAX_EXTENTS, 0, 0, 0 };
                fatMap_build("sd:/images/BLANK.IMG", m);
                CHECK(fatMap_verify("sd:/images/BLANK.IMG", m, false) == MapError::None, "read-only blank file refused");
            }
            // too fragmented for a small extent table
            ImageMap s = { gExt, 3, 0, 0, 0 };
            CHECK(fatMap_build("sd:/images/SCATTER.IMG", s) == MapError::TooFragmented, "expected TooFragmented");
            // missing file
            ImageMap n = { gExt, 3, 0, 0, 0 };
            CHECK(fatMap_build("sd:/images/NOPE.IMG", n) == MapError::OpenFailed, "expected OpenFailed");
        } else {
            testFile("F16A.IMG", MapError::None, 5);
            testFile("F16B.IMG", MapError::None, 0);
        }
        close(gDisk);
    }
    printf("\n%d checks passed, %d failed\n", gPasses, gFails);
    return gFails ? 1 : 0;
}
