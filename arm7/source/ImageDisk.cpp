#include "common.h"
#include <string.h>
#include <stddef.h>
#include <nds/disc_io.h>
#include <libtwl/rtos/rtosMutex.h>
#include <libtwl/rtos/rtosThread.h>
#include "tusb.h"
#include "ImageDisk.h"
#include "OpticalDrive.h"

#ifndef IMAGEDISK_READ_KEYXY
#define IMAGEDISK_READ_KEYXY() (*(vu16*)0x04000136) // REG_KEYXY (ARM7 only)
#endif

// Counted in platform/DSPicoUsbOutEndpoint.cpp
extern volatile u32 gDSPicoUsbSpuriousOutCompletions;

extern FN_MEDIUM_READSECTORS _DLDI_readSectors_ptr;
extern FN_MEDIUM_WRITESECTORS _DLDI_writeSectors_ptr;

// Protects all media state below. Held for the whole duration of a USB
// read/write so that unmounting waits for in-flight transfers to finish.
static rtos_mutex_t sMediaMutex;

static mount_status_t* sStatus;

static const mount_extent_t* sExtents;
static u32 sExtentCount;
static u32 sTotalSectors;
static u32 sMediaType;
static bool sMounted;
static bool sReadOnly;
static bool sEjected;
static bool sUnitAttention;
static bool sHostPrevent;   // host locked the medium (PREVENT MEDIUM REMOVAL)

static OpticalState sOptical;

static bool sUsbStarted;
static bool sConnected;
static rtos_thread_t sUsbThread;
static u32 sUsbThreadStack[512];

static void usbThreadMain(void* arg)
{
    while (true)
    {
        tud_task();
    }
}

static const u8* readSpan(u32 byteOffset, u32 length);

void imageDisk_init()
{
    rtos_createMutex(&sMediaMutex);
    optical_setSpanReader(readSpan, CD_BOUNCE_BYTES - 2 * MOUNT_SECTOR_SIZE);
}

static const cd_layout_t* sLayout;  // BIN/CUE disc being served, or null

/// A BIN/CUE layout from the ARM9 must stay inside the image and make sense
/// before the ARM7 reads anything by it.
static bool validateLayout(const cd_layout_t* l, u32 totalSectors)
{
    if (!l->bounce || l->bounceSize < CD_BOUNCE_BYTES || l->trackCount == 0 || l->trackCount > CD_MAX_TRACKS)
        return false;
    if (l->cdText && l->cdTextLength > CD_TEXT_MAX_BYTES)
        return false;
    u64 imageBytes = (u64)totalSectors * MOUNT_SECTOR_SIZE;
    for (u32 i = 0; i < l->trackCount; i++)
    {
        const cd_track_t& t = l->tracks[i];
        if (t.sectorSize != 2048 && t.sectorSize != 2336 && t.sectorSize != 2352)
            return false;
        if (t.mode > CD_TRACK_MODE2 || (t.mode != CD_TRACK_AUDIO && t.userOffset + 2048u > t.sectorSize))
            return false;
        if (t.frames == 0 || t.byteOffset + (u64)t.frames * t.sectorSize > imageBytes)
            return false;
        if (t.index0 > t.index1 || t.dataLba + t.frames > l->leadout || t.index1 >= l->leadout)
            return false;
        if (i > 0 && t.index0 < l->tracks[i - 1].index1)
            return false;
    }
    return true;
}

void imageDisk_setStatus(mount_status_t* status)
{
    sStatus = status;
}

mount_status_t* imageDisk_status()
{
    return sStatus;
}

