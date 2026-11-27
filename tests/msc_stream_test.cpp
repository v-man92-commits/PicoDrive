// Runs TinyUSB's real MSC Bulk-Only state machine (libs/tinyusb msc_device.c,
// with the image mounter's READ(12)/READ CD streaming patch) against
// ImageDisk in CD mode, acting as the USB host.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <random>
#include <unistd.h>
#include <fcntl.h>
#include "common.h"
#include <nds/disc_io.h>
#include "tusb.h"
#include "ImageDisk.h"

static int gPasses, gFails;
#define CHECK(c, ...) do { if (c) gPasses++; else { gFails++; printf("  FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- ImageDisk environment --------------------------------------------------
static int gDisk;
rtos_mutex_t gCardMutex;
volatile u32 gDSPicoUsbSpuriousOutCompletions = 0;
void rtos_lockMutex(rtos_mutex_t* m) { m->locked = 1; }
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

// ---- fake USB device controller ---------------------------------------------
extern "C" {
uint16_t mscd_open(uint8_t rhport, const void* itf_desc, uint16_t max_len);
bool mscd_xfer_cb(uint8_t rhport, uint8_t ep_addr, int event, uint32_t xferred_bytes);
bool mscd_control_xfer_cb(uint8_t rhport, uint8_t stage, const void* request);
void tud_mount_cb(void);
void mscd_init(void);
static uint8_t* gOutBuf; static uint16_t gOutLen;
static uint8_t* gInBuf; static uint16_t gInLen; static bool gInArmed, gInStalled;
bool usbd_open_edpt_pair(uint8_t, uint8_t const*, uint8_t, uint8_t, uint8_t* out, uint8_t* in) { *out = 0x01; *in = 0x81; return true; }
bool usbd_edpt_xfer(uint8_t, uint8_t ep, uint8_t* buf, uint16_t len)
{
    if (ep & 0x80) { gInBuf = buf; gInLen = len; gInArmed = true; } else { gOutBuf = buf; gOutLen = len; }
    return true;
}
bool usbd_edpt_busy(uint8_t, uint8_t) { return false; }
void usbd_edpt_stall(uint8_t, uint8_t ep) { if (ep & 0x80) gInStalled = true; }
bool usbd_edpt_stalled(uint8_t, uint8_t ep) { return (ep & 0x80) && gInStalled; }
void dcd_event_handler(const void*, bool) { printf("  unexpected dcd event\n"); gFails++; }
bool tud_control_xfer(uint8_t, const void*, void*, uint16_t) { return true; }
bool tud_control_status(uint8_t, const void*) { return true; }
}

// One Bulk-Only command as a host: returns data received; csw status in *status (-1 = stalled)
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
    for (int guard = 0; guard < 100000; guard++) {
        if (gInStalled) {
            // like a real host: CLEAR_FEATURE(ENDPOINT_HALT) on bulk-in, then read the CSW
            gInStalled = false;
            gInArmed = false;
            struct __attribute__((packed)) { u8 bmRequestType, bRequest; u16 wValue, wIndex, wLength; } req = { 0x02, 1, 0, 0x81, 0 };
            mscd_control_xfer_cb(0, 1 /* CONTROL_STAGE_SETUP */, &req);
            if (gInArmed && gInLen == 13 && !memcmp(gInBuf, "USBS", 4)) {
                *status = gInBuf[12];
                gInArmed = false;
                mscd_xfer_cb(0, 0x81, 0, 13);
            }
            return data;
        }
        if (!gInArmed) break;
        gInArmed = false;
        u8* buf = gInBuf; u16 len = gInLen;
        if (len == 13 && !memcmp(buf, "USBS", 4) && data.size() >= transferLen) {
            *status = buf[12];
            u32 residue = buf[8] | buf[9] << 8 | buf[10] << 16 | (u32)buf[11] << 24;
            if (residue) printf("  residue %u\n", residue);
            mscd_xfer_cb(0, 0x81, 0, 13);
            return data;
        }
        data.insert(data.end(), buf, buf + len);
        mscd_xfer_cb(0, 0x81, 0, len);
    }
    return data;
}

