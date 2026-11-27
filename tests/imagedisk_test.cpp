// Host test for arm7/source/ImageDisk.cpp (USB mass storage side)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <map>
#include <string>
#include <random>
#include <unistd.h>
#include <fcntl.h>
#include "common.h"
#include <nds/disc_io.h>
#include "tusb.h"
#include "ImageDisk.h"
#include "OpticalDrive.h"

static int gDisk;
static int gPasses, gFails;
#define CHECK(cond, ...) do { if (cond) gPasses++; else { gFails++; printf("  FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- stubs ---------------------------------------------------------------
rtos_mutex_t gCardMutex;
volatile u32 gDSPicoUsbSpuriousOutCompletions = 0;
static int gLockDepth;
void rtos_lockMutex(rtos_mutex_t* m) { if (m->locked) { printf("  FAIL: mutex re-locked (deadlock on DS)\n"); gFails++; } m->locked = 1; gLockDepth++; }
void rtos_unlockMutex(rtos_mutex_t* m) { m->locked = 0; gLockDepth--; }
static int gInits, gConnects, gDisconnects;
bool tusb_init(uint8_t, const tusb_rhport_init_t*) { gInits++; return true; }
void tud_task(void) {}
bool tud_connect(void) { gConnects++; return true; }
bool tud_disconnect(void) { gDisconnects++; return true; }
static bool gMountedHost = true;
bool tud_suspended(void) { return false; }
bool tud_mounted(void) { return gMountedHost; }
static uint8_t gKey, gAsc, gAscq;
bool tud_msc_set_sense(uint8_t, uint8_t key, uint8_t asc, uint8_t ascq) { gKey = key; gAsc = asc; gAscq = ascq; return true; }

static std::vector<std::pair<u32,u32>> gWrites; // sd sector, count
static bool dread(sec_t s, sec_t n, void* b) { return pread(gDisk, b, n * 512, (off_t)s * 512) == (ssize_t)(n * 512); }
static bool dwrite(sec_t s, sec_t n, const void* b) { gWrites.push_back({(u32)s, (u32)n}); return pwrite(gDisk, b, n * 512, (off_t)s * 512) == (ssize_t)(n * 512); }
FN_MEDIUM_READSECTORS _DLDI_readSectors_ptr = dread;
FN_MEDIUM_WRITESECTORS _DLDI_writeSectors_ptr = dwrite;

// ---- truth from mkfat ---------------------------------------------------
struct Run { u32 fs, sd, n; };
static std::map<std::string, std::vector<Run>> gTruth;
static std::map<std::string, u32> gSize;
static void load(const char* txt)
{
    FILE* f = fopen(txt, "r"); char name[64]; u32 size, start, nr, ps;
    fscanf(f, "%u", &ps);
    while (fscanf(f, "%63s %u %u %u", name, &size, &start, &nr) == 4) {
        std::vector<Run> v; for (u32 i = 0; i < nr; i++) { Run r; fscanf(f, "%u %u %u", &r.fs, &r.sd, &r.n); v.push_back(r); }
        gTruth[name] = v; gSize[name] = size;
    }
    fclose(f);
}
static u32 trueSd(const std::vector<Run>& v, u32 fs) { for (auto& r : v) if (fs >= r.fs && fs < r.fs + r.n) return r.sd + fs - r.fs; return 0xFFFFFFFF; }

static mount_status_t gStatus;
static mount_extent_t gExt[MOUNT_MAX_EXTENTS];

static u32 doMount(const char* name, bool ro, u32 mediaType = MOUNT_MEDIA_DISK)
{
    auto& v = gTruth[name];
    for (size_t i = 0; i < v.size(); i++) gExt[i] = { v[i].fs, v[i].sd, v[i].n };
    mount_ipc_cmd_t c = {};
    c.cmd = MOUNT_IPC_CMD_MOUNT; c.extents = gExt; c.extentCount = v.size();
    c.totalSectors = gSize[name] / 512; c.mediaType = mediaType; c.readOnly = ro;
    return imageDisk_mount(&c);
}