static bool validateExtents(const mount_ipc_cmd_t* cmd)
{
    if (!cmd->extents || cmd->extentCount == 0 || cmd->extentCount > MOUNT_MAX_EXTENTS)
        return false;
    if (cmd->totalSectors == 0)
        return false;

    // Extents must tile the image from sector 0 upwards without gaps.
    u32 expected = 0;
    for (u32 i = 0; i < cmd->extentCount; i++)
    {
        const mount_extent_t& e = cmd->extents[i];
        if (e.fileSector != expected || e.count == 0)
            return false;
        if (e.sdSector + e.count < e.sdSector) // overflow
            return false;
        expected += e.count;
    }
    return expected >= cmd->totalSectors;
}

u32 imageDisk_mount(const mount_ipc_cmd_t* cmd)
{
    if (!validateExtents(cmd))
        return MOUNT_RESULT_BAD_ARGS;
    if (cmd->mediaType == MOUNT_MEDIA_CDROM && cmd->totalSectors < 4)
        return MOUNT_RESULT_BAD_ARGS; // smaller than one 2048-byte block
    const cd_layout_t* layout = (const cd_layout_t*)cmd->cdLayout;
    if (layout && (cmd->mediaType != MOUNT_MEDIA_CDROM || !validateLayout(layout, cmd->totalSectors)))
        return MOUNT_RESULT_BAD_ARGS;

    rtos_lockMutex(&sMediaMutex);
    if (sMounted)
    {
        rtos_unlockMutex(&sMediaMutex);
        return MOUNT_RESULT_ALREADY_MOUNTED;
    }

    sLayout = layout;
    sExtents = cmd->extents;
    sExtentCount = cmd->extentCount;
    sTotalSectors = cmd->totalSectors;
    sMediaType = cmd->mediaType;
    sReadOnly = cmd->readOnly != 0;
    sEjected = false;
    sUnitAttention = true;
    sHostPrevent = false;
    sMounted = true;
    if (sMediaType == MOUNT_MEDIA_CDROM)
        optical_insert(sOptical, sTotalSectors / 4, sLayout);

    if (sStatus)
    {
        sStatus->mounted = 1;
        sStatus->readOnly = sReadOnly;
        sStatus->hostPrevent = 0;
        sStatus->hostEjected = 0;
        sStatus->writesSinceSync = 0;
        sStatus->sectorsRead = 0;
        sStatus->sectorsWritten = 0;
        sStatus->ioErrors = 0;
        sStatus->everConfigured = 0;
        sStatus->usbBadWrites = 0;
        sStatus->lastWriteFrame = sStatus->frame;
        sStatus->lastIoFrame = sStatus->frame;
    }
    rtos_unlockMutex(&sMediaMutex);

    // Present the device to the host only while an image is attached:
    // to the host (PC, original Xbox, ...) this looks exactly like
    // plugging in a USB drive.
    if (!sUsbStarted)
    {
        tusb_rhport_init_t devInit =
        {
            .role = TUSB_ROLE_DEVICE,
            .speed = TUSB_SPEED_AUTO
        };
        tusb_init(0, &devInit);
        rtos_createThread(&sUsbThread, 3, usbThreadMain, NULL, sUsbThreadStack, sizeof(sUsbThreadStack));
        rtos_wakeupThread(&sUsbThread);
        sUsbStarted = true;
    }
    else
    {
        tud_connect();
    }
    sConnected = true;

    return MOUNT_RESULT_OK;
}

void imageDisk_shutdown()
{
    imageDisk_unmount();
    if (sUsbStarted && sConnected)
    {
        tud_disconnect();
        sConnected = false;
    }
    rtos_lockMutex(&gCardMutex); // waits for a transfer in progress; never released
}

u32 imageDisk_reload()
{
    rtos_lockMutex(&sMediaMutex);
    if (!sMounted)
    {
        rtos_unlockMutex(&sMediaMutex);
        return MOUNT_RESULT_NOT_MOUNTED;
    }
    if (sEjected)
    {
        sEjected = false;
        sUnitAttention = true; // "medium may have changed"
        if (sMediaType == MOUNT_MEDIA_CDROM && !sOptical.present)
            optical_insert(sOptical, sTotalSectors / 4, sLayout);
        if (sStatus)
            sStatus->hostEjected = 0;
    }
    rtos_unlockMutex(&sMediaMutex);
    return MOUNT_RESULT_OK;
}