static void be32(u8* p, u32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

int main()
{
    const u32 blocks = 4096; // 8 MiB "disc"
    {
        std::mt19937 rng(7); std::vector<u8> d(blocks * 2048);
        for (auto& b : d) b = rng();
        FILE* f = fopen("disc.bin", "wb"); fwrite(d.data(), 1, d.size(), f); fclose(f);
    }
    gDisk = open("disc.bin", O_RDONLY);
    static mount_status_t status;
    imageDisk_init();
    imageDisk_setStatus(&status);
    static mount_extent_t ext[1] = { { 0, 0, blocks * 4 } };
    mount_ipc_cmd_t c = {};
    c.cmd = MOUNT_IPC_CMD_MOUNT; c.extents = ext; c.extentCount = 1;
    c.totalSectors = blocks * 4; c.mediaType = MOUNT_MEDIA_CDROM; c.readOnly = true;
    CHECK(imageDisk_mount(&c) == MOUNT_RESULT_OK, "mount");

    u8 desc[9 + 7 + 7] = { 9, 4, 0, 0, 2, 0x08, 0x06, 0x50, 0, 7, 5, 0x01, 2, 64, 0, 0, 7, 5, 0x81, 2, 64, 0, 0 };
    mscd_init();
    CHECK(mscd_open(0, desc, sizeof(desc)) != 0, "mscd_open");

    auto expect = [&](u32 lba, u32 n) { std::vector<u8> v(n * 2048); pread(gDisk, v.data(), v.size(), (off_t)lba * 2048); return v; };
    int st;

    // READ(10), 32 blocks (64 KiB, Windows' usual size)
    { u8 cdb[10] = { 0x28 }; be32(cdb + 2, 100); cdb[8] = 32;
      auto d = command(cdb, 10, 32 * 2048, &st);
      CHECK(st == 0 && d == expect(100, 32), "READ(10) 64 KiB (status %d, %zu bytes)", st, d.size()); }
    // READ(12), 32 blocks
    { u8 cdb[12] = { 0xA8 }; be32(cdb + 2, 777); be32(cdb + 6, 32);
      auto d = command(cdb, 12, 32 * 2048, &st);
      CHECK(st == 0 && d == expect(777, 32), "READ(12) 64 KiB (status %d, %zu bytes)", st, d.size()); }
    // READ CD, user data, 27 blocks (odd size, not a multiple of the 8 KiB buffer)
    { u8 cdb[12] = { 0xBE, 0 }; be32(cdb + 2, 4096 - 27); cdb[8] = 27; cdb[9] = 0x10;
      auto d = command(cdb, 12, 27 * 2048, &st);
      CHECK(st == 0 && d == expect(4096 - 27, 27), "READ CD 27 blocks to the end (status %d, %zu bytes)", st, d.size()); }
    // READ CD, mode 1, 2 blocks (small, previously handled in one buffer)
    { u8 cdb[12] = { 0xBE, 2 << 2 }; be32(cdb + 2, 5); cdb[8] = 2; cdb[9] = 0x10;
      auto d = command(cdb, 12, 2 * 2048, &st);
      CHECK(st == 0 && d == expect(5, 2), "READ CD 2 blocks (status %d)", st); }
    // a large READ(12): 1 MiB in one command
    { u8 cdb[12] = { 0xA8 }; be32(cdb + 2, 1000); be32(cdb + 6, 512);
      auto d = command(cdb, 12, 512 * 2048, &st);
      CHECK(st == 0 && d == expect(1000, 512), "READ(12) 1 MiB (status %d, %zu bytes)", st, d.size()); }
    // GET CONFIGURATION still goes through the normal SCSI callback
    { u8 cdb[10] = { 0x46, 0, 0, 0, 0, 0, 0, 0, 8 };
      auto d = command(cdb, 10, 8, &st);
      CHECK(st == 0 && d.size() == 8 && d[7] == 0x08, "GET CONFIGURATION (status %d, profile %02x)", st, d.size() >= 8 ? d[7] : 0); }
    CHECK(status.sectorsRead == (32 + 32 + 27 + 2 + 512) * 4, "sectors read %u", status.sectorsRead);
    CHECK(status.scsiRefused == 0, "nothing refused yet (%u)", status.scsiRefused);
    // raw READ CD (2352-byte sectors) is not supported: refused and reported
    { u8 cdb[12] = { 0xBE, 0 }; be32(cdb + 2, 16); cdb[8] = 1; cdb[9] = 0xF8;
      command(cdb, 12, 2352, &st);
      CHECK(st != 0, "raw READ CD should fail");
      CHECK(status.scsiRefused == 1 && status.lastRefusedOp == 0xBE && status.lastRefusedSense == 0x052400,
          "refusal recorded (%u, op %x, sense %06x)", status.scsiRefused, status.lastRefusedOp, status.lastRefusedSense); }


    // =====================================================================
    // Booting a PC from the virtual DVD drive: the sequence a BIOS / UEFI
    // firmware uses (modelled on SeaBIOS usb-msc + cdrom and EDK2
    // UsbMassStorageDxe), on an El Torito bootable image.
    // =====================================================================
    auto u32le = [](u8* p, u32 v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; };
    auto senseOf = [&](u32* key) { u8 cdb[6] = { 0x03, 0, 0, 0, 18, 0 }; int s2; auto d = command(cdb, 6, 18, &s2); *key = d.size() >= 14 ? (d[2] & 0xF) << 16 | d[12] << 8 | d[13] : 0xFFFFFF; };
    auto tur = [&]() { u8 cdb[6] = { 0x00 }; int s2; command(cdb, 6, 0, &s2); return s2; };
    auto read10 = [&](u32 lba, u32 n, int* s2) { u8 cdb[10] = { 0x28 }; be32(cdb + 2, lba); cdb[7] = n >> 8; cdb[8] = n; return command(cdb, 10, n * 2048, s2); };
    const u32 bootBlocks = 2000;
    std::vector<u8> iso(bootBlocks * 2048, 0);
    {
        std::mt19937 rng(99);
        u8* pvd = &iso[16 * 2048]; pvd[0] = 1; memcpy(pvd + 1, "CD001", 5); pvd[6] = 1;
        u8* brvd = &iso[17 * 2048]; brvd[0] = 0; memcpy(brvd + 1, "CD001", 5); brvd[6] = 1;
        memcpy(brvd + 7, "EL TORITO SPECIFICATION", 23); u32le(brvd + 0x47, 20);
        u8* term = &iso[18 * 2048]; term[0] = 0xFF; memcpy(term + 1, "CD001", 5); term[6] = 1;
        u8* cat = &iso[20 * 2048];
        cat[0] = 1; cat[0x1E] = 0x55; cat[0x1F] = 0xAA;
        u16 sum = 0; for (int i = 0; i < 32; i += 2) sum += cat[i] | cat[i + 1] << 8;
        u16 fix = (u16)(0x10000 - sum); cat[0x1C] = fix; cat[0x1D] = fix >> 8;
        cat[32] = 0x88; cat[33] = 0; cat[38] = 4; u32le(cat + 40, 30); // no emulation, 4 x 512 bytes at RBA 30
        for (u32 i = 30 * 2048; i < 31 * 2048; i++) iso[i] = rng();
        for (u32 i = 100 * 2048; i < bootBlocks * 2048; i++) iso[i] = rng();
        FILE* f = fopen("boot.iso", "wb"); fwrite(iso.data(), 1, iso.size(), f); fclose(f);
    }
    imageDisk_unmount();
    close(gDisk);
    gDisk = open("boot.iso", O_RDONLY);
    {
        static mount_extent_t bext[1] = { { 0, 0, bootBlocks * 4 } };
        mount_ipc_cmd_t bc = {};
        bc.cmd = MOUNT_IPC_CMD_MOUNT; bc.extents = bext; bc.extentCount = 1;
        bc.totalSectors = bootBlocks * 4; bc.mediaType = MOUNT_MEDIA_CDROM; bc.readOnly = true;
        CHECK(imageDisk_mount(&bc) == MOUNT_RESULT_OK, "mount boot.iso as CD");
    }
    tud_mount_cb(); // the PC powers on and configures the device
    {
        u8 cdb[6] = { 0x12, 0, 0, 0, 36, 0 };
        auto d = command(cdb, 6, 36, &st);
        CHECK(st == 0 && d.size() == 36 && (d[0] & 0x1F) == 0x05 && (d[1] & 0x80), "BIOS: INQUIRY says removable CD/DVD (type %02x)", d.size() ? d[0] : 0xFF);
        u32 key;
        CHECK(tur() != 0, "BIOS: first TEST UNIT READY fails (unit attention)");
        senseOf(&key);
        CHECK(key == 0x062800, "BIOS: sense 06/28/00 (%06x)", key);
        CHECK(tur() == 0, "BIOS: then ready");
        u8 rc[10] = { 0x25 };
        d = command(rc, 10, 8, &st);
        u32 last = d.size() == 8 ? (u32)d[0] << 24 | d[1] << 16 | d[2] << 8 | d[3] : 0, bs = d.size() == 8 ? (u32)d[4] << 24 | d[5] << 16 | d[6] << 8 | d[7] : 0;
        CHECK(st == 0 && last == bootBlocks - 1 && bs == 2048, "BIOS: READ CAPACITY %u x %u", last, bs);
        // El Torito: boot record volume descriptor, catalog, boot image
        d = read10(17, 1, &st);
        CHECK(st == 0 && d.size() == 2048 && !memcmp(&d[7], "EL TORITO SPECIFICATION", 23), "BIOS: boot record at LBA 17");
        u32 catLba = d.size() == 2048 ? d[0x47] | d[0x48] << 8 | d[0x49] << 16 | (u32)d[0x4A] << 24 : 0;
        d = read10(catLba, 1, &st);
        u16 sum = 0; for (int i = 0; i < 32 && d.size() == 2048; i += 2) sum += d[i] | d[i + 1] << 8;
        CHECK(st == 0 && d.size() == 2048 && sum == 0 && d[0x1E] == 0x55 && d[0x1F] == 0xAA && d[32] == 0x88, "BIOS: boot catalog valid");
        u32 rba = d.size() == 2048 ? d[40] | d[41] << 8 | d[42] << 16 | (u32)d[43] << 24 : 0, cnt = d.size() == 2048 ? d[38] | d[39] << 8 : 0;
        u32 blocks = (cnt * 512 + 2047) / 2048;
        d = read10(rba, blocks, &st);
        CHECK(st == 0 && d.size() == blocks * 2048 && !memcmp(d.data(), &iso[rba * 2048], cnt * 512), "BIOS: boot image loaded (%u blocks at %u)", blocks, rba);
        // the OS kernel/installer then reads big chunks
        d = read10(100, 32, &st);
        CHECK(st == 0 && d.size() == 65536 && !memcmp(d.data(), &iso[100 * 2048], 65536), "installer: 64 KiB reads");
        u8 ms6[6] = { 0x1A, 0, 0x3F, 0, 192, 0 };
        d = command(ms6, 6, 192, &st);
        CHECK(st == 0 && d.size() >= 4 && (d[2] & 0x80), "MODE SENSE(6) all pages: write-protected");

        // the BIOS hands over to the OS: USB is reconfigured
        tud_mount_cb();
        CHECK(tur() != 0, "after reconfigure: unit attention again");
        senseOf(&key);
        CHECK(key == 0x062800 && tur() == 0, "after reconfigure: then ready");
        u8 gesn[10] = { 0x4A, 0x01, 0, 0, 0x10, 0, 0, 0, 8, 0 };
        d = command(gesn, 10, 8, &st);
        CHECK(st == 0 && d.size() == 8 && (d[4] & 0x0F) == 2 && (d[5] & 0x02), "after reconfigure: new-media event, disc present");

        // installer locks the drive; an eject must be refused while locked
        u8 lock[6] = { 0x1E, 0, 0, 0, 1, 0 }, unlock[6] = { 0x1E, 0, 0, 0, 0, 0 };
        u8 eject[6] = { 0x1B, 0, 0, 0, 0x02, 0 }, load[6] = { 0x1B, 0, 0, 0, 0x03, 0 };
        command(lock, 6, 0, &st);
        CHECK(st == 0, "PREVENT MEDIUM REMOVAL");
        command(eject, 6, 0, &st);
        senseOf(&key);
        CHECK(st != 0 && key == 0x055302, "eject while locked refused (%06x)", key);
        CHECK(tur() == 0, "disc still there");
        command(unlock, 6, 0, &st);
        command(eject, 6, 0, &st);
        CHECK(st == 0 && status.hostEjected == 1, "eject after unlock");
        CHECK(tur() != 0, "ejected: not ready");
        senseOf(&key);
        CHECK(key == 0x023A00, "ejected: medium not present (%06x)", key);
        d = read10(17, 1, &st);
        CHECK(st != 0, "ejected: reads fail");
        // the DS "closes the tray" (X)
        CHECK(imageDisk_reload() == MOUNT_RESULT_OK && status.hostEjected == 0, "reload from the DS");
        CHECK(tur() != 0, "reloaded: unit attention");
        senseOf(&key);
        CHECK(key == 0x062800 && tur() == 0, "reloaded: ready");
        d = read10(17, 1, &st);
        CHECK(st == 0 && !memcmp(&d[7], "EL TORITO SPECIFICATION", 23), "reloaded: readable");
        // the host can also close the tray itself
        command(eject, 6, 0, &st);
        command(load, 6, 0, &st);
        CHECK(st == 0 && status.hostEjected == 0, "host load (close tray)");
        tur(); senseOf(&key);
        CHECK(tur() == 0, "host load: ready");
    }

    // ---- the same image as a read-only USB disk (L+A): SeaBIOS disk setup ----
    imageDisk_unmount();
    {
        static mount_extent_t bext[1] = { { 0, 0, bootBlocks * 4 } };
        mount_ipc_cmd_t bc = {};
        bc.cmd = MOUNT_IPC_CMD_MOUNT; bc.extents = bext; bc.extentCount = 1;
        bc.totalSectors = bootBlocks * 4; bc.mediaType = MOUNT_MEDIA_DISK; bc.readOnly = true;
        CHECK(imageDisk_mount(&bc) == MOUNT_RESULT_OK, "mount as read-only disk");
        u8 inq[6] = { 0x12, 0, 0, 0, 36, 0 };
        auto d = command(inq, 6, 36, &st);
        CHECK(st == 0 && (d[0] & 0x1F) == 0 && (d[1] & 0x80), "disk: INQUIRY direct-access, removable");
        u32 key;
        tur(); senseOf(&key);
        CHECK(tur() == 0, "disk: ready");
        u8 rc[10] = { 0x25 };
        d = command(rc, 10, 8, &st);
        CHECK(st == 0 && d.size() == 8 && d[6] == 0x02 && d[7] == 0x00, "disk: 512-byte sectors");
        u8 geo[6] = { 0x1A, 0, 0x04, 0, 36, 0 };
        d = command(geo, 6, 36, &st);
        senseOf(&key);
        CHECK(st != 0 && key == 0x052400, "disk: geometry page refused like a USB stick (%06x)", key);
        u8 all[6] = { 0x1A, 0, 0x3F, 0, 192, 0 };
        d = command(all, 6, 192, &st);
        CHECK(st == 0 && d.size() == 4 && (d[2] & 0x80), "disk: MODE SENSE(6) all pages still answered (WP)");
        u8 r10[10] = { 0x28, 0, 0, 0, 0, 64, 0, 0, 1, 0 }; // LBA 64 = byte 32768: the PVD
        d = command(r10, 10, 512, &st);
        CHECK(st == 0 && d.size() == 512 && !memcmp(&d[1], "CD001", 5), "disk: 512-byte reads");
    }

    printf("msc_stream: %d checks passed, %d failed\n", gPasses, gFails);
    return gFails ? 1 : 0;
}
