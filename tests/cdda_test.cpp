// BIN/CUE discs over USB: TinyUSB's real MSC code + ImageDisk + OpticalDrive,
// with the layout built by the ARM9's CueSheet.cpp. Acts as the USB host.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <string>
#include <random>
#include <unistd.h>
#include <fcntl.h>
#include "common.h"
#include <nds/disc_io.h>
#include "tusb.h"
#include "ImageDisk.h"
#include "CueSheet.h"

static int gPasses, gFails;
#define CHECK(c, ...) do { if (c) gPasses++; else { gFails++; printf("  FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- ImageDisk environment ----------------------------------------------------
static int gDisk;
rtos_mutex_t gCardMutex;
volatile u32 gDSPicoUsbSpuriousOutCompletions = 0;
void rtos_lockMutex(rtos_mutex_t* m) { if (m->locked) { printf("  FAIL: mutex re-locked\n"); gFails++; } m->locked = 1; }
void rtos_unlockMutex(rtos_mutex_t* m) { m->locked = 0; }
extern "C" {
bool tusb_init(uint8_t, const tusb_rhport_init_t*) { return true; }
void tud_task(void) {}
bool tud_connect(void) { return true; }
bool tud_disconnect(void) { return true; }
bool tud_suspended(void) { return false; }
bool tud_mounted(void) { return true; }
}
static bool dread(sec_t s, sec_t n, void* b) { return pread(gDisk, b, n * 512, (off_t)s * 512) == (ssize_t)(n * 512); }
static bool dwrite(sec_t, sec_t, const void*) { return false; }
FN_MEDIUM_READSECTORS _DLDI_readSectors_ptr = dread;
FN_MEDIUM_WRITESECTORS _DLDI_writeSectors_ptr = dwrite;

// ---- fake USB device controller (as in msc_stream_test) --------------------------
extern "C" {
uint16_t mscd_open(uint8_t rhport, const void* itf_desc, uint16_t max_len);
bool mscd_xfer_cb(uint8_t rhport, uint8_t ep_addr, int event, uint32_t xferred_bytes);
bool mscd_control_xfer_cb(uint8_t rhport, uint8_t stage, const void* request);
void mscd_init(void);
void tud_mount_cb(void);
static uint8_t* gOutBuf; static uint8_t* gInBuf; static uint16_t gInLen; static bool gInArmed, gInStalled;
bool usbd_open_edpt_pair(uint8_t, uint8_t const*, uint8_t, uint8_t, uint8_t* out, uint8_t* in) { *out = 0x01; *in = 0x81; return true; }
bool usbd_edpt_xfer(uint8_t, uint8_t ep, uint8_t* buf, uint16_t len) { if (ep & 0x80) { gInBuf = buf; gInLen = len; gInArmed = true; } else gOutBuf = buf; return true; }
bool usbd_edpt_busy(uint8_t, uint8_t) { return false; }
void usbd_edpt_stall(uint8_t, uint8_t ep) { if (ep & 0x80) gInStalled = true; }
bool usbd_edpt_stalled(uint8_t, uint8_t ep) { return (ep & 0x80) && gInStalled; }
void dcd_event_handler(const void*, bool) { printf("  unexpected dcd event\n"); gFails++; }
bool tud_control_xfer(uint8_t, const void*, void*, uint16_t) { return true; }
bool tud_control_status(uint8_t, const void*) { return true; }
}

static std::vector<u8> command(const u8* cdb, int cdbLen, u32 transferLen, int* status)
{
    u8 cbw[31] = { 'U', 'S', 'B', 'C', 1, 2, 3, 4 };
    cbw[8] = transferLen; cbw[9] = transferLen >> 8; cbw[10] = transferLen >> 16; cbw[11] = transferLen >> 24;
    cbw[12] = 0x80; cbw[14] = cdbLen; memcpy(cbw + 15, cdb, cdbLen);
    memcpy(gOutBuf, cbw, 31);
    gInArmed = false;
    mscd_xfer_cb(0, 0x01, 0, 31);
    std::vector<u8> data;
    *status = -1;
    for (int guard = 0; guard < 1000000; guard++) {
        if (gInStalled) {
            gInStalled = false; gInArmed = false;
            struct __attribute__((packed)) { u8 t, r; u16 v, i, l; } req = { 0x02, 1, 0, 0x81, 0 };
            mscd_control_xfer_cb(0, 1, &req);
            if (gInArmed && gInLen == 13 && !memcmp(gInBuf, "USBS", 4)) { *status = gInBuf[12]; gInArmed = false; mscd_xfer_cb(0, 0x81, 0, 13); }
            return data;
        }
        if (!gInArmed) break;
        gInArmed = false;
        u8* buf = gInBuf; u16 len = gInLen;
        if (len == 13 && !memcmp(buf, "USBS", 4) && data.size() >= transferLen) {
            *status = buf[12];
            mscd_xfer_cb(0, 0x81, 0, 13);
            return data;
        }
        data.insert(data.end(), buf, buf + len);
        mscd_xfer_cb(0, 0x81, 0, len);
    }
    return data;
}