// TinyUSB: the host has (re)configured the device - after plugging in, a PC
// reboot, the BIOS handing over to the OS, or a USB reset. A real drive
// reports "medium may have changed" to a new host and a CD drive raises a
// new-media event, so the new driver re-reads the disc from scratch.
extern "C" void tud_mount_cb(void)
{
    rtos_lockMutex(&sMediaMutex);
    sHostPrevent = false;
    if (sStatus)
        sStatus->hostPrevent = 0;
    if (sMounted && !sEjected)
    {
        sUnitAttention = true;
        if (sMediaType == MOUNT_MEDIA_CDROM && sOptical.present)
            sOptical.pendingEvent = 2; // new media
    }
    rtos_unlockMutex(&sMediaMutex);
}

u32 imageDisk_unmount()
{
    // Taking the mutex waits for a transfer that is currently being
    // written to the SD card. Every write is synchronous (no write cache
    // on our side), so once we hold it all acknowledged data is on the SD.
    rtos_lockMutex(&sMediaMutex);
    if (!sMounted)
    {
        rtos_unlockMutex(&sMediaMutex);
        return MOUNT_RESULT_NOT_MOUNTED;
    }
    sMounted = false;
    sLayout = NULL;
    sHostPrevent = false;
    sOptical.present = false;
    sOptical.pendingEvent = 0;
    sExtents = NULL;
    sExtentCount = 0;
    sTotalSectors = 0;
    if (sStatus)
    {
        sStatus->mounted = 0;
        sStatus->hostPrevent = 0;
    }
    rtos_unlockMutex(&sMediaMutex);

    if (sUsbStarted && sConnected)
    {
        tud_disconnect();
        sConnected = false;
    }
    return MOUNT_RESULT_OK;
}

void imageDisk_updateFrame()
{
    if (!sStatus)
        return;

    sStatus->frame = sStatus->frame + 1;
    sStatus->keyXY = IMAGEDISK_READ_KEYXY();

    u32 usbState;
    if (!sUsbStarted || !sConnected)
        usbState = MOUNT_USB_IDLE;
    else if (tud_suspended())
        usbState = MOUNT_USB_SUSPENDED;
    else if (tud_mounted())
        usbState = MOUNT_USB_CONFIGURED;
    else
        usbState = MOUNT_USB_WAITING;
    sStatus->usbState = usbState;
    sStatus->usbSpurious = gDSPicoUsbSpuriousOutCompletions;
    if (usbState == MOUNT_USB_CONFIGURED)
        sStatus->everConfigured = 1;
}

// ---------------------------------------------------------------------------
// Sector mapping
// ---------------------------------------------------------------------------

static int findExtent(u32 fileSector)
{
    int lo = 0;
    int hi = (int)sExtentCount - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) >> 1;
        const mount_extent_t& e = sExtents[mid];
        if (fileSector < e.fileSector)
            hi = mid - 1;
        else if (fileSector >= e.fileSector + e.count)
            lo = mid + 1;
        else
            return mid;
    }
    return -1;
}

static bool transferSectors(u32 sector, u32 count, u8* buffer, bool write)
{
    while (count > 0)
    {
        int idx = findExtent(sector);
        if (idx < 0)
            return false;
        const mount_extent_t& e = sExtents[idx];
        u32 offset = sector - e.fileSector;
        u32 n = e.count - offset;
        if (n > count)
            n = count;

        rtos_lockMutex(&gCardMutex);
        bool ok = write
            ? _DLDI_writeSectors_ptr(e.sdSector + offset, n, buffer)
            : _DLDI_readSectors_ptr(e.sdSector + offset, n, buffer);
        rtos_unlockMutex(&gCardMutex);
        if (!ok)
            return false;

        sector += n;
        count -= n;
        buffer += n * MOUNT_SECTOR_SIZE;
    }
    return true;
}