int main()
{
    gDisk = open("fat32.img", O_RDWR);
    load("fat32.txt");
    imageDisk_init();
    imageDisk_setStatus(&gStatus);
    std::mt19937 rng(42);

    // ---- validation of the extent list -------------------------------------
    {
        mount_ipc_cmd_t c = {}; c.extents = gExt; c.totalSectors = 100;
        gExt[0] = { 0, 5000, 50 }; gExt[1] = { 51, 9000, 50 }; c.extentCount = 2;   // gap
        CHECK(imageDisk_mount(&c) == MOUNT_RESULT_BAD_ARGS, "gap accepted");
        gExt[1] = { 50, 9000, 0 };
        CHECK(imageDisk_mount(&c) == MOUNT_RESULT_BAD_ARGS, "zero-length run accepted");
        gExt[1] = { 50, 9000, 40 };
        CHECK(imageDisk_mount(&c) == MOUNT_RESULT_BAD_ARGS, "runs shorter than image accepted");
        c.extentCount = 0;
        CHECK(imageDisk_mount(&c) == MOUNT_RESULT_BAD_ARGS, "empty list accepted");
        CHECK(gInits == 0, "USB started by a rejected mount");
    }

    // ---- not mounted ---------------------------------------------------------
    {
        CHECK(!tud_msc_test_unit_ready_cb(0) && gKey == 2 && gAsc == 0x3A, "TUR with no image should be NOT READY/medium not present");
        uint32_t n; uint16_t s; tud_msc_capacity_cb(0, &n, &s);
        CHECK(n == 0, "capacity without image");
        u8 buf[512];
        CHECK(tud_msc_read10_cb(0, 0, 0, buf, 512) < 0, "read without image succeeded");
        imageDisk_updateFrame();
        CHECK(gStatus.usbState == MOUNT_USB_IDLE, "usb state should be idle");
    }

    // ---- mount a fragmented image -------------------------------------------
    const char* img = "PIECES.IMG";
    u32 total = gSize[img] / 512;
    CHECK(doMount(img, false) == MOUNT_RESULT_OK, "mount failed");
    CHECK(gInits == 1 && gConnects == 0, "first mount should init USB once");
    CHECK(doMount(img, false) == MOUNT_RESULT_ALREADY_MOUNTED, "double mount allowed");
    imageDisk_updateFrame();
    CHECK(gStatus.mounted && gStatus.usbState == MOUNT_USB_CONFIGURED && gStatus.everConfigured, "status after mount");

    CHECK(!tud_msc_test_unit_ready_cb(0) && gKey == 6 && gAsc == 0x28, "first TUR should report UNIT ATTENTION (medium changed)");
    CHECK(tud_msc_test_unit_ready_cb(0), "second TUR should be ready");
    {
        uint32_t n; uint16_t s; tud_msc_capacity_cb(0, &n, &s);
        CHECK(n == total && s == 512, "capacity %u/%u want %u/512", n, s, total);
        u8 vid[8] = {}, pid[16] = {}, rev[4] = {};
        scsi_inquiry_resp_t inq = {}; memset(inq.vendor_id, ' ', 28);
        tud_msc_inquiry_cb(0, inq.vendor_id, inq.product_id, inq.product_rev);
        CHECK(inq.peripheral_device_type == 0, "disk mode should report direct-access device");
        (void)vid; (void)pid; (void)rev;
    }

    // whole image read in 8 KiB chunks matches the true layout
    {
        u32 bad = 0;
        std::vector<u8> buf(8192), want(512);
        for (u32 lba = 0; lba < total; lba += 16) {
            u32 n = std::min<u32>(16, total - lba);
            int32_t r = tud_msc_read10_cb(0, lba, 0, buf.data(), n * 512);
            if (r != (int32_t)(n * 512)) { bad++; continue; }
            for (u32 i = 0; i < n; i++) {
                pread(gDisk, want.data(), 512, (off_t)trueSd(gTruth[img], lba + i) * 512);
                if (memcmp(want.data(), buf.data() + i * 512, 512)) bad++;
            }
        }
        CHECK(bad == 0, "%u mismatches reading the whole image", bad);
        CHECK(gStatus.sectorsRead == total, "read counter %u want %u", gStatus.sectorsRead, total);
    }

    // random writes (crossing run boundaries) land exactly in the image's sectors
    {
        gWrites.clear();
        std::vector<u8> buf(8192);
        u32 bad = 0;
        for (int t = 0; t < 300; t++) {
            u32 n = 1 + rng() % 16;
            u32 lba = rng() % (total - n + 1);
            for (auto& b : buf) b = rng();
            if (tud_msc_write10_cb(0, lba, 0, buf.data(), n * 512) != (int32_t)(n * 512)) { bad++; continue; }
            std::vector<u8> chk(512);
            for (u32 i = 0; i < n; i++) {
                pread(gDisk, chk.data(), 512, (off_t)trueSd(gTruth[img], lba + i) * 512);
                if (memcmp(chk.data(), buf.data() + i * 512, 512)) bad++;
            }
        }
        CHECK(bad == 0, "%u write mismatches", bad);
        // every SD write must be inside the image's own sectors
        u32 outside = 0;
        for (auto& w : gWrites) for (u32 i = 0; i < w.second; i++) {
            bool in = false;
            for (auto& r : gTruth[img]) if (w.first + i >= r.sd && w.first + i < r.sd + r.n) in = true;
            if (!in) outside++;
        }
        CHECK(outside == 0, "%u sectors written outside the image!", outside);
        CHECK(gStatus.writesSinceSync == 300, "writesSinceSync %u", gStatus.writesSinceSync);
    }

    // the DSpico receive bug: a WRITE whose "data" is its own command block
    {
        auto makeCbw = [](u8* b, u8 op, u32 lba) {
            memset(b, 0, 512);
            memcpy(b, "USBC", 4); b[4] = 0x20; b[5] = 0xF6; b[6] = 0xC4; b[7] = 0x69;
            b[9] = 2; b[14] = op == 0x8A ? 16 : 10; b[15] = op;
            if (op == 0x8A) { b[21] = lba >> 24; b[22] = lba >> 16; b[23] = lba >> 8; b[24] = lba; b[28] = 1; }
            else { b[17] = lba >> 24; b[18] = lba >> 16; b[19] = lba >> 8; b[20] = lba; b[23] = 1; }
        };
        std::vector<u8> buf(512);
        u32 lba = 37;
        std::vector<u8> before(512), after(512);
        pread(gDisk, before.data(), 512, (off_t)trueSd(gTruth[img], lba) * 512);
        gWrites.clear();
        makeCbw(buf.data(), 0x2A, lba);
        CHECK(tud_msc_write10_cb(0, lba, 0, buf.data(), 512) < 0, "own WRITE(10) CBW as data was accepted");
        CHECK(gWrites.empty() && gStatus.usbBadWrites == 1, "CBW write reached the SD (%zu writes)", gWrites.size());
        CHECK(gKey == 0x0B, "sense should be ABORTED COMMAND, got %u", gKey);
        makeCbw(buf.data(), 0x8A, lba);
        CHECK(tud_msc_write10_cb(0, lba, 0, buf.data(), 512) < 0 && gWrites.empty(), "own WRITE(16) CBW accepted");
        pread(gDisk, after.data(), 512, (off_t)trueSd(gTruth[img], lba) * 512);
        CHECK(before == after, "sector changed after refused writes");
        // a CBW for a *different* LBA is just data that happens to look like one: allowed
        makeCbw(buf.data(), 0x2A, lba + 1);
        CHECK(tud_msc_write10_cb(0, lba, 0, buf.data(), 512) == 512 && gWrites.size() == 1, "legit data starting with USBC was refused");
        pread(gDisk, after.data(), 512, (off_t)trueSd(gTruth[img], lba) * 512);
        CHECK(after == buf, "legit write not stored");
        gDSPicoUsbSpuriousOutCompletions = 5;
        imageDisk_updateFrame();
        CHECK(gStatus.usbSpurious == 5, "spurious counter not published");
    }

    // out of range
    {
        std::vector<u8> buf(1024);
        CHECK(tud_msc_read10_cb(0, total - 1, 0, buf.data(), 1024) < 0 && gKey == 5 && gAsc == 0x21, "read past end not rejected");
        gWrites.clear();
        CHECK(tud_msc_write10_cb(0, total, 0, buf.data(), 512) < 0 && gWrites.empty(), "write past end not rejected");
        CHECK(tud_msc_write10_cb(0, 0xFFFFFFFF, 0, buf.data(), 512) < 0 && gWrites.empty(), "huge LBA not rejected");
    }

    // SCSI extras
    {
        u8 cmd[16] = { 0x35 }; u8 out[64];
        CHECK(tud_msc_scsi_cb(0, cmd, out, 64) == 0 && gStatus.writesSinceSync == 0, "SYNCHRONIZE CACHE");
        u8 ms[16] = { 0x5A, 0, 0x3F, 0, 0, 0, 0, 0, 8 };
        CHECK(tud_msc_scsi_cb(0, ms, out, 64) == 8 && (out[3] & 0x80) == 0, "MODE SENSE(10) r/w");
        u8 rc16[16] = { 0x9E, 0x10 }; rc16[13] = 32;
        CHECK(tud_msc_scsi_cb(0, rc16, out, 64) == 32 && ((out[4] << 24) | (out[5] << 16) | (out[6] << 8) | out[7]) == total - 1 && out[10] == 2, "READ CAPACITY(16)");
        u8 bad[16] = { 0xEE };
        CHECK(tud_msc_scsi_cb(0, bad, out, 64) < 0 && gKey == 5 && gAsc == 0x20, "unknown command not rejected");
    }

    // host locks, then ejects
    {
        tud_msc_prevent_allow_medium_removal_cb(0, 1, 0);
        CHECK(gStatus.hostPrevent == 1, "prevent flag");
        tud_msc_start_stop_cb(0, 0, false, true);
        CHECK(gStatus.hostEjected == 1 && gStatus.hostPrevent == 0, "eject flags");
        CHECK(!tud_msc_test_unit_ready_cb(0) && gKey == 2 && gAsc == 0x3A, "after eject TUR should be medium not present");
        u8 b[512];
        CHECK(tud_msc_read10_cb(0, 0, 0, b, 512) < 0, "read after eject succeeded");
        tud_msc_start_stop_cb(0, 0, true, true); // host loads it again
        CHECK(gStatus.hostEjected == 0 && tud_msc_test_unit_ready_cb(0), "reload after eject");
    }

    // unmount
    {
        CHECK(imageDisk_unmount() == MOUNT_RESULT_OK, "unmount");
        CHECK(gDisconnects == 1, "unmount should disconnect USB");
        CHECK(gStatus.mounted == 0, "status mounted cleared");
        u8 b[512];
        CHECK(tud_msc_read10_cb(0, 0, 0, b, 512) < 0 && gKey == 2, "read after unmount");
        CHECK(imageDisk_unmount() == MOUNT_RESULT_NOT_MOUNTED, "double unmount");
        imageDisk_updateFrame();
        CHECK(gStatus.usbState == MOUNT_USB_IDLE, "usb idle after unmount");
    }

    // read-only remount uses connect (not a second init)
    {
        CHECK(doMount("CONTIG.IMG", true) == MOUNT_RESULT_OK, "ro mount");
        CHECK(gInits == 1 && gConnects == 1, "remount should reconnect, not re-init");
        CHECK(!tud_msc_is_writable_cb(0), "read-only reported writable");
        u8 ms[16] = { 0x5A, 0, 0x3F, 0, 0, 0, 0, 0, 8 }; u8 out[64];
        CHECK(tud_msc_scsi_cb(0, ms, out, 64) == 8 && (out[3] & 0x80), "MODE SENSE(10) WP bit");
        imageDisk_unmount();
    }

    // ---- optical drive (ISO) mode, CD-sized image -----------------------------
    {
        const char* iso = "CONTIG.IMG";
        u32 blocks = gSize[iso] / 2048;
        CHECK(doMount(iso, true, MOUNT_MEDIA_CDROM) == MOUNT_RESULT_OK, "cd mount");
        uint32_t n; uint16_t sz; tud_msc_capacity_cb(0, &n, &sz);
        CHECK(sz == 2048 && n == blocks, "cd capacity %u/%u", n, sz);
        scsi_inquiry_resp_t inq = {};
        memset(inq.vendor_id, ' ', 28);
        tud_msc_inquiry_cb(0, inq.vendor_id, inq.product_id, inq.product_rev);
        CHECK(inq.peripheral_device_type == 5 && !memcmp(inq.product_id, "Virtual DVD-ROM", 15), "cd inquiry");
        CHECK(!tud_msc_is_writable_cb(0), "cd must be read-only");
        CHECK(!tud_msc_test_unit_ready_cb(0) && gKey == 6, "cd first TUR = unit attention");
        CHECK(tud_msc_test_unit_ready_cb(0), "cd ready");

        u8 out[8192 + 64];
        auto cmd = [&](std::initializer_list<int> bytes, uint16_t bufsize = 8192) {
            u8 c[16] = {}; int i = 0; for (int b : bytes) c[i++] = (u8)b;
            gKey = 0; memset(out, 0xEE, sizeof(out));
            return tud_msc_scsi_cb(0, c, out, bufsize);
        };
        auto rd32 = [](const u8* p) { return ((u32)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; };

        // media events: new media once, then no change
        CHECK(cmd({0x4A, 0x01, 0, 0, 0x10, 0, 0, 0, 8}) == 8 && out[2] == 0x04 && out[4] == 2 && out[5] == 0x02, "event: new media");
        CHECK(cmd({0x4A, 0x01, 0, 0, 0x10, 0, 0, 0, 8}) == 8 && out[4] == 0 && out[5] == 0x02, "event: no change");
        CHECK(cmd({0x4A, 0x00, 0, 0, 0x10, 0, 0, 0, 8}) < 0 && gKey == 5, "event: async mode rejected");
        CHECK(cmd({0x4A, 0x01, 0, 0, 0x02, 0, 0, 0, 8}) == 4 && (out[2] & 0x80), "event: other class = no event");

        // READ TOC format 0 (LBA, then MSF)
        int32_t r = cmd({0x43, 0, 0, 0, 0, 0, 0, 0, 100});
        CHECK(r == 20 && out[0] == 0 && out[1] == 18 && out[2] == 1 && out[3] == 1, "toc header (%d)", r);
        CHECK(out[5] == 0x14 && out[6] == 1 && rd32(out + 8) == 0, "toc track 1");
        CHECK(out[14] == 0xAA && rd32(out + 16) == blocks, "toc lead-out at %u", rd32(out + 16));
        r = cmd({0x43, 0x02, 0, 0, 0, 0, 0, 0, 100});
        u32 f = blocks + 150;
        CHECK(r == 20 && out[9] == 0 && out[10] == 2 && out[11] == 0, "toc MSF track 1 = 00:02:00");
        CHECK(out[17] == f / 4500 && out[18] == (f / 75) % 60 && out[19] == f % 75, "toc MSF lead-out");
        CHECK(cmd({0x43, 0, 0, 0, 0, 0, 0xAA, 0, 100}) == 12 && out[6] == 0xAA, "toc from lead-out only");
        CHECK(cmd({0x43, 0, 0, 0, 0, 0, 2, 0, 100}) < 0 && gKey == 5, "toc bad start track");
        CHECK(cmd({0x43, 0, 0, 0, 0, 0, 0, 0, 4}) == 4, "toc honours allocation length");
        // format 1 (session info), also via the legacy byte-9 location; format 2 (full TOC)
        CHECK(cmd({0x43, 0, 1, 0, 0, 0, 0, 0, 100}) == 12 && out[1] == 10 && out[6] == 1, "toc session info");
        CHECK(cmd({0x43, 0, 0, 0, 0, 0, 0, 0, 100, 0x40}) == 12, "toc legacy format field");
        r = cmd({0x43, 0x02, 2, 0, 0, 0, 0, 0, 200});
        CHECK(r == 48 && out[4 + 3] == 0xA0 && out[15 + 3] == 0xA1 && out[26 + 3] == 0xA2 && out[37 + 3] == 0x01, "full toc points (%d)", r);

        // disc / track information
        CHECK(cmd({0x51, 0, 0, 0, 0, 0, 0, 0, 34}) == 34 && out[2] == 0x0E && out[1] == 32, "disc info");
        CHECK(cmd({0x52, 1, 0, 0, 0, 1, 0, 0, 36}) == 36 && rd32(out + 24) == blocks && out[2] == 1, "track info by number");
        CHECK(cmd({0x52, 0, 0, 0, 0, 5, 0, 0, 36}) == 36, "track info by LBA");
        CHECK(cmd({0x52, 1, 0, 0, 0, 2, 0, 0, 36}) < 0, "track 2 doesn't exist");

        // GET CONFIGURATION
        r = cmd({0x46, 0, 0, 0, 0, 0, 0, 0x01, 0x00});
        CHECK(r > 8 && ((out[6] << 8) | out[7]) == 0x0008 && rd32(out) == (u32)r - 4, "config: current profile CD-ROM (%d)", r);
        CHECK(out[8] == 0 && out[9] == 0 && out[11] == 8 && ((out[12] << 8) | out[13]) == 0x0010 && out[14] == 0 && ((out[16] << 8) | out[17]) == 0x0008 && out[18] == 1, "config: profile list");
        r = cmd({0x46, 2, 0, 0x1E, 0, 0, 0, 0x01, 0x00});
        CHECK(r == 16 && out[8] == 0 && out[9] == 0x1E && (out[10] & 1), "config: single CD Read feature current");
        r = cmd({0x46, 1, 0, 0x1F, 0, 0, 0, 0x01, 0x00});
        CHECK(r == 8, "config: DVD Read not current on a CD (%d)", r);

        // MODE SENSE(10) capabilities page; unsupported page rejected
        r = cmd({0x5A, 0, 0x2A, 0, 0, 0, 0, 0, 64});
        CHECK(r == 30 && out[8] == 0x2A && out[9] == 0x14 && out[14] == 0x29 && out[10] == 0, "mode page 2A (%d)", r);
        CHECK(cmd({0x5A, 0, 0x3F, 0, 0, 0, 0, 0, 64}) == 42, "mode page 3F");
        CHECK(cmd({0x5A, 0, 0x0E, 0, 0, 0, 0, 0, 64}) < 0 && gKey == 5, "mode page 0E unsupported");
        CHECK(cmd({0xBD, 0, 0, 0, 0, 0, 0, 0, 0, 8}) == 8 && out[1] == 0, "mechanism status");
        CHECK(cmd({0xAD, 0, 0, 0, 0, 0, 0, 0x00, 0x08, 0x04}) < 0 && gKey == 5 && gAsc == 0x30, "DVD structure on a CD rejected");

        // reads: READ(12) and READ CD return the same data as the image
        std::vector<u8> want(4096);
        for (u32 i = 0; i < 8; i++) pread(gDisk, want.data() + i * 512, 512, (off_t)trueSd(gTruth[iso], 12 + i) * 512);
        CHECK(cmd({0xA8, 0, 0, 0, 0, 3, 0, 0, 0, 2}) == 4096 && !memcmp(out, want.data(), 4096), "READ(12)");
        CHECK(cmd({0xBE, 0x08, 0, 0, 0, 3, 0, 0, 2, 0x10}) == 4096 && !memcmp(out, want.data(), 4096), "READ CD user data");
        CHECK(cmd({0xBE, 0, 0, 0, 0, 3, 0, 0, 1, 0xF8}) < 0 && gKey == 5, "READ CD raw sectors rejected");
        CHECK(cmd({0xA8, 0, 0, 0, 0, 3, 0, 0, 0, 5}) < 0 && gKey == 5, "READ(12) bigger than the buffer rejected");
        // the host's requested length must never overflow the 8 KiB buffer
        CHECK(cmd({0xA8, 0, 0, 0, 0, 3, 0, 0, 0, 6}, 65535) < 0 && out[8192] == 0xEE, "buffer overflow guard");
        CHECK(cmd({0xA8, 0, 0, 0, (int)(blocks >> 8), (int)(blocks & 0xFF), 0, 0, 0, 1}) < 0 && gAsc == 0x21, "READ(12) past the end");
        CHECK(tud_msc_read10_cb(0, 3, 0, out, 4096) == 4096 && !memcmp(out, want.data(), 4096), "READ(10) 2048-byte blocks");

        // eject from the host: tray open, removal event, then reload
        tud_msc_start_stop_cb(0, 0, false, true);
        CHECK(!tud_msc_test_unit_ready_cb(0) && gKey == 2 && gAsc == 0x3A, "after eject: no medium");
        CHECK(cmd({0x4A, 0x01, 0, 0, 0x10, 0, 0, 0, 8}) == 8 && out[4] == 3 && out[5] == 0, "event: media removal");
        CHECK(cmd({0x43, 0, 0, 0, 0, 0, 0, 0, 100}) < 0 && gKey == 2, "TOC without a disc");
        CHECK(cmd({0x46, 0, 0, 0, 0, 0, 0, 0x01, 0x00}) > 8 && out[6] == 0 && out[7] == 0, "config: no current profile when ejected");
        CHECK(cmd({0xBD, 0, 0, 0, 0, 0, 0, 0, 0, 8}) == 8 && (out[1] & 0x10), "mechanism: door open");
        tud_msc_start_stop_cb(0, 0, true, true);
        CHECK(cmd({0x4A, 0x01, 0, 0, 0x10, 0, 0, 0, 8}) == 8 && out[4] == 2 && out[5] == 0x02, "event: new media after reload");
        CHECK(gStatus.hostEjected == 0 && tud_msc_test_unit_ready_cb(0), "ready after reload");
        imageDisk_unmount();
    }

    // ---- optical layer on its own: DVD-sized disc ------------------------------
    {
        OpticalState st = {};
        optical_insert(st, 2000000); // ~4 GB
        CHECK(optical_isDvd(st), "DVD profile for >CD size");
        auto sense = [](u8 k, u8 a, u8 q) { gKey = k; gAsc = a; gAscq = q; };
        auto rdfn = [](u32, u32, u8*) { return true; };
        u8 out[8192];
        bool handled;
        auto run = [&](std::initializer_list<int> bytes) {
            u8 c[16] = {}; int i = 0; for (int b : bytes) c[i++] = (u8)b;
            gKey = 0;
            return optical_scsi(st, c, out, sizeof(out), rdfn, sense, handled);
        };
        CHECK(run({0x46, 0, 0, 0, 0, 0, 0, 0x01, 0x00}) > 8 && out[6] == 0 && out[7] == 0x10, "DVD: current profile DVD-ROM");
        int32_t r = run({0xAD, 0, 0, 0, 0, 0, 0, 0x00, 0x08, 0x04});
        u32 endPsn = (out[4 + 9] << 16) | (out[4 + 10] << 8) | out[4 + 11];
        CHECK(r == 2052 && out[4] == 0x01 && endPsn == 0x30000 + 2000000 - 1, "DVD physical format (%d, end %x)", r, endPsn);
        CHECK(run({0xAD, 0, 0, 0, 0, 0, 0, 0x01, 0, 8}) == 8 && out[4] == 0, "DVD copyright: none");
        CHECK(run({0x5A, 0, 0x2A, 0, 0, 0, 0, 0, 64}) == 30 && out[10] == 0x08, "DVD read capability bit");
        CHECK(run({0x43, 0x02, 0, 0, 0, 0, 0, 0, 100}) == 20 && out[17] == 255 && out[18] == 59 && out[19] == 74, "DVD MSF clamps at 255:59:74");
        CHECK(run({0x12}) == -1 && !handled, "unknown command falls through");
    }

    // ---- whole SD card sharing: one identity extent over the entire card -----
    {
        imageDisk_unmount();
        off_t bytes = lseek(gDisk, 0, SEEK_END);
        u32 card = (u32)(bytes / 512);
        gExt[0] = { 0, 0, card };
        mount_ipc_cmd_t c = {};
        c.cmd = MOUNT_IPC_CMD_MOUNT; c.extents = gExt; c.extentCount = 1;
        c.totalSectors = card; c.mediaType = MOUNT_MEDIA_DISK; c.readOnly = false;
        CHECK(imageDisk_mount(&c) == MOUNT_RESULT_OK, "whole-card mount refused");
        tud_msc_test_unit_ready_cb(0);
        uint32_t n; uint16_t sz; tud_msc_capacity_cb(0, &n, &sz);
        CHECK(n == card, "whole-card capacity %u want %u", n, card);
        u32 bad = 0;
        std::vector<u8> got(512), want(512);
        u32 probes[] = { 0, 1, 32, card / 3, card / 2, card - 2, card - 1 };
        for (u32 lba : probes) {
            pread(gDisk, want.data(), 512, (off_t)lba * 512);
            if (tud_msc_read10_cb(0, lba, 0, got.data(), 512) != 512 || memcmp(got.data(), want.data(), 512)) bad++;
        }
        CHECK(bad == 0, "whole-card identity reads: %u mismatches", bad);
        CHECK(tud_msc_read10_cb(0, card, 0, got.data(), 512) < 0, "read past end of card succeeded");
        // a write lands on exactly the same SD sector (then restore it)
        u32 lba = card - 1;
        std::vector<u8> orig(512), pat(512, 0xA5);
        pread(gDisk, orig.data(), 512, (off_t)lba * 512);
        gWrites.clear();
        CHECK(tud_msc_write10_cb(0, lba, 0, pat.data(), 512) == 512, "whole-card write failed");
        CHECK(gWrites.size() == 1 && gWrites[0].first == lba, "whole-card write went to the wrong sector");
        pread(gDisk, got.data(), 512, (off_t)lba * 512);
        CHECK(!memcmp(got.data(), pat.data(), 512), "whole-card write not on the card");
        // the CBW-as-data guard still protects the boot sector in this mode
        std::vector<u8> cbw(512, 0);
        memcpy(cbw.data(), "USBC", 4); cbw[8] = 0; cbw[14] = 10; cbw[15] = 0x2A; // WRITE10, LBA 0
        CHECK(tud_msc_write10_cb(0, 0, 0, cbw.data(), 512) < 0, "CBW written over sector 0 of the card");
        pwrite(gDisk, orig.data(), 512, (off_t)lba * 512);
        CHECK(imageDisk_unmount() == MOUNT_RESULT_OK, "whole-card unmount");
    }

    CHECK(gLockDepth == 0, "mutex imbalance %d", gLockDepth);
    printf("\n%d checks passed, %d failed\n", gPasses, gFails);
    return gFails ? 1 : 0;
}