static u32 senseNow()
{
    u8 cdb[6] = { 0x03, 0, 0, 0, 18, 0 }; int st;
    auto d = command(cdb, 6, 18, &st);
    return d.size() >= 14 ? (d[2] & 0xF) << 16 | d[12] << 8 | d[13] : 0xFFFFFF;
}
static void be32(u8* p, u32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static u32 rd32be(const u8* p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static u8 bcd(u32 v) { return (u8)((v / 10) << 4 | (v % 10)); }

// ---- the disc --------------------------------------------------------------------
static std::vector<u8> gBin1, gBin2;     // the two .bin files
static std::vector<u8> gVirtual;         // both, each padded to 512 bytes
static CueSheet gSheet;
static cd_layout_t gLayout;
alignas(32) static u8 gBounce[CD_BOUNCE_BYTES];
static mount_extent_t gExt[MOUNT_MAX_EXTENTS];
static u32 gExtCount;

static void makeDataFrame(u8* f, u32 lba, std::mt19937& rng)
{
    f[0] = 0; memset(f + 1, 0xFF, 10); f[11] = 0;
    u32 a = lba + 150;
    f[12] = bcd(a / 4500); f[13] = bcd(a / 75 % 60); f[14] = bcd(a % 75); f[15] = 1;
    for (u32 i = 0; i < 2048; i++) f[16 + i] = (u8)(lba * 7 + i * 13);
    for (u32 i = 2064; i < 2352; i++) f[i] = rng();
}

// What the disc holds at an LBA (2352 bytes; zeros in the PREGAP that isn't stored)
static std::vector<u8> frameAt(u32 lba)
{
    std::vector<u8> f(2352, 0);
    for (u32 i = 0; i < gLayout.trackCount; i++) {
        const cd_track_t& t = gLayout.tracks[i];
        if (lba >= t.dataLba && lba < t.dataLba + t.frames)
            memcpy(f.data(), &gVirtual[t.byteOffset + (lba - t.dataLba) * t.sectorSize], t.sectorSize);
    }
    return f;
}

static int gSt;
static std::vector<u8> readCd(u32 lba, u32 count, u8 type, u8 flags, u8 sub = 0, u32 per = 2352)
{
    u8 cdb[12] = { 0xBE, (u8)(type << 2) }; be32(cdb + 2, lba);
    cdb[6] = count >> 16; cdb[7] = count >> 8; cdb[8] = count; cdb[9] = flags; cdb[10] = sub;
    return command(cdb, 12, count * per, &gSt);
}

int main(int argc, char** argv)
{
    const char* dataDir = argc > 1 ? argv[1] : "data";
    std::mt19937 rng(2026);
    // bin 1: track 1 data (300 frames), track 2 audio (pregap 150 frames in the file, 350 after INDEX 01)
    gBin1.resize(800 * 2352);
    for (u32 lba = 0; lba < 300; lba++) makeDataFrame(&gBin1[lba * 2352], lba, rng);
    for (u32 i = 300 * 2352; i < gBin1.size(); i++) gBin1[i] = rng();
    // bin 2: track 3 audio, 400 frames, size not a multiple of 512; PREGAP of 2 s before it isn't stored
    gBin2.resize(400 * 2352);
    for (auto& b : gBin2) b = rng();
    const char* cue =
        "REM mixed-mode test disc\n"
        "FILE \"disc (Track 1-2).bin\" BINARY\n"
        "  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n"
        "  TRACK 02 AUDIO\n    INDEX 00 00:04:00\n    INDEX 01 00:06:00\n"
        "FILE \"disc (Track 3).bin\" BINARY\n"
        "  TRACK 03 AUDIO\n    PREGAP 00:02:00\n    INDEX 01 00:00:00\n";
    CHECK(cue_parse(cue, strlen(cue), gSheet) == CueError::None, "parse");
    u64 sizes[2] = { gBin1.size(), gBin2.size() };
    u32 starts[2], total;
    memset(&gLayout, 0, sizeof(gLayout));
    gLayout.bounce = gBounce;
    gLayout.bounceSize = sizeof(gBounce);
    // CD-Text: the real Disney Princess .cdt
    static u8 cdt[4096], scratch[4096];
    {
        std::string p = std::string(dataDir) + "/disney.cdt";
        FILE* f = fopen(p.c_str(), "rb"); u32 n = f ? fread(cdt, 1, sizeof(cdt), f) : 0; if (f) fclose(f);
        gLayout.cdTextLength = cue_normalizeCdText(cdt, n, scratch);
        gLayout.cdText = cdt;
    }
    CHECK(cue_buildLayout(gSheet, sizes, gLayout, starts, total) == CueError::None, "layout");
    CHECK(gLayout.leadout == 1350 && gLayout.tracks[2].index1 == 950 && gLayout.tracks[2].index0 == 800, "layout LBAs (%u)", gLayout.leadout);

    // virtual image, scattered over the "SD card" in 7-sector pieces in reverse order
    gVirtual.assign((size_t)total * 512, 0);
    memcpy(&gVirtual[starts[0] * 512], gBin1.data(), gBin1.size());
    memcpy(&gVirtual[starts[1] * 512], gBin2.data(), gBin2.size());
    {
        u32 pieces = (total + 6) / 7;
        std::vector<u8> sd((size_t)(pieces * 7 + 64) * 512, 0xEE);
        u32 sdPos = 64;
        for (s32 p = pieces - 1; p >= 0; p--) {
            u32 fs = p * 7, n = std::min<u32>(7, total - fs);
            memcpy(&sd[(size_t)sdPos * 512], &gVirtual[(size_t)fs * 512], n * 512);
            gExt[p] = { fs, sdPos, n };
            sdPos += n;
        }
        gExtCount = pieces;
        FILE* f = fopen("cdda_sd.bin", "wb"); fwrite(sd.data(), 1, sd.size(), f); fclose(f);
    }
    gDisk = open("cdda_sd.bin", O_RDONLY);

    static mount_status_t status;
    imageDisk_init();
    imageDisk_setStatus(&status);
    mount_ipc_cmd_t c = {};
    c.cmd = MOUNT_IPC_CMD_MOUNT; c.extents = gExt; c.extentCount = gExtCount; c.totalSectors = total;
    c.mediaType = MOUNT_MEDIA_CDROM; c.readOnly = true; c.cdLayout = &gLayout;
    CHECK(imageDisk_mount(&c) == MOUNT_RESULT_OK, "mount BIN/CUE");
    u8 desc[23] = { 9, 4, 0, 0, 2, 0x08, 0x06, 0x50, 0, 7, 5, 0x01, 2, 64, 0, 0, 7, 5, 0x81, 2, 64, 0, 0 };
    mscd_init();
    mscd_open(0, desc, sizeof(desc));
    tud_mount_cb();
    { u8 tur[6] = { 0 }; command(tur, 6, 0, &gSt); senseNow(); command(tur, 6, 0, &gSt); CHECK(gSt == 0, "ready"); }

    // ---- capacity, TOC, track info ----
    {
        u8 rc[10] = { 0x25 };
        auto d = command(rc, 10, 8, &gSt);
        CHECK(gSt == 0 && rd32be(&d[0]) == 1349 && rd32be(&d[4]) == 2048, "READ CAPACITY: last LBA %u", rd32be(&d[0]));
        u8 toc[10] = { 0x43, 0, 0, 0, 0, 0, 0, 0x03, 0x24, 0 };
        d = command(toc, 10, 804, &gSt);
        CHECK(gSt == 0 && d.size() == 4 + 4 * 8 && d[2] == 1 && d[3] == 3, "TOC: tracks 1-3 + lead-out (%zu bytes)", d.size());
        if (d.size() >= 36) {
            CHECK(d[5] == 0x14 && d[6] == 1 && rd32be(&d[8]) == 0, "TOC track 1: data at 0");
            CHECK(d[13] == 0x10 && d[14] == 2 && rd32be(&d[16]) == 450, "TOC track 2: audio at 450");
            CHECK(d[21] == 0x10 && d[22] == 3 && rd32be(&d[24]) == 950, "TOC track 3: audio at 950");
            CHECK(d[29] == 0x10 && d[30] == 0xAA && rd32be(&d[32]) == 1350, "TOC lead-out 1350");
        }
        toc[1] = 0x02; // MSF
        d = command(toc, 10, 804, &gSt);
        CHECK(gSt == 0 && d.size() >= 36 && d[17] == 0 && d[18] == 8 && d[19] == 0, "TOC MSF: track 2 at 00:08:00 (%u:%u:%u)", d.size() > 19 ? d[17] : 0, d.size() > 19 ? d[18] : 0, d.size() > 19 ? d[19] : 0);
        u8 full[10] = { 0x43, 0x02, 0x02, 0, 0, 0, 0, 0x04, 0, 0 };
        d = command(full, 10, 1024, &gSt);
        CHECK(gSt == 0 && d.size() == 4 + 11 * 6, "full TOC: A0/A1/A2 + 3 tracks");
        if (d.size() == 70) {
            CHECK(d[4 + 3] == 0xA0 && d[4 + 8] == 1 && d[4 + 1] == 0x14, "A0: first track 1 (data)");
            CHECK(d[15 + 3] == 0xA1 && d[15 + 8] == 3 && d[15 + 1] == 0x10, "A1: last track 3 (audio)");
            CHECK(d[37 + 3] == 1 && d[48 + 3] == 2 && d[48 + 1] == 0x10 && d[59 + 3] == 3, "track entries");
        }
        u8 text[10] = { 0x43, 0, 0x05, 0, 0, 0, 0, 0x20, 0, 0 };
        d = command(text, 10, 8192, &gSt);
        CHECK(gSt == 0 && d.size() == 4 + gLayout.cdTextLength && !memcmp(&d[4], cdt, gLayout.cdTextLength) && d[4] == 0x80,
            "READ TOC format 5: CD-Text packs (%zu bytes)", d.size());
        u8 ti[10] = { 0x52, 0x01, 0, 0, 0, 2, 0, 0, 36, 0 };
        d = command(ti, 10, 36, &gSt);
        CHECK(gSt == 0 && d[2] == 2 && d[5] == 0x00 && d[6] == 0x0F && rd32be(&d[8]) == 450 && rd32be(&d[24]) == 500, "track info: track 2 audio, 450, 500 frames");
        ti[5] = 1;
        d = command(ti, 10, 36, &gSt);
        CHECK(gSt == 0 && d[5] == 0x04 && d[6] == 0x01 && rd32be(&d[24]) == 450, "track info: track 1 data mode 1");
        u8 di[10] = { 0x51, 0, 0, 0, 0, 0, 0, 0, 34, 0 };
        d = command(di, 10, 34, &gSt);
        CHECK(gSt == 0 && d[3] == 1 && d[6] == 3, "disc info: tracks 1-3");
        u8 gc[10] = { 0x46, 0x02, 0x00, 0x1E, 0, 0, 0, 0, 16, 0 };
        d = command(gc, 10, 16, &gSt);
        CHECK(gSt == 0 && d.size() >= 13 && d[9] == 0x1E && (d[12] & 0x01), "CD Read feature: CD-Text bit");
        u8 ms[10] = { 0x5A, 0, 0x2A, 0, 0, 0, 0, 0, 64, 0 };
        d = command(ms, 10, 64, &gSt);
        CHECK(gSt == 0 && d.size() > 13 && d[8] == 0x2A && (d[13] & 0x03) == 0x03, "mode page 2A: CD-DA supported, stream accurate");
    }

    // ---- data track through READ (10): user data out of 2352-byte frames ----
    {
        u8 r10[10] = { 0x28 }; be32(r10 + 2, 100); r10[8] = 64;
        auto d = command(r10, 10, 64 * 2048, &gSt);
        u32 bad = 0;
        for (u32 i = 0; i < 64 && d.size() == 64 * 2048; i++)
            if (memcmp(&d[i * 2048], &frameAt(100 + i)[16], 2048)) bad++;
        CHECK(gSt == 0 && d.size() == 64 * 2048 && bad == 0, "READ(10) 128 KiB of the data track (%u bad blocks)", bad);
        be32(r10 + 2, 296); r10[8] = 4;
        d = command(r10, 10, 4 * 2048, &gSt);
        CHECK(gSt == 0 && !memcmp(&d[3 * 2048], &frameAt(299)[16], 2048), "READ(10) up to the last data frame");
        be32(r10 + 2, 500); r10[8] = 1;
        command(r10, 10, 2048, &gSt);
        u32 k = senseNow();
        CHECK(gSt != 0 && k == 0x056400, "READ(10) on audio: illegal mode for this track (%06x)", k);
        u8 r12[12] = { 0xA8 }; be32(r12 + 2, 7); be32(r12 + 6, 3);
        d = command(r12, 12, 3 * 2048, &gSt);
        CHECK(gSt == 0 && d.size() == 6144 && !memcmp(&d[2048], &frameAt(8)[16], 2048), "READ(12) on the data track");
    }

    // ---- audio: READ CD of whole frames, odd lengths (not a multiple of the 8 KiB buffer) ----
    {
        auto d = readCd(450, 27, 1, 0x10);
        u32 bad = 0;
        for (u32 i = 0; i < 27 && d.size() == 27 * 2352; i++)
            if (memcmp(&d[i * 2352], frameAt(450 + i).data(), 2352)) bad++;
        CHECK(gSt == 0 && d.size() == 27 * 2352 && bad == 0, "READ CD 27 audio frames (%zu bytes, %u bad)", d.size(), bad);
        // track 2 pregap (stored) through track 3's PREGAP (silence) into track 3
        u8 msf[12] = { 0xB9, 0, 0, 0, 10, 40, 0, 12, 60, 0x10, 0, 0 }; // 00:10:40 .. 00:12:60 = LBA 640 .. 810+? 
        u32 a = 790 + 150, b = 960 + 150;
        msf[3] = a / 4500; msf[4] = a / 75 % 60; msf[5] = a % 75;
        msf[6] = b / 4500; msf[7] = b / 75 % 60; msf[8] = b % 75;
        d = command(msf, 12, 170 * 2352, &gSt);
        bad = 0; u32 zeros = 0;
        for (u32 i = 0; i < 170 && d.size() == 170 * 2352; i++) {
            auto want = frameAt(790 + i);
            if (memcmp(&d[i * 2352], want.data(), 2352)) bad++;
            if (790 + i >= 800 && 790 + i < 950) { bool z = true; for (u32 j = 0; j < 2352; j++) if (d[i * 2352 + j]) z = false; if (z) zeros++; }
        }
        CHECK(gSt == 0 && d.size() == 170 * 2352 && bad == 0 && zeros == 150, "READ CD MSF across the PREGAP: %u bad, %u silent frames", bad, zeros);
        d = readCd(1340, 10, 0, 0xF8);
        CHECK(gSt == 0 && d.size() == 23520 && !memcmp(&d[9 * 2352], frameAt(1349).data(), 2352), "last frames of the disc");
        readCd(1345, 10, 1, 0x10);
        u32 k = senseNow();
        CHECK(gSt != 0 && k == 0x052100, "past the lead-out: LBA out of range (%06x)", k);
    }

    // ---- data track through READ CD: raw frames and field selections ----
    {
        auto d = readCd(16, 4, 0, 0xF8);
        CHECK(gSt == 0 && d.size() == 4 * 2352 && !memcmp(d.data(), frameAt(16).data(), 2352) && !memcmp(&d[3 * 2352], frameAt(19).data(), 2352), "READ CD raw data frames (sync+header+data+EDC/ECC)");
        d = readCd(16, 2, 2, 0x10, 0, 2048);
        CHECK(gSt == 0 && d.size() == 4096 && !memcmp(d.data(), &frameAt(16)[16], 2048), "READ CD mode 1 user data");
        u8 cdb[12] = { 0xBE, 0 }; be32(cdb + 2, 20); cdb[8] = 3; cdb[9] = 0x78; // headers + data + EDC
        d = command(cdb, 12, 3 * 2340, &gSt);
        CHECK(gSt == 0 && d.size() == 3 * 2340 && !memcmp(d.data(), &frameAt(20)[12], 2340), "READ CD header+data+EDC (2340 bytes)");
        cdb[9] = 0xB8; // sync + data + EDC without the header: not contiguous
        command(cdb, 12, 3 * 2348, &gSt);
        u32 k = senseNow();
        CHECK(gSt != 0 && k == 0x052400, "non-contiguous fields refused (%06x)", k);
        readCd(16, 1, 1, 0x10);
        k = senseNow();
        CHECK(gSt != 0 && k == 0x056400, "CD-DA type on a data track refused (%06x)", k);
        readCd(16, 1, 0, 0x10, 1);
        k = senseNow();
        CHECK(gSt != 0 && k == 0x052400, "sub-channel request refused (%06x)", k);
        readCd(290, 20, 0, 0x10);
        k = senseNow();
        CHECK(gSt != 0 && k == 0x056400, "read crossing data -> audio refused (%06x)", k);
    }

    // ---- the real Disney Princess cue: 10 tracks in the TOC as listed ----
    {
        imageDisk_unmount();
        std::string p = std::string(dataDir) + "/disney.cue";
        FILE* f = fopen(p.c_str(), "rb"); static char text[4096]; u32 n = fread(text, 1, sizeof(text), f); fclose(f);
        CHECK(cue_parse(text, n, gSheet) == CueError::None, "disney.cue");
        u64 size = 282630432;
        u32 st[1], tot;
        gLayout.cdText = nullptr; gLayout.cdTextLength = 0;
        CHECK(cue_buildLayout(gSheet, &size, gLayout, st, tot) == CueError::None, "disney layout");
        close(gDisk);
        int fd = open("cdda_big.bin", O_RDWR | O_CREAT | O_TRUNC, 0644); ftruncate(fd, (off_t)tot * 512); close(fd);
        gDisk = open("cdda_big.bin", O_RDONLY);
        static mount_extent_t one[1]; one[0] = { 0, 0, tot };
        mount_ipc_cmd_t c2 = {};
        c2.cmd = MOUNT_IPC_CMD_MOUNT; c2.extents = one; c2.extentCount = 1; c2.totalSectors = tot;
        c2.mediaType = MOUNT_MEDIA_CDROM; c2.readOnly = true; c2.cdLayout = &gLayout;
        CHECK(imageDisk_mount(&c2) == MOUNT_RESULT_OK, "mount disney layout");
        tud_mount_cb();
        u8 tur[6] = { 0 }; command(tur, 6, 0, &gSt); senseNow();
        u8 toc[10] = { 0x43, 0x02, 0, 0, 0, 0, 0, 0x03, 0x24, 0 };
        auto d = command(toc, 10, 804, &gSt);
        CHECK(gSt == 0 && d.size() == 4 + 11 * 8 && d[3] == 10, "10 tracks + lead-out");
        const u32 want[10][3] = { {0,2,0}, {4,37,51}, {7,4,39}, {8,35,62}, {11,51,24}, {14,35,33}, {17,0,48}, {19,31,0}, {21,49,74}, {25,41,0} };
        u32 bad = 0;
        for (u32 i = 0; i < 10 && d.size() >= 92; i++) {
            const u8* e = &d[4 + i * 8];
            if (e[1] != 0x10 || e[2] != i + 1 || e[5] != want[i][0] || e[6] != want[i][1] || e[7] != want[i][2]) bad++;
        }
        CHECK(bad == 0, "track starts = cue INDEX 01 + 2 s (%u wrong)", bad);
        CHECK(d.size() >= 92 && d[4 + 80 + 2] == 0xAA && d[4 + 80 + 5] == 26 && d[4 + 80 + 6] == 44 && d[4 + 80 + 7] == 16, "lead-out at 26:44:16 (120166 + 150 frames)");
        imageDisk_unmount();
    }

    // ---- the ARM7 refuses a layout that points outside the image ----
    {
        static cd_layout_t badLayout;
        badLayout = gLayout;
        badLayout.tracks[9].frames += 1000;
        static mount_extent_t one[1]; one[0] = { 0, 0, (282630432 + 511) / 512 };
        mount_ipc_cmd_t c3 = {};
        c3.cmd = MOUNT_IPC_CMD_MOUNT; c3.extents = one; c3.extentCount = 1; c3.totalSectors = one[0].count;
        c3.mediaType = MOUNT_MEDIA_CDROM; c3.readOnly = true; c3.cdLayout = &badLayout;
        CHECK(imageDisk_mount(&c3) == MOUNT_RESULT_BAD_ARGS, "layout past the end of the image refused");
    }

    printf("cdda_test: %d checks passed, %d failed\n", gPasses, gFails);
    return gFails ? 1 : 0;
}