static u32 sLastSense;

// Every error the DS reports goes through here, so the last one can be shown
// on the top screen when a host command is refused.
static void setSense(uint8_t lun, uint8_t key, uint8_t asc, uint8_t ascq)
{
    sLastSense = ((u32)key << 16) | ((u32)asc << 8) | ascq;
    tud_msc_set_sense(lun, key, asc, ascq);
}

static void noteRefused(u8 op)
{
    if (!sStatus)
        return;
    sStatus->scsiRefused = sStatus->scsiRefused + 1;
    sStatus->lastRefusedOp = op;
    sStatus->lastRefusedSense = sLastSense;
}

static u32 blockSize()
{
    return sMediaType == MOUNT_MEDIA_CDROM ? 2048 : 512;
}

// ---------------------------------------------------------------------------
// TinyUSB MSC callbacks
// ---------------------------------------------------------------------------

static_assert(offsetof(scsi_inquiry_resp_t, vendor_id) == 8, "unexpected inquiry layout");

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
{
    (void) lun;
    const char vid[] = "DSpico";
    const char* pid = sMediaType == MOUNT_MEDIA_CDROM ? "Virtual DVD-ROM" : "DSPicoDrive";
    const char rev[] = "0.1";
    memcpy(vendor_id, vid, strlen(vid));
    memcpy(product_id, pid, strlen(pid));
    memcpy(product_rev, rev, strlen(rev));
    if (sMediaType == MOUNT_MEDIA_CDROM)
    {
        // TinyUSB always reports a direct-access device; patch the
        // peripheral device type (byte 0 of the response) to CD/DVD.
        vendor_id[-8] = 0x05;
    }
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    (void) lun;
    bool ready;
    rtos_lockMutex(&sMediaMutex);
    if (!sMounted || sEjected)
    {
        setSense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00); // medium not present
        ready = false;
    }
    else if (sUnitAttention)
    {
        sUnitAttention = false;
        setSense(lun, SCSI_SENSE_UNIT_ATTENTION, 0x28, 0x00); // medium may have changed
        ready = false;
    }
    else
    {
        ready = true;
    }
    rtos_unlockMutex(&sMediaMutex);
    return ready;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t* block_count, uint16_t* block_size)
{
    (void) lun;
    rtos_lockMutex(&sMediaMutex);
    if (!sMounted || sEjected)
    {
        *block_count = 0;
        *block_size = 0;
    }
    else
    {
        u32 bs = blockSize();
        *block_count = sMediaType == MOUNT_MEDIA_CDROM ? sOptical.blocks : sTotalSectors / (bs / MOUNT_SECTOR_SIZE);
        *block_size = (uint16_t)bs;
    }
    rtos_unlockMutex(&sMediaMutex);
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    (void) lun;
    (void) power_condition;
    if (load_eject)
    {
        rtos_lockMutex(&sMediaMutex);
        if (!start && sHostPrevent && sMediaType == MOUNT_MEDIA_CDROM)
        {
            // MMC: a locked drive refuses to open its tray.
            rtos_unlockMutex(&sMediaMutex);
            setSense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x53, 0x02); // medium removal prevented
            return false;
        }
        if (!start)
        {
            // Host ejected the medium ("Safely remove" / eject).
            sEjected = true;
            if (sMediaType == MOUNT_MEDIA_CDROM)
                optical_remove(sOptical);
            if (sStatus)
            {
                sStatus->hostEjected = 1;
                sStatus->writesSinceSync = 0;
                sStatus->hostPrevent = 0;
            }
        }
        else if (sMounted)
        {
            sEjected = false;
            if (sMediaType == MOUNT_MEDIA_CDROM && !sOptical.present)
                optical_insert(sOptical, sTotalSectors / 4, sLayout);
            if (sStatus)
                sStatus->hostEjected = 0;
        }
        rtos_unlockMutex(&sMediaMutex);
    }
    return true;
}

bool tud_msc_prevent_allow_medium_removal_cb(uint8_t lun, uint8_t prohibit_removal, uint8_t control)
{
    (void) lun;
    (void) control;
    sHostPrevent = (prohibit_removal & 1) != 0;
    if (sStatus)
        sStatus->hostPrevent = sHostPrevent ? 1 : 0;
    return true;
}

bool tud_msc_is_writable_cb(uint8_t lun)
{
    (void) lun;
    return !sReadOnly && sMediaType == MOUNT_MEDIA_DISK;
}

static inline u32 be32(const u8* p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/// True if a write's data is actually the USB command block (CBW) of a
/// WRITE command for this very LBA - the signature of the DSpico USB receive
/// bug where a transfer completes without the data being copied. Real sector
/// data essentially never looks like this.
static bool isOwnCommandBlock(const u8* data, uint32_t lba)
{
    if (data[0] != 'U' || data[1] != 'S' || data[2] != 'B' || data[3] != 'C')
        return false;
    u8 cbLength = data[14];
    const u8* cdb = data + 15;
    if (cbLength < 10 || cbLength > 16)
        return false;
    switch (cdb[0])
    {
        case 0x2A: // WRITE (10)
        case 0xAA: // WRITE (12)
        case 0x2E: // WRITE AND VERIFY (10)
            return be32(cdb + 2) == lba;
        case 0x8A: // WRITE (16): low 32 bits of the 64-bit LBA
            return be32(cdb + 2) == 0 && be32(cdb + 6) == lba;
    }
    return false;
}

static int32_t readWriteCommon(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize, bool write)
{
    if (offset % MOUNT_SECTOR_SIZE != 0 || bufsize % MOUNT_SECTOR_SIZE != 0)
    {
        setSense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x24, 0x00);
        return -1;
    }

    int32_t result;
    rtos_lockMutex(&sMediaMutex);
    if (!sMounted || sEjected)
    {
        setSense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
        result = -1;
    }
    else
    {
        u32 perBlock = blockSize() / MOUNT_SECTOR_SIZE;
        u32 sector = lba * perBlock + offset / MOUNT_SECTOR_SIZE;
        u32 count = bufsize / MOUNT_SECTOR_SIZE;
        if (sector >= sTotalSectors || count > sTotalSectors - sector)
        {
            setSense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x21, 0x00); // LBA out of range
            result = -1;
        }
        else if (write && offset == 0 && isOwnCommandBlock(buffer, lba))
        {
            // Don't write it: fail the command so the host retries.
            setSense(lun, 0x0B, 0x4B, 0x00); // aborted command, data phase error
            if (sStatus)
                sStatus->usbBadWrites = sStatus->usbBadWrites + 1;
            result = -1;
        }
        else if (!transferSectors(sector, count, buffer, write))
        {
            setSense(lun, write ? SCSI_SENSE_MEDIUM_ERROR : SCSI_SENSE_MEDIUM_ERROR,
                write ? 0x0C : 0x11, 0x00); // write error / unrecovered read error
            if (sStatus)
                sStatus->ioErrors = sStatus->ioErrors + 1;
            result = -1;
        }
        else
        {
            if (sStatus)
            {
                u32 frame = sStatus->frame;
                sStatus->lastIoFrame = frame;
                if (write)
                {
                    sStatus->sectorsWritten = sStatus->sectorsWritten + count;
                    sStatus->lastWriteFrame = frame;
                    sStatus->writesSinceSync = sStatus->writesSinceSync + 1;
                }
                else
                {
                    sStatus->sectorsRead = sStatus->sectorsRead + count;
                }
            }
            result = (int32_t)bufsize;
        }
    }
    rtos_unlockMutex(&sMediaMutex);
    if (result < 0)
        noteRefused(write ? 0x2A : 0x28);
    return result;
}

// ---- BIN/CUE reads (raw 2352-byte frames etc.) ---------------------------------

static u8 sReadCdb[16];     // the command TinyUSB is currently streaming

/// TinyUSB (DSpico image mounter patch): called before each chunk of a
/// streamed read with the command being served.
extern "C" void tud_msc_read_context_cb(uint8_t lun, uint8_t const scsi_cmd[16])
{
    (void)lun;
    memcpy(sReadCdb, scsi_cmd, sizeof(sReadCdb));
}

/// Reads bytes of the virtual image into the layout's bounce buffer (main RAM).
static const u8* readSpan(u32 byteOffset, u32 length)
{
    if (!sLayout || length == 0)
        return nullptr;
    u32 first = byteOffset / MOUNT_SECTOR_SIZE;
    u32 last = (byteOffset + length - 1) / MOUNT_SECTOR_SIZE;
    u32 count = last - first + 1;
    if (count * MOUNT_SECTOR_SIZE > sLayout->bounceSize || last >= sTotalSectors)
        return nullptr;
    if (!transferSectors(first, count, sLayout->bounce, false))
        return nullptr;
    if (sStatus)
        sStatus->sectorsRead = sStatus->sectorsRead + count;
    return sLayout->bounce + byteOffset % MOUNT_SECTOR_SIZE;
}

static void opticalSense(u8 key, u8 asc, u8 ascq);

static int32_t layoutRead(uint8_t lun, uint32_t lba, uint32_t offset, u8* buffer, uint32_t bufsize)
{
    int32_t result;
    rtos_lockMutex(&sMediaMutex);
    OpticalReadPlan plan;
    sLastSense = 0;
    if (!sMounted || sEjected || !sOptical.present)
    {
        setSense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
        result = -1;
    }
    else if (!optical_planRead(sOptical, sReadCdb, plan, opticalSense))
    {
        result = -1;
    }
    else if (!optical_produce(sOptical, plan, lba - plan.cmdLbaField, offset, buffer, bufsize,
                 readSpan, CD_BOUNCE_BYTES - 2 * MOUNT_SECTOR_SIZE))
    {
        setSense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x11, 0x00); // unrecovered read error
        if (sStatus)
            sStatus->ioErrors = sStatus->ioErrors + 1;
        result = -1;
    }
    else
    {
        if (sStatus)
            sStatus->lastIoFrame = sStatus->frame;
        result = (int32_t)bufsize;
    }
    rtos_unlockMutex(&sMediaMutex);
    if (result < 0)
        noteRefused(sReadCdb[0]);
    return result;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize)
{
    if (sLayout && sMediaType == MOUNT_MEDIA_CDROM)
        return layoutRead(lun, lba, offset, (u8*)buffer, bufsize);
    return readWriteCommon(lun, lba, offset, (uint8_t*)buffer, bufsize, false);
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize)
{
    return readWriteCommon(lun, lba, offset, buffer, bufsize, true);
}

static void putBe32(u8* p, u32 v)
{
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

static bool opticalRead(u32 block, u32 count, u8* buffer)
{
    rtos_lockMutex(&sMediaMutex);
    bool ok = sMounted && !sEjected && transferSectors(block * 4, count * 4, buffer, false);
    if (ok && sStatus)
    {
        sStatus->sectorsRead = sStatus->sectorsRead + count * 4;
        sStatus->lastIoFrame = sStatus->frame;
    }
    rtos_unlockMutex(&sMediaMutex);
    return ok;
}

static void opticalSense(u8 key, u8 asc, u8 ascq)
{
    setSense(0, key, asc, ascq);
}

// SCSI commands not handled by TinyUSB itself.
static int32_t scsiCommand(uint8_t lun, uint8_t const scsi_cmd[16], void* buffer, uint16_t bufsize);

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void* buffer, uint16_t bufsize)
{
    sLastSense = 0;
    int32_t result = scsiCommand(lun, scsi_cmd, buffer, bufsize);
    if (result < 0)
        noteRefused(scsi_cmd[0]);
    return result;
}

uint32_t tud_msc_stream_read_blocks_cb(uint8_t lun, uint8_t const scsi_cmd[16], uint32_t total_bytes)
{
    (void)lun;
    if (sMediaType != MOUNT_MEDIA_CDROM || !sMounted)
        return 0;
    return optical_streamBlocks(sOptical, scsi_cmd, total_bytes);
}

static int32_t scsiCommand(uint8_t lun, uint8_t const scsi_cmd[16], void* buffer, uint16_t bufsize)
{
    u8* out = (u8*)buffer;
    int32_t resplen = 0;

    // TinyUSB passes the host's requested length (truncated to 16 bits) here,
    // not the size of its transfer buffer: never write more than the buffer.
    if (bufsize > CFG_TUD_MSC_EP_BUFSIZE)
        bufsize = CFG_TUD_MSC_EP_BUFSIZE;

    if (sMediaType == MOUNT_MEDIA_CDROM && sMounted)
    {
        bool handled;
        resplen = optical_scsi(sOptical, scsi_cmd, out, bufsize, opticalRead, opticalSense, handled);
        if (handled)
            return resplen;
        resplen = 0;
    }

    switch (scsi_cmd[0])
    {
        case 0x35: // SYNCHRONIZE CACHE (10)
        case 0x91: // SYNCHRONIZE CACHE (16)
        {
            // Every write already went straight to the SD card.
            if (sStatus)
                sStatus->writesSinceSync = 0;
            resplen = 0;
            break;
        }
        case 0x2F: // VERIFY (10)
        case 0x04: // FORMAT UNIT
        case 0x01: // REZERO UNIT
        {
            resplen = 0;
            break;
        }
        case 0x15: // MODE SELECT (6)
        case 0x55: // MODE SELECT (10)
        {
            // Accept and ignore parameters (e.g. caching mode pages).
            resplen = bufsize;
            break;
        }
        case 0x5A: // MODE SENSE (10)
        {
            u8 resp[8] = { 0, 6, 0, 0, 0, 0, 0, 0 };
            if (!tud_msc_is_writable_cb(lun))
                resp[3] = 0x80; // write protected
            u32 alloc = (scsi_cmd[7] << 8) | scsi_cmd[8];
            resplen = sizeof(resp);
            if ((u32)resplen > alloc)
                resplen = alloc;
            if (resplen > bufsize)
                resplen = bufsize;
            memcpy(out, resp, resplen);
            break;
        }
        case 0x9E: // SERVICE ACTION IN (16) - READ CAPACITY (16)
        {
            if ((scsi_cmd[1] & 0x1F) != 0x10)
            {
                setSense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
                resplen = -1;
                break;
            }
            uint32_t count;
            uint16_t size;
            tud_msc_capacity_cb(lun, &count, &size);
            if (count == 0)
            {
                setSense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
                resplen = -1;
                break;
            }
            u8 resp[32];
            memset(resp, 0, sizeof(resp));
            putBe32(resp + 4, count - 1); // upper 32 bits of last LBA stay 0
            putBe32(resp + 8, size);
            u32 alloc = ((u32)scsi_cmd[10] << 24) | ((u32)scsi_cmd[11] << 16) | ((u32)scsi_cmd[12] << 8) | scsi_cmd[13];
            resplen = sizeof(resp);
            if ((u32)resplen > alloc)
                resplen = alloc;
            if (resplen > bufsize)
                resplen = bufsize;
            memcpy(out, resp, resplen);
            break;
        }
        default:
        {
            setSense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00); // invalid command
            resplen = -1;
            break;
        }
    }

    return resplen;
}
