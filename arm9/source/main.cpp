#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <libtwl/gfx/gfxStatus.h>
#include <libtwl/mem/memExtern.h>
#include <libtwl/rtos/rtosIrq.h>
#include <libtwl/rtos/rtosThread.h>
#include <libtwl/rtos/rtosEvent.h>
#include <libtwl/ipc/ipcSync.h>
#include <libtwl/ipc/ipcFifoSystem.h>
#include "dldiIpc.h"
#include "sdDisc.h"
#include "mountClient.h"
#include "FatMap.h"
#include "ImageInfo.h"
#include "QuickAlloc.h"
#include "FatVolume.h"
#include "exitToMenu.h"
#include "cueImage.h"
#include <malloc.h>
#include <nds/arm9/cache.h>
#include <libtwl/mem/memExtern.h>
#include "ui.h"
#include "fatfs/fsSyscalls.h"
#include <fcntl.h>
#include <nds/system.h>
#include <nds/exceptions.h>
#include "web/wifiNet.h"
#include "web/webApp.h"
#include "web/webBridge.h"
#include "shim/ndsFifoShim.h"

#include "version.h"
#define IMAGE_DIR       "sd:/images"
#define MAX_ENTRIES     128
#define NAME_LEN        128
#define LIST_ROWS       18
#define LIST_FIRST_ROW  2

// FAT32 files can be up to 4 GiB - 1, but libfat treats any write to a file
// within 512 bytes of that as an append and truncates it. 4095 MiB keeps
// every in-place write (verification, zeroing) working.
// The largest image depends on the SD card's filesystem (sd_maxImageBytes):
// 4095 MiB on FAT, up to 2 TiB on exFAT (exFAT build only).
static const u64 MiB = 1024ull * 1024ull;

struct Entry
{
    char name[NAME_LEN];
    u64 size;
    bool infoValid;
    ImageFormat format;
    bool layoutValid;
    u32 fragments;
    MapError layoutError;
    bool isCue;         // a .cue: mounts its .bin files as a CD (audio / mixed / data)
    bool cueOk;         // the sheet and its files are usable
};

enum class Mode
{
    Browse,
    ConfirmUnmount,
    ConfirmDelete,
    CreateSize,
    CreateName,
    Message,
    StartMenu,
    Fatal
};

struct SizePreset
{
    const char* label;
    u32 mib;    // 0 = custom
};

static const SizePreset sPresets[] =
#ifdef IMAGE_MOUNTER_EXFAT
{
    { "8 MB   (Xbox memory unit)", 8 },
    { "32 MB", 32 },
    { "128 MB", 128 },
    { "512 MB", 512 },
    { "1 GB", 1024 },
    { "2 GB", 2048 },
    { "4095 MB (FAT32 maximum)", 4095 },
    { "8 GB   (exFAT card only)", 8192 },
    { "16 GB  (exFAT card only)", 16384 },
    { "32 GB  (exFAT card only)", 32768 },
    { "Custom", 0 }
};
#else
{
    { "8 MB   (Xbox memory unit)", 8 },
    { "16 MB", 16 },
    { "32 MB", 32 },
    { "64 MB", 64 },
    { "128 MB", 128 },
    { "256 MB", 256 },
    { "512 MB", 512 },
    { "1 GB", 1024 },
    { "2 GB", 2048 },
    { "4095 MB (maximum)", 4095 },
    { "Custom", 0 }
};
#endif
static const int PRESET_COUNT = sizeof(sPresets) / sizeof(sPresets[0]);

static const char sNameChars[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-";
static const int NAME_EDIT_LEN = 16;

static rtos_event_t sVblankEvent;

static Entry sEntries[MAX_ENTRIES];
static int sEntryCount;
static int sSkippedEntries;
static int sCursor;
static int sScroll;
static int sRestFrames;

alignas(32) static mount_extent_t sExtents[MOUNT_MAX_EXTENTS];
alignas(32) static u8 sZeroBuffer[64 * 1024];

static bool sMounted;
static char sMountedName[NAME_LEN];
static bool sMountedReadOnly;
static MountMediaType sMountedMedia;
static bool sMountedSd;
static u32 sReadRateKBs;         // host read speed over the last second (0 when idle)
static u32 sRateFrame, sRateSectors;          // the whole SD card is shared (libfat unmounted)
static int sMenuIndex;
alignas(32) static mount_extent_t sSdExtent[1];
alignas(32) static u8 sCardSector[512];
static u32 sMountedSectors;
static u32 sMountedCdBlocks;    // CD/DVD size in blocks (frames for BIN/CUE)
static u32 sMountedFragments;

static Mode sMode = Mode::Browse;
static char sMsg[4][UI_COLS + 1];
static UiColor sMsgColor;

static int sPresetIndex;
static u32 sCustomMiB = 100;
static u64 sFreeBytes;
static char sNewName[NAME_EDIT_LEN + 1];
static int sNameCursor;
static bool sCreateQuick = true;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Watchdog for the WiFi work: if the main loop stops for 2 seconds (a library
// call that never returns), the VBlank interrupt draws what it was doing and
// what each CPU has seen, so a photo of the screen says where it got stuck.
static volatile u32 sLoopCount, sLoopSeen, sStuckFrames;
static volatile bool sInWaitFrame;
static void drawStuckScreen();

static void vblankIrq(u32 irqMask)
{
    rtos_signalEvent(&sVblankEvent);
    ndsFifoShim_pump();
    if (sLoopCount != sLoopSeen)
    {
        sLoopSeen = sLoopCount;
        sStuckFrames = 0;
    }
    else if (++sStuckFrames >= 120 && (sStuckFrames % 30) == 0 &&
             (gWifiStep || wifiNet_state() != WifiNetState::Off))
        drawStuckScreen();
}

static void waitFrame()
{
    sLoopCount++;
    sInWaitFrame = true;
    rtos_waitEvent(&sVblankEvent, true, true);
    sInWaitFrame = false;
}

static void entryPath(char* out, size_t outSize, const char* name)
{
    snprintf(out, outSize, IMAGE_DIR "/%s", name);
}

static void showMessage(UiColor color, const char* l1, const char* l2 = "", const char* l3 = "", const char* l4 = "")
{
    snprintf(sMsg[0], sizeof(sMsg[0]), "%s", l1);
    snprintf(sMsg[1], sizeof(sMsg[1]), "%s", l2);
    snprintf(sMsg[2], sizeof(sMsg[2]), "%s", l3);
    snprintf(sMsg[3], sizeof(sMsg[3]), "%s", l4);
    sMsgColor = color;
    sMode = Mode::Message;
}

/// Shows a full-screen "please wait" note immediately (for blocking work).
static void showBusy(const char* title, const char* line)
{
    ui_clear(UI_BOTTOM);
    ui_line(UI_BOTTOM, 10, UI_CYAN, " %s", title);
    ui_line(UI_BOTTOM, 12, UI_WHITE, " %s", line);
    ui_flush();
}

static u64 presetBytes(int index)
{
    u64 mib = sPresets[index].mib ? sPresets[index].mib : sCustomMiB;
    return mib * MiB;
}

static u32 maxImageMiB()
{
    return (u32)(sd_maxImageBytes() / MiB);
}

static int compareEntries(const void* a, const void* b)
{
    return strcasecmp(((const Entry*)a)->name, ((const Entry*)b)->name);
}

static int findEntry(const char* name)
{
    for (int i = 0; i < sEntryCount; i++)
        if (!strcasecmp(sEntries[i].name, name))
            return i;
    return -1;
}

static void clampCursor()
{
    if (sCursor >= sEntryCount)
        sCursor = sEntryCount - 1;
    if (sCursor < 0)
        sCursor = 0;
    if (sCursor < sScroll)
        sScroll = sCursor;
    if (sCursor >= sScroll + LIST_ROWS)
        sScroll = sCursor - LIST_ROWS + 1;
    if (sScroll < 0)
        sScroll = 0;
}

// ---------------------------------------------------------------------------
// BIN/CUE images
// ---------------------------------------------------------------------------

static CueSheet sCue;                                   // scratch: one sheet at a time
static u64 sCueSizes[CUE_MAX_FILES];
static cd_layout_t sCueLayout;                          // the mounted disc (read by the ARM7)
alignas(32) static u8 sCueBounce[CD_BOUNCE_BYTES];      // ARM7 scratch while mounted
alignas(32) static u8 sCueCdText[CD_TEXT_MAX_BYTES];
static u8 sInfoCdText[CD_TEXT_MAX_BYTES];               // for the info panel (sCueCdText may be in use)

static int findEntryCount(const char* name, int count)
{
    for (int i = 0; i < count; i++)
        if (!strcasecmp(sEntries[i].name, name))
            return i;
    return -1;
}

/// A .cue stands for its .bin (and .cdt) files: show only the .cue, sized as
/// the files together.
static void hideCueFiles()
{
    static bool hidden[MAX_ENTRIES];
    memset(hidden, 0, sizeof(hidden));
    for (int i = 0; i < sEntryCount; i++)
    {
        Entry& e = sEntries[i];
        if (!cueImage_isCueName(e.name))
            continue;
        e.isCue = true;
        if (cueImage_parse(IMAGE_DIR, e.name, sCue) != CueError::None)
            continue;
        u64 total = 0;
        for (u32 f = 0; f <= sCue.fileCount; f++)
        {
            const char* ref = f < sCue.fileCount ? sCue.files[f] : sCue.cdTextFile;
            if (!cueImage_isLocalName(ref))
                continue;
            int j = findEntryCount(ref, sEntryCount);
            if (j >= 0 && j != i)
            {
                hidden[j] = true;
                if (f < sCue.fileCount)
                    total += sEntries[j].size;
            }
        }
        if (total)
            e.size = total;
    }
    int out = 0;
    for (int i = 0; i < sEntryCount; i++)
        if (!hidden[i])
            sEntries[out++] = sEntries[i];
    sEntryCount = out;
}

static void cueFormat(Entry& e)
{
    ImageFormat& f = e.format;
    memset(&f, 0, sizeof(f));
    int missing;
    CueError err = cueImage_load(IMAGE_DIR, e.name, sCue, sCueSizes, missing);
    u32 starts[CUE_MAX_FILES], total;
    static cd_layout_t info;
    if (err == CueError::None)
    {
        memset(&info, 0, sizeof(info));
        err = cue_buildLayout(sCue, sCueSizes, info, starts, total);
    }
    e.cueOk = err == CueError::None;
    if (!e.cueOk)
    {
        snprintf(f.name, sizeof(f.name), "BIN/CUE: can't use");
        if (err == CueError::MissingFile && missing >= 0)
            snprintf(f.detail, sizeof(f.detail), "Missing %.20s", sCue.files[missing]);
        else
            snprintf(f.detail, sizeof(f.detail), "%.29s", cue_errorString(err));
        return;
    }
    u32 audio = cue_audioTracks(info);
    if (audio == info.trackCount)
        snprintf(f.name, sizeof(f.name), "Audio CD (BIN/CUE)");
    else if (audio)
        snprintf(f.name, sizeof(f.name), "Mixed-mode CD (BIN/CUE)");
    else
        snprintf(f.name, sizeof(f.name), "Data CD (BIN/CUE)");
    u32 seconds = info.leadout / 75;
    snprintf(f.detail, sizeof(f.detail), "%lu track%s, %lu:%02lu", info.trackCount,
        info.trackCount == 1 ? "" : "s", seconds / 60, seconds % 60);
    u32 textLen = cueImage_loadCdText(IMAGE_DIR, sCue, sInfoCdText);
    if (textLen)
        cue_cdTextTitle(sInfoCdText, textLen, f.label, sizeof(f.label));
    if (!f.label[0])
        snprintf(f.label, sizeof(f.label), "%.15s", sCue.title);
    f.isIso = true;
}

static void cueLayoutInfo(Entry& e)
{
    // pieces on the SD card, all .bin files together
    e.fragments = 0;
    e.layoutError = MapError::None;
    int missing;
    if (cueImage_load(IMAGE_DIR, e.name, sCue, sCueSizes, missing) != CueError::None)
    {
        e.layoutError = MapError::OpenFailed;
        return;
    }
    for (u32 i = 0; i < sCue.fileCount; i++)
    {
        char path[CUE_NAME_LEN + 32];
        cueImage_path(path, sizeof(path), IMAGE_DIR, sCue.files[i]);
        ImageMap map = {};
        map.roundUp = true;
        MapError err = fatMap_build(path, map);
        e.fragments += map.extentCount;
        if (err != MapError::None && e.layoutError == MapError::None)
            e.layoutError = err;
    }
}

static void scanImages(const char* selectName = nullptr)
{
    char keep[NAME_LEN] = "";
    if (selectName)
        snprintf(keep, sizeof(keep), "%s", selectName);
    else if (sCursor < sEntryCount)
        snprintf(keep, sizeof(keep), "%s", sEntries[sCursor].name);

    sEntryCount = 0;
    sSkippedEntries = 0;
    DIR* dir = opendir(IMAGE_DIR);
    if (dir)
    {
        struct dirent* de;
        char path[NAME_LEN + 16];
        while ((de = readdir(dir)) != nullptr)
        {
            if (de->d_name[0] == '.')
                continue;
            if (strlen(de->d_name) >= NAME_LEN)
            {
                sSkippedEntries++;
                continue;
            }
            entryPath(path, sizeof(path), de->d_name);
            // 64-bit size straight from FatFs (stat's st_size is 32-bit here)
            unsigned long long fileSize;
            int isDir;
            if (fs_fileSize64(path, &fileSize, &isDir) != 0 || isDir)
                continue;
            if (sEntryCount >= MAX_ENTRIES)
            {
                sSkippedEntries++;
                continue;
            }
            Entry& e = sEntries[sEntryCount++];
            memset(&e, 0, sizeof(e));
            snprintf(e.name, sizeof(e.name), "%s", de->d_name);
            e.size = (u64)fileSize;
        }
        closedir(dir);
    }
    hideCueFiles();
    qsort(sEntries, sEntryCount, sizeof(Entry), compareEntries);

    int idx = keep[0] ? findEntry(keep) : -1;
    if (idx >= 0)
        sCursor = idx;
    clampCursor();
    sRestFrames = 0;
}

static bool isMountedEntry(int idx)
{
    return sMounted && !sMountedSd && idx >= 0 && idx < sEntryCount && !strcasecmp(sEntries[idx].name, sMountedName);
}

static void analyzeEntry(Entry& e)
{
    char path[NAME_LEN + 16];
    entryPath(path, sizeof(path), e.name);
    if (e.isCue)
    {
        if (!e.infoValid)
        {
            cueFormat(e);
            e.infoValid = true;
        }
        if (!e.layoutValid)
        {
            cueLayoutInfo(e);
            e.layoutValid = true;
        }
        return;
    }
    if (!e.infoValid)
    {
        imageInfo_detect(path, e.size > 0xFFFFFFFFull ? 0xFFFFFFFFu : (u32)e.size, e.format);
        e.infoValid = true;
    }
    if (!e.layoutValid)
    {
        ImageMap map = { nullptr, 0, 0, 0, 0 };
        e.layoutError = fatMap_build(path, map);
        e.fragments = map.extentCount;
        e.layoutValid = true;
    }
}

// ---------------------------------------------------------------------------
// USB status
// ---------------------------------------------------------------------------

static u32 secondsSince(const mount_status_t& s, u32 frame)
{
    return (s.frame - frame) / 60;
}

enum class Safety
{
    Safe,       // host ejected / not connected / read-only
    Idle,       // no recent writes, but the host never ejected
    Busy        // host recently wrote or has the medium locked
};

static Safety unmountSafety(const mount_status_t& s)
{
    if (!s.mounted || s.hostEjected || s.readOnly)
        return Safety::Safe;
    if (s.usbState == MOUNT_USB_IDLE || s.usbState == MOUNT_USB_WAITING)
        return Safety::Safe; // no host attached right now
    if (s.hostPrevent)
        return Safety::Busy;
    if (s.sectorsWritten == 0 || secondsSince(s, s.lastWriteFrame) >= 3)
        return Safety::Idle;
    return Safety::Busy;
}

static const char* usbStateString(const mount_status_t& s)
{
    switch (s.usbState)
    {
        case MOUNT_USB_IDLE:       return "Off";
        case MOUNT_USB_WAITING:    return s.everConfigured ? "Cable unplugged?" : "Waiting for host";
        case MOUNT_USB_CONFIGURED: return s.hostEjected ? "Host ejected drive" : "Connected";
        case MOUNT_USB_SUSPENDED:  return "Host asleep";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

static void verifyProgress(u32 done, u32 total)
{
    char line[UI_COLS + 1];
    snprintf(line, sizeof(line), "Verifying sector map %lu%%", done * 100 / total);
    showBusy("Mounting...", line);
}

static void doMount(int idx, bool readOnly, MountMediaType media)
{
    Entry& e = sEntries[idx];
    char path[NAME_LEN + 16];
    entryPath(path, sizeof(path), e.name);

    showBusy("Mounting...", "Flushing SD card cache");
    if (!sd_remount())
    {
        showMessage(UI_RED, "SD card remount failed.");
        return;
    }

    showBusy("Mounting...", "Mapping image on SD card");
    ImageMap map = { sExtents, MOUNT_MAX_EXTENTS, 0, 0, 0 };
    MapError err = fatMap_build(path, map);
    if (err != MapError::None)
    {
        if (err == MapError::TooFragmented)
            showMessage(UI_RED, "Can't mount: file is in too", "many pieces on the SD card.", "Copy it off and back on, or", "defragment the card.");
        else
            showMessage(UI_RED, "Can't mount:", mapErrorString(err));
        e.layoutValid = false;
        return;
    }

    showBusy("Mounting...", "Verifying sector map");
    err = fatMap_verify(path, map, !readOnly, verifyProgress);
    if (err != MapError::None)
    {
        showMessage(UI_RED, "Can't mount: sector map", "check failed. Nothing was", "written. Please report this.", mapErrorString(err));
        return;
    }

    if (media == MOUNT_MEDIA_CDROM && map.totalSectors < 4)
    {
        showMessage(UI_RED, "Too small for a disc image.");
        return;
    }
    u32 result = mountClient_mount(sExtents, map.extentCount, map.totalSectors, media, readOnly);
    if (result != MOUNT_RESULT_OK)
    {
        char line[UI_COLS + 1];
        snprintf(line, sizeof(line), "ARM7 refused (code %lu)", result);
        showMessage(UI_RED, "Can't mount:", line);
        return;
    }

    sMounted = true;
    sMountedReadOnly = readOnly;
    sMountedMedia = media;
    sMountedSectors = map.totalSectors;
    sMountedCdBlocks = map.totalSectors / 4;
    sMountedFragments = map.extentCount;
    snprintf(sMountedName, sizeof(sMountedName), "%s", e.name);
    e.fragments = map.extentCount;
    e.layoutError = MapError::None;
    e.layoutValid = true;
    sMode = Mode::Browse;
}

/// Mounts a .cue: maps and checks every .bin, then serves them as one CD.
static void doMountCue(int idx)
{
    Entry& e = sEntries[idx];
    showBusy("Mounting...", "Flushing SD card cache");
    if (!sd_remount())
    {
        showMessage(UI_RED, "SD card remount failed.");
        return;
    }
    showBusy("Mounting...", "Reading the cue sheet");
    int missing;
    CueError cerr = cueImage_load(IMAGE_DIR, e.name, sCue, sCueSizes, missing);
    u32 starts[CUE_MAX_FILES], total = 0;
    if (cerr == CueError::None)
    {
        memset(&sCueLayout, 0, sizeof(sCueLayout));
        sCueLayout.bounce = sCueBounce;
        sCueLayout.bounceSize = sizeof(sCueBounce);
        u32 textLen = cueImage_loadCdText(IMAGE_DIR, sCue, sCueCdText);
        sCueLayout.cdText = textLen ? sCueCdText : nullptr;
        sCueLayout.cdTextLength = textLen;
        cerr = cue_buildLayout(sCue, sCueSizes, sCueLayout, starts, total);
    }
    if (cerr != CueError::None)
    {
        if (cerr == CueError::MissingFile && missing >= 0)
            showMessage(UI_RED, "Can't mount: missing file", sCue.files[missing]);
        else
            showMessage(UI_RED, "Can't mount:", cue_errorString(cerr));
        e.infoValid = false;
        return;
    }

    // map every .bin into one extent table, each at its place in the
    // virtual image (see cdLayout.h)
    u32 used = 0;
    for (u32 i = 0; i < sCue.fileCount; i++)
    {
        char path[CUE_NAME_LEN + 32];
        cueImage_path(path, sizeof(path), IMAGE_DIR, sCue.files[i]);
        char line[UI_COLS + 1];
        snprintf(line, sizeof(line), "Mapping file %lu of %lu", i + 1, sCue.fileCount);
        showBusy("Mounting...", line);
        ImageMap map = {};
        map.extents = sExtents + used;
        map.maxExtents = MOUNT_MAX_EXTENTS - used;
        map.roundUp = true;
        MapError err = fatMap_build(path, map);
        if (err == MapError::None && map.totalSectors != (u32)((sCueSizes[i] + 511) / 512))
            err = MapError::VerifyFailed;
        if (err == MapError::None)
        {
            // check against the filesystem (whole sectors only: the partial
            // last one can't be read back as a full sector)
            ImageMap check = map;
            check.totalSectors = (u32)(sCueSizes[i] / 512);
            if (check.totalSectors > 0)
            {
                showBusy("Mounting...", "Verifying sector map");
                err = fatMap_verify(path, check, false, verifyProgress);
            }
        }
        if (err != MapError::None)
        {
            if (err == MapError::TooFragmented)
                showMessage(UI_RED, "Can't mount: the .bin files", "are in too many pieces on", "the SD card.");
            else
                showMessage(UI_RED, "Can't mount:", sCue.files[i], mapErrorString(err));
            return;
        }
        for (u32 x = 0; x < map.extentCount; x++)
            sExtents[used + x].fileSector += starts[i];
        used += map.extentCount;
    }

    u32 result = mountClient_mount(sExtents, used, total, MOUNT_MEDIA_CDROM, true, &sCueLayout);
    if (result != MOUNT_RESULT_OK)
    {
        char line[UI_COLS + 1];
        snprintf(line, sizeof(line), "ARM7 refused (code %lu)", result);
        showMessage(UI_RED, "Can't mount:", line);
        return;
    }
    sMounted = true;
    sMountedReadOnly = true;
    sMountedMedia = MOUNT_MEDIA_CDROM;
    sMountedSectors = total;
    sMountedCdBlocks = sCueLayout.leadout;
    sMountedFragments = used;
    snprintf(sMountedName, sizeof(sMountedName), "%s", e.name);
    e.fragments = used;
    e.layoutError = MapError::None;
    e.layoutValid = true;
    sMode = Mode::Browse;
}

static void doMountSd()
{
    showBusy("Sharing SD card...", "Flushing and releasing it");
    // The PC gets the raw card: libfat must let go of it completely first
    // (flushes its cache); nothing on the DS touches the card while shared.
    if (!sd_unmount())
    {
        sd_mount();
        showMessage(UI_RED, "Couldn't release the SD card.", "Nothing was shared.");
        return;
    }
    u32 sectors = fatVolume_cardSectors(sCardSector);
    if (sectors == 0)
    {
        sd_mount();
        showMessage(UI_RED, "Couldn't work out the SD card", "size from its partition table.");
        return;
    }
    sSdExtent[0] = { 0, 0, sectors }; // identity map: LBA N = SD sector N
    u32 result = mountClient_mount(sSdExtent, 1, sectors, MOUNT_MEDIA_DISK, false);
    if (result != MOUNT_RESULT_OK)
    {
        sd_mount();
        char line[UI_COLS + 1];
        snprintf(line, sizeof(line), "ARM7 refused (code %lu)", result);
        showMessage(UI_RED, "Can't share the SD card:", line);
        return;
    }
    sMounted = true;
    sMountedSd = true;
    sMountedReadOnly = false;
    sMountedMedia = MOUNT_MEDIA_DISK;
    sMountedSectors = sectors;
    sMountedFragments = 1;
    snprintf(sMountedName, sizeof(sMountedName), "Whole SD card");
    sMode = Mode::Browse;
}

static void doUnmount()
{
    showBusy("Unmounting...", "Detaching from USB");
    mountClient_unmount();
    sMounted = false;

    if (sMountedSd)
    {
        // Take the card back: the PC may have changed anything on it.
        sMountedSd = false;
        sMountedName[0] = 0;
        showBusy("Unmounting...", "Reading the SD card again");
        if (!sd_mount())
        {
            snprintf(sMsg[0], sizeof(sMsg[0]), "Couldn't read the SD card.");
            snprintf(sMsg[1], sizeof(sMsg[1]), "Check it on the PC (chkdsk),");
            snprintf(sMsg[2], sizeof(sMsg[2]), "then restart the DS.");
            sMsg[3][0] = 0;
            sMode = Mode::Fatal;
            return;
        }
        mkdir(IMAGE_DIR, 0777);
        scanImages();
        showMessage(UI_GREEN, "SD card is back on the DS.", "Image list rescanned.");
        return;
    }

    // The host may have changed the image (e.g. formatted it): drop all
    // cached data and re-detect it.
    showBusy("Unmounting...", "Flushing SD card cache");
    sd_remount();
    int idx = findEntry(sMountedName);
    if (idx >= 0)
    {
        sEntries[idx].infoValid = false;
        sEntries[idx].layoutValid = false;
    }
    sMountedName[0] = 0;
    showMessage(UI_GREEN, "Image unmounted.", "All data written by the host", "is on the SD card.");
}

static void doDelete(int idx)
{
    char path[NAME_LEN + 16];
    entryPath(path, sizeof(path), sEntries[idx].name);
    showBusy("Deleting...", sEntries[idx].name);
    int rc = 0;
    if (sEntries[idx].isCue && cueImage_parse(IMAGE_DIR, sEntries[idx].name, sCue) == CueError::None)
    {
        // its .bin and .cdt files go too (only those next to the .cue)
        for (u32 f = 0; f <= sCue.fileCount; f++)
        {
            const char* ref = f < sCue.fileCount ? sCue.files[f] : sCue.cdTextFile;
            if (!cueImage_isLocalName(ref))
                continue;
            char refPath[CUE_NAME_LEN + 32];
            cueImage_path(refPath, sizeof(refPath), IMAGE_DIR, ref);
            struct stat st;
            if (stat(refPath, &st) == 0 && unlink(refPath) != 0)
                rc = -1;
        }
    }
    if (unlink(path) != 0)
        rc = -1;
    sd_remount();
    scanImages();
    if (rc != 0)
        showMessage(UI_RED, "Delete failed.");
    else
        sMode = Mode::Browse;
}

static u64 queryFreeBytes()
{
    struct statvfs v;
    if (statvfs("sd:/", &v) != 0)
        return 0;
    return (u64)v.f_bfree * (u64)v.f_bsize;
}

static void defaultNewName()
{
    char candidate[NAME_LEN];
    int n = 1;
    for (; n < 100; n++)
    {
        snprintf(candidate, sizeof(candidate), "IMAGE_%02d.img", n);
        if (findEntry(candidate) < 0)
            break;
    }
    char base[NAME_EDIT_LEN + 1];
    snprintf(base, sizeof(base), "IMAGE_%02d", n);
    memset(sNewName, ' ', NAME_EDIT_LEN);
    memcpy(sNewName, base, strlen(base));
    sNewName[NAME_EDIT_LEN] = 0;
    sNameCursor = strlen(base) < NAME_EDIT_LEN ? strlen(base) : NAME_EDIT_LEN - 1;
}

static bool buildNewFileName(char* out, size_t outSize)
{
    char base[NAME_EDIT_LEN + 1];
    memcpy(base, sNewName, NAME_EDIT_LEN);
    base[NAME_EDIT_LEN] = 0;
    int len = NAME_EDIT_LEN;
    while (len > 0 && base[len - 1] == ' ')
        len--;
    base[len] = 0;
    if (len == 0)
        return false;
    for (int i = 0; i < len; i++)
        if (base[i] == ' ')
            base[i] = '_';
    snprintf(out, outSize, "%s.img", base);
    return true;
}

static void drawCreateProgress(const char* name, u64 done, u64 total, u32 startFrame)
{
    const mount_status_t& s = mountClient_status();
    u32 frames = s.frame - startFrame;
    char doneStr[12], totalStr[12];
    ui_formatSize(doneStr, done);
    ui_formatSize(totalStr, total);
    u32 pct = total ? (u32)(done * 100 / total) : 100;

    ui_clear(UI_BOTTOM);
    ui_line(UI_BOTTOM, 0, UI_CYAN, " Creating image");
    ui_line(UI_BOTTOM, 1, UI_GREY, "--------------------------------");
    ui_line(UI_BOTTOM, 3, UI_WHITE, " %.30s", name);
    ui_line(UI_BOTTOM, 5, UI_WHITE, " %s of %s  (%lu%%)", doneStr, totalStr, pct);

    char bar[31];
    u32 filled = pct * 30 / 100;
    for (u32 i = 0; i < 30; i++)
        bar[i] = i < filled ? '#' : '.';
    bar[30] = 0;
    ui_line(UI_BOTTOM, 7, UI_GREEN, " %s", bar);

    if (frames >= 60 && done > 0)
    {
        u64 bytesPerSec = done * 60 / frames;
        u32 eta = bytesPerSec ? (u32)((total - done) / bytesPerSec) : 0;
        char speed[12];
        ui_formatSize(speed, bytesPerSec);
        ui_line(UI_BOTTOM, 9, UI_WHITE, " %s/s, about %lum %02lus left", speed, eta / 60, eta % 60);
    }
    ui_line(UI_BOTTOM, 12, UI_GREY, " Space is reserved by writing");
    ui_line(UI_BOTTOM, 13, UI_GREY, " zeros, so the image starts");
    ui_line(UI_BOTTOM, 14, UI_GREY, " blank and in one piece.");
    ui_line(UI_BOTTOM, 21, UI_YELLOW, " B: cancel");
    ui_flush();
}

static u32 sQuickStartFrame;

static void quickProgress(const char* stage, u32 done, u32 total)
{
    mountClient_poll();
    const mount_status_t& s = mountClient_status();
    u32 pct = total ? (u32)((u64)done * 100 / total) : 100;
    ui_clear(UI_BOTTOM);
    ui_line(UI_BOTTOM, 0, UI_CYAN, " Quick create");
    ui_line(UI_BOTTOM, 1, UI_GREY, "--------------------------------");
    ui_line(UI_BOTTOM, 4, UI_WHITE, " %s...", stage);
    char bar[31];
    u32 filled = pct * 30 / 100;
    for (u32 i = 0; i < 30; i++)
        bar[i] = i < filled ? '#' : '.';
    bar[30] = 0;
    ui_line(UI_BOTTOM, 6, UI_GREEN, " %s", bar);
    ui_line(UI_BOTTOM, 8, UI_GREY, " %lus", (s.frame - sQuickStartFrame) / 60);
    ui_line(UI_BOTTOM, 12, UI_YELLOW, " Don't turn the DS off.");
    ui_flush();
}

static void doCreateQuick(const char* name, u64 total)
{
    char path[NAME_LEN + 16];
    entryPath(path, sizeof(path), name);
    mountClient_poll();
    sQuickStartFrame = mountClient_status().frame;
    quickProgress("Preparing", 0, 1);

    AllocResult result;
    AllocError err = quickAlloc_create(IMAGE_DIR, path, total, result, quickProgress);
    scanImages(name);
    if (err != AllocError::None)
    {
        if (err == AllocError::TooFragmented || err == AllocError::NoSpace || err == AllocError::Unsupported)
            showMessage(UI_RED, "Quick create failed:", allocErrorString(err), "Full mode (Y) may still work.");
        else
            showMessage(UI_RED, "Quick create failed:", allocErrorString(err), "Nothing was left behind.");
        return;
    }

    mountClient_poll();
    char line1[UI_COLS + 1], line2[UI_COLS + 1];
    snprintf(line1, sizeof(line1), "Done in %lus.", (mountClient_status().frame - sQuickStartFrame) / 60);
    if (result.runs == 1)
        snprintf(line2, sizeof(line2), "It is in one piece on the SD.");
    else
        snprintf(line2, sizeof(line2), "It is in %lu pieces on the SD.", result.runs);
    showMessage(UI_GREEN, "Image created.", line1, line2, "Mount it with A, then format.");
}

static void doCreate()
{
    char name[NAME_LEN];
    if (!buildNewFileName(name, sizeof(name)))
    {
        showMessage(UI_RED, "Please enter a name.");
        return;
    }
    if (findEntry(name) >= 0)
    {
        showMessage(UI_RED, "An image with that name", "already exists.");
        return;
    }

    u64 total = presetBytes(sPresetIndex);
    if (total > sd_maxImageBytes())
    {
        const char* fs = sd_fsTypeName();
        char line[48];
        snprintf(line, sizeof(line), "%s limit: %lu MB per file.", fs ? fs : "Card", maxImageMiB());
        showMessage(UI_RED, "Too big for this SD card.", line, "An exFAT card allows more.");
        return;
    }
    if (sFreeBytes && total + MiB > sFreeBytes)
    {
        char need[12], have[12], line[48];
        ui_formatSize(need, total);
        ui_formatSize(have, sFreeBytes);
        snprintf(line, sizeof(line), "Need %s, %s free.", need, have);
        showMessage(UI_RED, "Not enough free space.", line);
        return;
    }

    if (sCreateQuick)
    {
        doCreateQuick(name, total);
        return;
    }

    char path[NAME_LEN + 16];
    entryPath(path, sizeof(path), name);
    // Plain write() calls (no stdio): sequential writes never need a file
    // offset, which is only 32-bit (2 GiB) in BlocksDS's C library.
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
    {
        showMessage(UI_RED, "Couldn't create the file.");
        return;
    }

    mountClient_poll();
    u32 startFrame = mountClient_status().frame;
    u64 done = 0;
    bool failed = false;
    bool cancelled = false;
    u32 chunks = 0;
    drawCreateProgress(name, 0, total, startFrame);

    while (done < total)
    {
        u32 chunk = (total - done) > sizeof(sZeroBuffer) ? sizeof(sZeroBuffer) : (u32)(total - done);
        if (write(fd, sZeroBuffer, chunk) != (ssize_t)chunk)
        {
            failed = true;
            break;
        }
        done += chunk;

        if ((++chunks & 7) == 0)
        {
            mountClient_poll();
            Buttons b = ui_readButtons(mountClient_status().keyXY);
            if (b.pressed & BTN_B)
            {
                cancelled = true;
                break;
            }
            drawCreateProgress(name, done, total, startFrame);
        }
    }

    if (close(fd) != 0)
        failed = true;
    if (failed || cancelled)
        unlink(path);
    sd_remount();
    scanImages(name);

    if (failed)
        showMessage(UI_RED, "Writing the image failed.", "The partial file was removed.");
    else if (cancelled)
        showMessage(UI_YELLOW, "Cancelled.", "The partial file was removed.");
    else
        showMessage(UI_GREEN, "Image created.", "Mount it with A, then format", "it on the PC or Xbox.");
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

static void drawStuckScreen()
{
    static mount_status_t s;
    mountClient_peekStatus(&s);
    static const char* const kStates[] = { "not started", "waiting", "dispatching", "in Wifi_Update" };
    ui_clear(UI_TOP);
    ui_line(UI_TOP, 0, UI_RED, " WiFi seems stuck (%lus)", (unsigned long)(sStuckFrames / 60));
    ui_line(UI_TOP, 1, UI_GREY, "--------------------------------");
    ui_line(UI_TOP, 3, UI_YELLOW, " Step:");
    static const char* const kNet[] = { "Off", "Starting", "Scanning", "Connecting", "Connected", "Failed" };
    unsigned ns = (unsigned)wifiNet_state();
    ui_line(UI_TOP, 2, UI_GREY, " WiFi state: %s", ns < 6 ? kNet[ns] : "?");
    ui_line(UI_TOP, 4, UI_WHITE, " %.31s", gWifiStep ? (const char*)gWifiStep : sInWaitFrame ? "frame wait (WiFi stack busy?)" : "(outside WiFi code)");
    ui_line(UI_TOP, 5, UI_GREY, " %.31s", wifiNet_status());
    ui_line(UI_TOP, 6, UI_YELLOW, " ARM9 got from ARM7:");
    ui_line(UI_TOP, 7, UI_WHITE, "  %lu msgs, %lu done, last %03lx",
        (unsigned long)ndsFifoShim_received(), (unsigned long)ndsFifoShim_dispatched(),
        (unsigned long)ndsFifoShim_last());
    ui_line(UI_TOP, 8, UI_WHITE, "  queued to send %lu, lost %lu",
        (unsigned long)ndsFifoShim_outboxCount(), (unsigned long)ndsFifoShim_outboxDropped());
    ui_line(UI_TOP, 9, UI_YELLOW, " ARM7 got from ARM9:");
    ui_line(UI_TOP, 10, UI_WHITE, "  %lu msgs, %lu done, last %03lx",
        (unsigned long)s.wifiFifoRx, (unsigned long)s.wifiFifoDone, (unsigned long)s.wifiFifoLast);
    ui_line(UI_TOP, 11, UI_WHITE, "  WiFi thread: %s",
        s.wifiThreadState < 4 ? kStates[s.wifiThreadState] : "?");
    ui_line(UI_TOP, 12, UI_WHITE, "  Wifi_Update calls: %lu", (unsigned long)s.wifiUpdates);
    static u32 sLastArm7Frame, sLastArm7Updates;
    bool arm7Alive = s.frame != sLastArm7Frame;
    bool wifiAlive = s.wifiUpdates != sLastArm7Updates;
    sLastArm7Frame = s.frame;
    sLastArm7Updates = s.wifiUpdates;
    ui_line(UI_TOP, 13, arm7Alive ? UI_WHITE : UI_RED, "  ARM7 frame: %lu (%s)",
        (unsigned long)s.frame, arm7Alive ? "running" : "STOPPED");
    ui_line(UI_TOP, 14, wifiAlive ? UI_WHITE : UI_RED, "  ARM7 WiFi: %s",
        wifiAlive ? "running" : "not updating");
    ui_line(UI_TOP, 15, UI_WHITE, "  ARM7 main: %s  IRQ %lx/%lx",
        s.arm7Step == 1 ? "vblank wait" : s.arm7Step == 2 ? "working" : "?",
        (unsigned long)s.arm7IrqIn, (unsigned long)s.arm7Irq2In);
    ui_line(UI_TOP, 17, UI_GREY, " Please send a photo of this.");
    ui_flushDirect();
}

static void drawWifiLine()
{
    WifiNetState ws = wifiNet_state();
    if (ws == WifiNetState::Off)
        return;
    if (ws == WifiNetState::Connected && webApp_running())
    {
        char ip[16];
        wifiNet_ipString(wifiNet_ip(), ip, sizeof(ip));
        ui_line(UI_TOP, 23, UI_GREEN, " http://%s  PIN %s", ip, webApp_pin());
    }
    else
        ui_line(UI_TOP, 23, ws == WifiNetState::Failed ? UI_YELLOW : UI_GREY, " WiFi: %.25s", wifiNet_status());
}

static void drawTop()
{
    const mount_status_t& s = mountClient_status();
    ui_clear(UI_TOP);
#ifdef IMAGE_MOUNTER_EXFAT
    {
        const char* fs = sd_fsTypeName();
        ui_line(UI_TOP, 0, UI_CYAN, " " APP_NAME " v" APP_VERSION "       SD:%-6s", fs ? fs : "?");
    }
#else
    ui_line(UI_TOP, 0, UI_CYAN, " " APP_NAME "                v" APP_VERSION);
#endif
    ui_line(UI_TOP, 1, UI_GREY, "--------------------------------");

    if (sMountedSd)
    {
        ui_line(UI_TOP, 3, UI_YELLOW, " Whole SD card shared over USB");
        ui_line(UI_TOP, 5, UI_GREY, " The PC has the card. Browsing");
        ui_line(UI_TOP, 6, UI_GREY, " is paused until you unmount.");
    }
    else if (sEntryCount > 0)
    {
        Entry& e = sEntries[sCursor];
        ui_line(UI_TOP, 2, UI_YELLOW, " %.31s", e.name);
        if (strlen(e.name) > 31)
            ui_line(UI_TOP, 3, UI_YELLOW, " %.31s", e.name + 31);

        char size[12];
        ui_formatSize(size, e.size);
        ui_line(UI_TOP, 5, UI_WHITE, " Size   %s (%lu sectors)", size, (u32)(e.size / MOUNT_SECTOR_SIZE));
        if (e.size % MOUNT_SECTOR_SIZE)
            ui_line(UI_TOP, 6, UI_YELLOW, "        last %lu bytes ignored", (u32)(e.size % MOUNT_SECTOR_SIZE));

        if (!e.infoValid)
        {
            ui_line(UI_TOP, 7, UI_GREY, " Format ...");
        }
        else
        {
            ui_line(UI_TOP, 7, UI_WHITE, " Format %.24s", e.format.name);
            if (e.format.detail[0])
                ui_line(UI_TOP, 8, UI_GREY, "        %.24s", e.format.detail);
            if (e.format.label[0])
                ui_line(UI_TOP, 9, UI_WHITE, " Label  %.24s", e.format.label);
        }

        if (!e.layoutValid)
            ui_line(UI_TOP, 10, UI_GREY, " Layout ...");
        else if (e.layoutError != MapError::None)
            ui_line(UI_TOP, 10, UI_RED, " Layout %.24s", mapErrorString(e.layoutError));
        else if (e.fragments == 1)
            ui_line(UI_TOP, 10, UI_GREEN, " Layout contiguous");
        else if (e.fragments > MOUNT_MAX_EXTENTS)
            ui_line(UI_TOP, 10, UI_RED, " Layout %lu pieces (too many)", e.fragments);
        else
            ui_line(UI_TOP, 10, UI_WHITE, " Layout %lu pieces on SD", e.fragments);

        if (e.infoValid && e.isCue && e.cueOk)
            ui_line(UI_TOP, 11, UI_YELLOW, " A: mount as a CD drive");
        else if (e.infoValid && e.format.isIso && !e.isCue)
            ui_line(UI_TOP, 11, UI_YELLOW, " A: %s drive  L+A: as disk", e.size > 360000u * 2048 ? "DVD" : "CD");
    }
    else
    {
        ui_line(UI_TOP, 3, UI_GREY, " No image selected.");
    }

    ui_line(UI_TOP, 13, UI_GREY, "------------- USB --------------");
    if (!sMounted)
    {
        ui_line(UI_TOP, 14, UI_WHITE, " Nothing mounted");
        ui_line(UI_TOP, 16, UI_GREY, " Select an image and press A.");
        ui_line(UI_TOP, 17, UI_GREY, " The DS then appears on the");
        ui_line(UI_TOP, 18, UI_GREY, " PC/Xbox as a USB drive.");
    }
    else
    {
        ui_line(UI_TOP, 14, UI_GREEN, " %.31s", sMountedName);
        char size[12];
        ui_formatSize(size, (u64)sMountedSectors * MOUNT_SECTOR_SIZE);
        if (sMountedMedia == MOUNT_MEDIA_CDROM)
            ui_line(UI_TOP, 15, UI_WHITE, " %s, as %s drive", size,
                sMountedCdBlocks > 360000 ? "DVD-ROM" : "CD-ROM");
        else
            ui_line(UI_TOP, 15, UI_WHITE, " %s, %s", size, sMountedReadOnly ? "read-only" : "read/write");
        ui_line(UI_TOP, 16, UI_WHITE, " Host   %s", usbStateString(s));
        char rd[12], wr[12];
        ui_formatSize(rd, (u64)s.sectorsRead * MOUNT_SECTOR_SIZE);
        ui_formatSize(wr, (u64)s.sectorsWritten * MOUNT_SECTOR_SIZE);
        ui_line(UI_TOP, 17, UI_WHITE, " Read   %-8s Written %s", rd, wr);
        if (s.sectorsWritten)
            ui_line(UI_TOP, 18, UI_WHITE, " Last write %lus ago", secondsSince(s, s.lastWriteFrame));
        else if (sReadRateKBs)
            ui_line(UI_TOP, 18, UI_WHITE, " Reading at %lu KB/s", sReadRateKBs);
        if (s.scsiRefused)
            ui_line(UI_TOP, 12, UI_YELLOW, " Refused %lux, last %02lX %lX/%02lX/%02lX", s.scsiRefused,
                s.lastRefusedOp, s.lastRefusedSense >> 16, (s.lastRefusedSense >> 8) & 0xFF,
                s.lastRefusedSense & 0xFF);
        if (s.ioErrors)
            ui_line(UI_TOP, 19, UI_RED, " SD errors: %lu", s.ioErrors);
        if (s.usbBadWrites || s.usbSpurious)
            ui_line(UI_TOP, 20, UI_YELLOW, " USB glitches caught: %lu/%lu", s.usbBadWrites, s.usbSpurious);

        switch (unmountSafety(s))
        {
            case Safety::Safe:
                ui_line(UI_TOP, 21, UI_GREEN, " Safe to unmount (B)");
                if (s.hostEjected)
                    ui_line(UI_TOP, 22, UI_WHITE, " X: put it back (reinsert)");
                break;
            case Safety::Idle:
                ui_line(UI_TOP, 21, UI_YELLOW, " Idle. Eject on the host first");
                ui_line(UI_TOP, 22, UI_YELLOW, " if you can, then press B.");
                break;
            case Safety::Busy:
                ui_line(UI_TOP, 21, UI_RED, " Host is using the drive.");
                ui_line(UI_TOP, 22, UI_RED, " Eject it on the host first.");
                break;
        }
    }
}

static void drawBrowse()
{
    ui_clear(UI_BOTTOM);
    if (sMountedSd)
    {
        ui_line(UI_BOTTOM, 0, UI_CYAN, " Whole SD card");
        ui_line(UI_BOTTOM, 1, UI_GREY, "--------------------------------");
        ui_line(UI_BOTTOM, 6, UI_WHITE, " The SD card is shared with the");
        ui_line(UI_BOTTOM, 7, UI_WHITE, " PC. Copy files as usual, then");
        ui_line(UI_BOTTOM, 8, UI_WHITE, " Eject it on the PC and press B.");
        ui_line(UI_BOTTOM, 10, UI_GREY, " Don't remove the SD card or");
        ui_line(UI_BOTTOM, 11, UI_GREY, " turn the DS off while shared.");
        ui_line(UI_BOTTOM, 20, UI_GREY, "--------------------------------");
        ui_line(UI_BOTTOM, 21, UI_WHITE, " B: unmount");
        return;
    }
    ui_line(UI_BOTTOM, 0, UI_CYAN, " sd:/images/ %*d image%s", 14, sEntryCount, sEntryCount == 1 ? " " : "s");
    ui_line(UI_BOTTOM, 1, UI_GREY, "--------------------------------");

    if (sEntryCount == 0)
    {
        ui_line(UI_BOTTOM, 4, UI_WHITE, " No images yet.");
        ui_line(UI_BOTTOM, 6, UI_GREY, " Press X to create one, or copy");
        ui_line(UI_BOTTOM, 7, UI_GREY, " image files into the /images");
        ui_line(UI_BOTTOM, 8, UI_GREY, " folder on the SD card.");
    }
    for (int row = 0; row < LIST_ROWS; row++)
    {
        int idx = sScroll + row;
        if (idx >= sEntryCount)
            break;
        const Entry& e = sEntries[idx];
        char size[12];
        ui_formatSize(size, e.size);
        bool sel = idx == sCursor;
        bool mnt = isMountedEntry(idx);
        UiColor color = mnt ? UI_GREEN : (sel ? UI_YELLOW : UI_WHITE);
        ui_line(UI_BOTTOM, LIST_FIRST_ROW + row, color, "%c%c%-23.23s %6s",
            sel ? '>' : ' ', mnt ? '*' : ' ', e.name, size);
    }
    if (sSkippedEntries)
        ui_line(UI_BOTTOM, 19, UI_YELLOW, " (%d file%s not shown)", sSkippedEntries, sSkippedEntries == 1 ? "" : "s");

    ui_line(UI_BOTTOM, 20, UI_GREY, "--------------------------------");
    if (sMounted)
    {
        ui_line(UI_BOTTOM, 21, UI_WHITE, " B: unmount   Y: refresh");
        ui_line(UI_BOTTOM, 22, UI_GREY, " (unmount to create/delete)");
    }
    else
    {
        ui_line(UI_BOTTOM, 21, UI_WHITE, " A: mount  L+A: R/O  R+A: as DVD");
        ui_line(UI_BOTTOM, 22, UI_WHITE, " X: new  SELECT: del  Y: scan");
        ui_line(UI_BOTTOM, 23, UI_GREY, " START: menu (share whole SD)");
    }
    if (sMounted)
        ui_line(UI_BOTTOM, 23, UI_GREEN, " * = mounted on USB");
}

static void drawMessage()
{
    ui_clear(UI_BOTTOM);
    for (int i = 0; i < 4; i++)
        ui_line(UI_BOTTOM, 8 + i, i == 0 ? sMsgColor : UI_WHITE, " %s", sMsg[i]);
    ui_line(UI_BOTTOM, 21, UI_WHITE, " A: OK");
}

static void drawConfirmUnmount()
{
    const mount_status_t& s = mountClient_status();
    ui_clear(UI_BOTTOM);
    ui_line(UI_BOTTOM, 2, UI_RED, " Unmount without host eject?");
    ui_line(UI_BOTTOM, 4, UI_WHITE, " The host never ejected the");
    ui_line(UI_BOTTOM, 5, UI_WHITE, " drive. If it still has unsaved");
    ui_line(UI_BOTTOM, 6, UI_WHITE, " changes cached, they're lost.");
    ui_line(UI_BOTTOM, 8, UI_GREY, " PC: use Eject / Safely Remove.");
    ui_line(UI_BOTTOM, 9, UI_GREY, " Xbox: wait until the dashboard");
    ui_line(UI_BOTTOM, 10, UI_GREY, " or game has finished saving.");
    if (s.sectorsWritten)
        ui_line(UI_BOTTOM, 12, UI_WHITE, " Last write: %lus ago", secondsSince(s, s.lastWriteFrame));
    else
        ui_line(UI_BOTTOM, 12, UI_WHITE, " Nothing written this session.");
    if (s.hostPrevent)
        ui_line(UI_BOTTOM, 13, UI_RED, " Host has locked the drive.");
    ui_line(UI_BOTTOM, 20, UI_GREY, "--------------------------------");
    ui_line(UI_BOTTOM, 21, UI_YELLOW, " A: unmount anyway");
    ui_line(UI_BOTTOM, 22, UI_WHITE, " B: cancel");
}

static void drawConfirmDelete()
{
    ui_clear(UI_BOTTOM);
    ui_line(UI_BOTTOM, 6, UI_RED, " Delete this image?");
    ui_line(UI_BOTTOM, 8, UI_WHITE, " %.30s", sEntries[sCursor].name);
    if (sEntries[sCursor].isCue)
        ui_line(UI_BOTTOM, 9, UI_YELLOW, " and the .bin/.cdt files it uses");
    ui_line(UI_BOTTOM, 10, UI_GREY, " This can't be undone.");
    ui_line(UI_BOTTOM, 21, UI_YELLOW, " A: delete");
    ui_line(UI_BOTTOM, 22, UI_WHITE, " B: cancel");
}

static void drawCreateMode(int row)
{
    if (sCreateQuick)
    {
        ui_line(UI_BOTTOM, row, UI_GREEN, " Mode: QUICK (seconds)");
        ui_line(UI_BOTTOM, row + 1, UI_GREY, " Reserves space; clears only the");
        ui_line(UI_BOTTOM, row + 2, UI_GREY, " first and last 1 MB.");
    }
    else
    {
        ui_line(UI_BOTTOM, row, UI_YELLOW, " Mode: FULL (writes all zeros)");
        ui_line(UI_BOTTOM, row + 1, UI_GREY, " Slow (~0.7 MB/s), but every");
        ui_line(UI_BOTTOM, row + 2, UI_GREY, " byte of the image is blank.");
    }
}

static void drawCreateSize()
{
    ui_clear(UI_BOTTOM);
    ui_line(UI_BOTTOM, 0, UI_CYAN, " New image: choose a size");
    ui_line(UI_BOTTOM, 1, UI_GREY, "--------------------------------");
    for (int i = 0; i < PRESET_COUNT; i++)
    {
        bool sel = i == sPresetIndex;
        if (sPresets[i].mib == 0)
            ui_line(UI_BOTTOM, 2 + i, sel ? UI_YELLOW : UI_WHITE, "%c Custom: %lu MB", sel ? '>' : ' ', sCustomMiB);
        else
            ui_line(UI_BOTTOM, 2 + i, sel ? UI_YELLOW : UI_WHITE, "%c %s", sel ? '>' : ' ', sPresets[i].label);
    }
    if (sPresets[sPresetIndex].mib == 0)
        ui_line(UI_BOTTOM, 13, UI_GREY, " Left/Right: 1 MB  L/R: 64 MB");
    char free[12];
    ui_formatSize(free, sFreeBytes);
    ui_line(UI_BOTTOM, 14, UI_WHITE, " Free on SD card: %s", free);
    drawCreateMode(16);
    ui_line(UI_BOTTOM, 20, UI_GREY, "--------------------------------");
    ui_line(UI_BOTTOM, 21, UI_WHITE, " A: next  B: cancel  Y: mode");
}

static void drawCreateName()
{
    ui_clear(UI_BOTTOM);
    ui_line(UI_BOTTOM, 0, UI_CYAN, " New image: name");
    ui_line(UI_BOTTOM, 1, UI_GREY, "--------------------------------");
    char shown[NAME_EDIT_LEN + 1];
    for (int i = 0; i < NAME_EDIT_LEN; i++)
        shown[i] = sNewName[i] == ' ' ? '_' : sNewName[i];
    shown[NAME_EDIT_LEN] = 0;
    ui_line(UI_BOTTOM, 5, UI_YELLOW, "   %s.img", shown);
    char caret[UI_COLS + 1];
    memset(caret, ' ', sizeof(caret));
    caret[3 + sNameCursor] = '^';
    caret[4 + sNameCursor] = 0;
    ui_line(UI_BOTTOM, 6, UI_YELLOW, "%s", caret);

    char size[12];
    ui_formatSize(size, presetBytes(sPresetIndex));
    ui_line(UI_BOTTOM, 9, UI_WHITE, " Size: %s", size);
    ui_line(UI_BOTTOM, 11, UI_GREY, " Up/Down: change letter");
    ui_line(UI_BOTTOM, 12, UI_GREY, " Left/Right: move");
    ui_line(UI_BOTTOM, 13, UI_GREY, " '_' at the end is trimmed.");
    drawCreateMode(16);
    ui_line(UI_BOTTOM, 20, UI_GREY, "--------------------------------");
    ui_line(UI_BOTTOM, 21, UI_WHITE, " A: create  B: back  Y: mode");
}

enum MenuItem
{
    MENU_SHARE_SD,
    MENU_WIFI,
    MENU_RETURN,
    MENU_BACK,
    MENU_COUNT
};

static const char* menuLabel(int i)
{
    switch (i)
    {
        case MENU_SHARE_SD: return "Share whole SD card over USB";
        case MENU_WIFI:     return wifiNet_state() == WifiNetState::Off ? "Phone/web control: Off" : "Phone/web control: On";
        case MENU_RETURN:   return "Return to DSpico menu";
        default:            return "Back";
    }
}

// ---------------------------------------------------------------------------
// Return to the DSpico menu (see exitToMenu.h)
// ---------------------------------------------------------------------------

extern "C" u8 gDldiStub[];

static constexpr const char* MENU_ROM_PATH = "/_picoboot.nds";      // what the DSpico boots
static constexpr const char* MENU_ROM_ARGV0 = "fat:/_picoboot.nds";
static constexpr u32 VRAM_AB_BYTES = 256 * 1024;

#define EXIT_VRAMCNT(n) (*(vu8*)(0x04000240 + (n)))

static void stopArm9Dma()
{
    for (u32 i = 0; i < 4; i++)
        *(vu32*)(0x040000B8 + i * 12) = 0;
}

[[noreturn]] static void leaveViaBootstub(const ExitBootstub* stub)
{
    REG_IME = 0;
    stopArm9Dma();
    // the state homebrew normally exits in: slot-1 belongs to the ARM9
    mem_setDsCartridgeCpu(EXMEMCNT_SLOT1_CPU_ARM9);
    DC_FlushAll();
    IC_InvalidateAll();
    stub->arm9reboot();
    while (true)
        ;
}

static u8* readWholeFile(const char* path, u32 maxSize, u32& size)
{
    FILE* f = fopen(path, "rb");
    if (!f)
        return nullptr;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8* buf = nullptr;
    if (len > 0 && (u32)len <= maxSize && (buf = (u8*)memalign(32, (len + 3) & ~3)) != nullptr)
    {
        if (fread(buf, 1, len, f) != (size_t)len)
        {
            free(buf);
            buf = nullptr;
        }
    }
    fclose(f);
    size = (u32)len;
    return buf;
}

static void copyToVram(u32 vramAddress, const u8* src, u32 size)
{
    // VRAM doesn't take byte writes
    vu32* dst = (vu32*)vramAddress;
    const u32* s32 = (const u32*)src;
    for (u32 i = 0; i < (size + 3) / 4; i++)
        dst[i] = s32[i];
}

static void doReturnToMenu()
{
    if (sMounted)
    {
        showMessage(UI_YELLOW, "Unmount first (B),", "then return to the menu.");
        return;
    }

    // WiFi off first: the next program starts with the WiFi hardware idle.
    webApp_stop();
    wifiNet_shutdown();

    const ExitBootstub* stub = exit9_findBootstub();
    if (stub)
    {
        showBusy("Returning to DSpico menu", "");
        sd_unmount();
        if (mountClient_exit(MOUNT_EXIT_BOOTSTUB, 0) == MOUNT_RESULT_OK)
            leaveViaBootstub(stub);
        sd_mount();
        showMessage(UI_RED, "The ARM7 didn't hand over.", "Turn the DS off and on.");
        return;
    }

    // No bootstub (started by something other than Pico Launcher 1.1+ with
    // Pico Loader 1.5+): boot the launcher through Pico Loader ourselves.
    showBusy("Returning to DSpico menu", "Loading Pico Loader");
    struct stat st;
    if (stat("sd:" "/_picoboot.nds", &st) != 0)
    {
        showMessage(UI_RED, "No way back found:", "no bootstub and no", "/_picoboot.nds on the SD.");
        return;
    }
    u32 size9 = 0, size7 = 0;
    u8* loader9 = readWholeFile("sd:/_pico/picoLoader9.bin", VRAM_AB_BYTES, size9);
    u8* loader7 = readWholeFile("sd:/_pico/picoLoader7.bin", VRAM_AB_BYTES, size7);
    if (!loader9 || !loader7)
    {
        free(loader9);
        free(loader7);
        showMessage(UI_RED, "No way back found:", "no bootstub and no Pico", "Loader in /_pico on the SD.");
        return;
    }
    u32 entry7 = 0;
    u16 api = 0;
    PicoLoaderError err = exit9_patchPicoLoader7(loader7, size7, (u32)gDldiStub,
        MENU_ROM_PATH, MENU_ROM_ARGV0, entry7, api);
    if (err != PicoLoaderError::None)
    {
        free(loader9);
        free(loader7);
        showMessage(UI_RED, "Can't use Pico Loader:", exit9_picoLoaderErrorString(err));
        return;
    }

    sd_unmount();
    DC_FlushAll();
    for (u32 i = 0; i < 4; i++)
        EXIT_VRAMCNT(i) = 0x80;              // A-D to LCDC
    copyToVram(0x06800000, loader9, size9);  // VRAM A+B
    copyToVram(0x06840000, loader7, size7);  // VRAM C+D
    EXIT_VRAMCNT(2) = 0x82;                  // C -> ARM7 0x06000000
    EXIT_VRAMCNT(3) = 0x8A;                  // D -> ARM7 0x06020000
    if (mountClient_exit(MOUNT_EXIT_PICO_LOADER, entry7) != MOUNT_RESULT_OK)
    {
        free(loader9);
        free(loader7);
        ui_init();
        sd_mount();
        showMessage(UI_RED, "The ARM7 didn't hand over.", "Turn the DS off and on.");
        return;
    }
    REG_IME = 0;
    stopArm9Dma();
    DC_FlushAll();
    IC_InvalidateAll();
    reinterpret_cast<void (*)()>(0x06800000)();
    while (true)
        ;
}

static void drawStartMenu()
{
    ui_clear(UI_BOTTOM);
    ui_line(UI_BOTTOM, 0, UI_CYAN, " Menu");
    ui_line(UI_BOTTOM, 1, UI_GREY, "--------------------------------");
    for (int i = 0; i < MENU_COUNT; i++)
        ui_line(UI_BOTTOM, 3 + i * 2, i == sMenuIndex ? UI_YELLOW : UI_WHITE, "%c %s",
            i == sMenuIndex ? '>' : ' ', menuLabel(i));
    if (sMenuIndex == MENU_SHARE_SD)
    {
        ui_line(UI_BOTTOM, 11, UI_GREY, " The PC sees the whole card,");
        ui_line(UI_BOTTOM, 12, UI_GREY, " like LNH's mass-storage app.");
        ui_line(UI_BOTTOM, 13, UI_GREY, " Browsing pauses until you");
        ui_line(UI_BOTTOM, 14, UI_GREY, " unmount with B.");
    }
    else if (sMenuIndex == MENU_WIFI)
    {
        ui_line(UI_BOTTOM, 11, UI_GREY, " Mount and manage images from");
        ui_line(UI_BOTTOM, 12, UI_GREY, " a phone or PC browser, even");
        ui_line(UI_BOTTOM, 13, UI_GREY, " with the DS closed. Uses the");
        ui_line(UI_BOTTOM, 14, UI_GREY, " WiFi saved in DSi Settings");
        ui_line(UI_BOTTOM, 15, UI_GREY, " (or sd:/wifi.txt).");
        if (!isDSiMode())
            ui_line(UI_BOTTOM, 17, UI_YELLOW, " DS mode: open/WEP WiFi only.");
    }
    else if (sMenuIndex == MENU_RETURN)
    {
        ui_line(UI_BOTTOM, 11, UI_GREY, " Disconnects USB and goes back");
        ui_line(UI_BOTTOM, 12, UI_GREY, " to Pico Launcher. Unmount any");
        ui_line(UI_BOTTOM, 13, UI_GREY, " image first.");
    }
    ui_line(UI_BOTTOM, 20, UI_GREY, "--------------------------------");
    ui_line(UI_BOTTOM, 21, UI_WHITE, " A: select   B: back");
}

static void drawFatal()
{
    ui_clear(UI_BOTTOM);
    for (int i = 0; i < 4; i++)
        ui_line(UI_BOTTOM, 8 + i, i == 0 ? UI_RED : UI_WHITE, " %s", sMsg[i]);
}

static void draw()
{
    drawTop();
    drawWifiLine();
    switch (sMode)
    {
        case Mode::Browse:         drawBrowse(); break;
        case Mode::ConfirmUnmount: drawConfirmUnmount(); break;
        case Mode::ConfirmDelete:  drawConfirmDelete(); break;
        case Mode::CreateSize:     drawCreateSize(); break;
        case Mode::CreateName:     drawCreateName(); break;
        case Mode::Message:        drawMessage(); break;
        case Mode::StartMenu:      drawStartMenu(); break;
        case Mode::Fatal:          drawFatal(); break;
    }
    ui_flush();
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

static void updateReadRate()
{
    const mount_status_t& s = mountClient_status();
    if (!sMounted)
    {
        sReadRateKBs = 0;
        sRateFrame = s.frame;
        sRateSectors = s.sectorsRead;
        return;
    }
    u32 frames = s.frame - sRateFrame;
    if (frames < 60)
        return;
    u32 sectors = s.sectorsRead - sRateSectors;
    sReadRateKBs = (u32)((u64)sectors * MOUNT_SECTOR_SIZE * 60 / frames / 1024);
    sRateFrame = s.frame;
    sRateSectors = s.sectorsRead;
}

static void requestUnmount()
{
    if (unmountSafety(mountClient_status()) == Safety::Safe)
        doUnmount();
    else
        sMode = Mode::ConfirmUnmount;
}

static void updateBrowse(const Buttons& b)
{
    if (sMountedSd)
    {
        // libfat is unmounted: nothing but unmounting is possible
        if (b.pressed & BTN_B)
            requestUnmount();
        else if ((b.pressed & BTN_X) && mountClient_status().hostEjected)
            mountClient_reload();
        return;
    }

    int prev = sCursor;
    if (b.repeat & BTN_UP)
        sCursor--;
    if (b.repeat & BTN_DOWN)
        sCursor++;
    if (b.repeat & BTN_LEFT)
        sCursor -= LIST_ROWS;
    if (b.repeat & BTN_RIGHT)
        sCursor += LIST_ROWS;
    clampCursor();
    if (sCursor != prev)
        sRestFrames = 0;

    if (b.pressed & BTN_Y)
    {
        showBusy("Scanning...", IMAGE_DIR);
        scanImages();
        return;
    }

    if (sMounted)
    {
        if ((b.pressed & BTN_B) || ((b.pressed & BTN_A) && isMountedEntry(sCursor)))
            requestUnmount();
        else if (b.pressed & BTN_A)
            showMessage(UI_YELLOW, "Another image is mounted.", "Unmount it first (B).");
        else if ((b.pressed & BTN_X) && mountClient_status().hostEjected)
            mountClient_reload(); // "close the tray": same image, ready again
        else if (b.pressed & (BTN_X | BTN_SELECT))
            showMessage(UI_YELLOW, "Unmount the current image", "before creating or deleting.");
        return;
    }

    if ((b.pressed & BTN_A) && sEntryCount > 0 && sEntries[sCursor].isCue)
    {
        Entry& e = sEntries[sCursor];
        if (!e.infoValid)
            analyzeEntry(e);
        if (b.held & BTN_L)
            showMessage(UI_YELLOW, "BIN/CUE images mount as a", "CD drive only (A).");
        else if (!e.cueOk)
            showMessage(UI_RED, "Can't use this BIN/CUE:", e.format.detail);
        else
            doMountCue(sCursor);
        return;
    }
    if ((b.pressed & BTN_A) && sEntryCount > 0)
    {
        Entry& e = sEntries[sCursor];
        if (!e.infoValid)
            analyzeEntry(e);
        // A: disc images (ISO/UDF/XISO) as a USB CD/DVD drive, anything else as
        // a read/write disk. L+A: any image as a read-only disk (e.g. a hybrid
        // ISO a BIOS boots as a USB stick). R+A: any image as a CD/DVD drive
        // (for disc images the detection doesn't recognise).
        bool forceDisk = (b.held & BTN_L) != 0;
        bool forceOptical = !forceDisk && (b.held & BTN_R) != 0;
        MountMediaType media = forceOptical || (e.format.isIso && !forceDisk) ? MOUNT_MEDIA_CDROM : MOUNT_MEDIA_DISK;
        bool readOnly = forceDisk || media == MOUNT_MEDIA_CDROM;
        if (e.size < MOUNT_SECTOR_SIZE)
            showMessage(UI_RED, "File is too small to mount.");
        else if (media == MOUNT_MEDIA_CDROM && e.size < 64 * 1024)
            showMessage(UI_RED, "Too small for a CD/DVD image.");
        else
            doMount(sCursor, readOnly, media);
        return;
    }
    if (b.pressed & BTN_START)
    {
        sMenuIndex = 0;
        sMode = Mode::StartMenu;
        return;
    }
    if (b.pressed & BTN_X)
    {
        if (sEntryCount >= MAX_ENTRIES)
        {
            showMessage(UI_RED, "Too many images in the folder.");
            return;
        }
        showBusy("Please wait...", "Checking free space");
        sFreeBytes = queryFreeBytes();
        defaultNewName();
        sMode = Mode::CreateSize;
        return;
    }
    if ((b.pressed & BTN_SELECT) && sEntryCount > 0)
    {
        sMode = Mode::ConfirmDelete;
        return;
    }

    // Analyse the highlighted image once the cursor has rested briefly
    if (sEntryCount > 0 && !isMountedEntry(sCursor))
    {
        Entry& e = sEntries[sCursor];
        if ((!e.infoValid || !e.layoutValid) && ++sRestFrames >= 6)
            analyzeEntry(e);
    }
}

static void updateCreateSize(const Buttons& b)
{
    if (b.repeat & BTN_UP)
        sPresetIndex = (sPresetIndex + PRESET_COUNT - 1) % PRESET_COUNT;
    if (b.repeat & BTN_DOWN)
        sPresetIndex = (sPresetIndex + 1) % PRESET_COUNT;
    if (sPresets[sPresetIndex].mib == 0)
    {
        s32 delta = 0;
        if (b.repeat & BTN_LEFT) delta -= 1;
        if (b.repeat & BTN_RIGHT) delta += 1;
        if (b.repeat & BTN_L) delta -= 64;
        if (b.repeat & BTN_R) delta += 64;
        s32 v = (s32)sCustomMiB + delta;
        if (v < 1) v = 1;
        if (v > (s32)maxImageMiB()) v = (s32)maxImageMiB();
        sCustomMiB = (u32)v;
    }
    if (b.pressed & BTN_Y)
        sCreateQuick = !sCreateQuick;
    if (b.pressed & BTN_A)
        sMode = Mode::CreateName;
    else if (b.pressed & BTN_B)
        sMode = Mode::Browse;
}

static void updateCreateName(const Buttons& b)
{
    const int charCount = sizeof(sNameChars) - 1;
    if (b.repeat & (BTN_UP | BTN_DOWN))
    {
        const char* p = strchr(sNameChars, sNewName[sNameCursor]);
        int ci = p ? (int)(p - sNameChars) : 0;
        if (b.repeat & BTN_UP)
            ci = (ci + 1) % charCount;
        if (b.repeat & BTN_DOWN)
            ci = (ci + charCount - 1) % charCount;
        sNewName[sNameCursor] = sNameChars[ci];
    }
    if ((b.repeat & BTN_LEFT) && sNameCursor > 0)
        sNameCursor--;
    if ((b.repeat & BTN_RIGHT) && sNameCursor < NAME_EDIT_LEN - 1)
        sNameCursor++;
    if (b.pressed & BTN_Y)
        sCreateQuick = !sCreateQuick;
    if (b.pressed & BTN_A)
        doCreate();
    else if (b.pressed & BTN_B)
        sMode = Mode::CreateSize;
}

static void update(const Buttons& b)
{
    switch (sMode)
    {
        case Mode::Browse:
            updateBrowse(b);
            break;
        case Mode::ConfirmUnmount:
            if (b.pressed & BTN_A)
                doUnmount();
            else if (b.pressed & BTN_B)
                sMode = Mode::Browse;
            else if (unmountSafety(mountClient_status()) == Safety::Safe)
                doUnmount(); // host ejected while the dialog was open
            break;
        case Mode::ConfirmDelete:
            if (b.pressed & BTN_A)
                doDelete(sCursor);
            else if (b.pressed & BTN_B)
                sMode = Mode::Browse;
            break;
        case Mode::CreateSize:
            updateCreateSize(b);
            break;
        case Mode::CreateName:
            updateCreateName(b);
            break;
        case Mode::Message:
            if (b.pressed & (BTN_A | BTN_B))
                sMode = Mode::Browse;
            break;
        case Mode::StartMenu:
            if (b.repeat & BTN_UP)
                sMenuIndex = (sMenuIndex + MENU_COUNT - 1) % MENU_COUNT;
            if (b.repeat & BTN_DOWN)
                sMenuIndex = (sMenuIndex + 1) % MENU_COUNT;
            if (b.pressed & BTN_A)
            {
                if (sMenuIndex == MENU_SHARE_SD)
                    doMountSd();
                else if (sMenuIndex == MENU_WIFI)
                {
                    if (wifiNet_state() == WifiNetState::Off)
                        wifiNet_start();
                    else
                    {
                        webApp_stop();
                        wifiNet_stop();
                    }
                }
                else if (sMenuIndex == MENU_RETURN)
                    doReturnToMenu();
                else
                    sMode = Mode::Browse;
            }
            else if (b.pressed & (BTN_B | BTN_START))
            {
                sMode = Mode::Browse;
            }
            break;
        case Mode::Fatal:
            break;
    }
}


// ---------------------------------------------------------------------------
// Phone / web control (see web/). Web actions run here, in the main loop,
// through the same functions as the buttons; the DS screen shows the result.
// ---------------------------------------------------------------------------

static void updateWifi()
{
    if (!gWifiStep)
        gWifiStep = "wifiNet_poll";
    wifiNet_poll();
    gWifiStep = nullptr;
    u32 now = mountClient_status().frame;
    bool connected = wifiNet_state() == WifiNetState::Connected;
    if (connected && !webApp_running())
    {
        gWifiStep = "webApp_start (socket/bind)";
        webApp_start(now);
    }
    else if (!connected && webApp_running())
    {
        gWifiStep = "webApp_stop";
        webApp_stop();
    }
    if (webApp_running())
    {
        gWifiStep = "webApp_poll (select/accept)";
        webApp_poll(now);
    }
    gWifiStep = nullptr;
}

int webBridge_imageCount()
{
    return sMountedSd ? 0 : sEntryCount;
}

bool webBridge_image(int index, WebImageInfo& out)
{
    if (index < 0 || index >= sEntryCount || sMountedSd)
        return false;
    Entry& e = sEntries[index];
    out.name = e.name;
    out.size = e.size;
    out.mounted = isMountedEntry(index);
    out.isCue = e.isCue;
    out.isDisc = e.isCue || (e.infoValid && e.format.isIso);
    out.kind = e.infoValid ? e.format.name : "";
    return true;
}

void webBridge_status(WebStatus& out)
{
    const mount_status_t& s = mountClient_status();
    Safety safety = unmountSafety(s);
    out.mounted = sMounted;
    out.sdShared = sMountedSd;
    out.readOnly = sMountedReadOnly;
    out.cd = sMounted && sMountedMedia == MOUNT_MEDIA_CDROM;
    out.hostEjected = sMounted && s.hostEjected;
    out.safeToUnmount = sMounted && safety == Safety::Safe;
    out.busy = sMounted && safety == Safety::Busy;
    out.mountedName = sMounted ? sMountedName : "";
    out.usbState = sMounted ? usbStateString(s) : "";
    out.readKBs = sReadRateKBs;
    const char* fs = sd_fsTypeName();
    out.fsType = fs ? fs : "";
    out.maxImageMiB = maxImageMiB();
}

// Runs an action and reports how it went: a red message on the DS means
// failure; any message is passed on to the browser.
static bool webOutcome(bool hadMessageBefore, const char* okText, char* msg, size_t msgSize)
{
    if (sMode == Mode::Message && !hadMessageBefore)
    {
        msg[0] = 0;
        for (int i = 0; i < 4; i++)
        {
            if (!sMsg[i][0])
                continue;
            size_t len = strlen(msg);
            snprintf(msg + len, msgSize - len, "%s%s", len ? " " : "", sMsg[i]);
        }
        return sMsgColor != UI_RED;
    }
    snprintf(msg, msgSize, "%s", okText);
    return true;
}

static bool webPrepare(char* msg, size_t msgSize)
{
    if (sMode == Mode::Fatal)
    {
        snprintf(msg, msgSize, "The DS has a problem with the SD card. Check its screen.");
        return false;
    }
    // Leave any dialog the DS was showing; the action's result replaces it.
    sMode = Mode::Browse;
    return true;
}

bool webBridge_mount(const char* name, WebMountMode mode, char* msg, size_t msgSize)
{
    if (!webPrepare(msg, msgSize))
        return false;
    if (sMountedSd || sMounted)
    {
        snprintf(msg, msgSize, "Unmount the current image first.");
        return false;
    }
    int idx = findEntry(name);
    if (idx < 0)
    {
        snprintf(msg, msgSize, "No image called %s.", name);
        return false;
    }
    sCursor = idx;
    clampCursor();
    Entry& e = sEntries[idx];
    if (!e.infoValid)
        analyzeEntry(e);
    if (e.isCue)
    {
        if (mode == WebMountMode::ReadOnly)
        {
            snprintf(msg, msgSize, "BIN/CUE images mount as a CD drive only.");
            return false;
        }
        if (!e.cueOk)
        {
            snprintf(msg, msgSize, "Can't use this BIN/CUE: %s", e.format.detail);
            return false;
        }
        doMountCue(idx);
    }
    else
    {
        bool forceDisk = mode == WebMountMode::ReadOnly;
        bool forceOptical = mode == WebMountMode::Disc;
        MountMediaType media = forceOptical || (e.format.isIso && !forceDisk) ? MOUNT_MEDIA_CDROM : MOUNT_MEDIA_DISK;
        bool readOnly = forceDisk || media == MOUNT_MEDIA_CDROM;
        if (e.size < MOUNT_SECTOR_SIZE)
        {
            snprintf(msg, msgSize, "File is too small to mount.");
            return false;
        }
        if (media == MOUNT_MEDIA_CDROM && e.size < 64 * 1024)
        {
            snprintf(msg, msgSize, "Too small for a CD/DVD image.");
            return false;
        }
        doMount(idx, readOnly, media);
    }
    char ok[96];
    snprintf(ok, sizeof(ok), "Mounted %.60s", name);
    bool result = webOutcome(false, ok, msg, msgSize);
    return result && sMounted;
}

bool webBridge_unmount(bool force, char* msg, size_t msgSize)
{
    if (!webPrepare(msg, msgSize))
        return false;
    if (!sMounted)
    {
        snprintf(msg, msgSize, "Nothing is mounted.");
        return false;
    }
    Safety safety = unmountSafety(mountClient_status());
    if (safety != Safety::Safe && !force)
    {
        snprintf(msg, msgSize, safety == Safety::Busy ? "The PC is using the drive. Eject it there first."
                                                      : "Eject it on the PC first (or unmount anyway).");
        return false;
    }
    doUnmount();
    bool ok = webOutcome(false, "Unmounted", msg, msgSize);
    return ok && !sMounted;
}

bool webBridge_reinsert(char* msg, size_t msgSize)
{
    if (!sMounted || !mountClient_status().hostEjected)
    {
        snprintf(msg, msgSize, "Nothing to reinsert.");
        return false;
    }
    mountClient_reload();
    snprintf(msg, msgSize, "Reinserted");
    return true;
}

bool webBridge_delete(const char* name, char* msg, size_t msgSize)
{
    if (!webPrepare(msg, msgSize))
        return false;
    if (sMounted || sMountedSd)
    {
        snprintf(msg, msgSize, "Unmount the current image before deleting.");
        return false;
    }
    int idx = findEntry(name);
    if (idx < 0)
    {
        snprintf(msg, msgSize, "No image called %s.", name);
        return false;
    }
    doDelete(idx);
    char ok[96];
    snprintf(ok, sizeof(ok), "Deleted %.60s", name);
    return webOutcome(false, ok, msg, msgSize) && findEntry(name) < 0;
}

bool webBridge_create(const char* name, u32 mib, char* msg, size_t msgSize)
{
    if (!webPrepare(msg, msgSize))
        return false;
    if (sMounted || sMountedSd)
    {
        snprintf(msg, msgSize, "Unmount the current image before creating one.");
        return false;
    }
    // Same rules as the DS name editor: letters, digits, '_' and '-', 1-16 long.
    char base[NAME_EDIT_LEN + 1];
    size_t len = strlen(name);
    if (len >= 4 && strcasecmp(name + len - 4, ".img") == 0)
        len -= 4;
    if (len == 0 || len > NAME_EDIT_LEN)
    {
        snprintf(msg, msgSize, "Use a name of 1-%d characters.", NAME_EDIT_LEN);
        return false;
    }
    for (size_t i = 0; i < len; i++)
    {
        char c = name[i];
        if (c >= 'a' && c <= 'z')
            c = c - 'a' + 'A';
        bool okChar = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (c == ' ')
        {
            c = '_';
            okChar = true;
        }
        if (!okChar)
        {
            snprintf(msg, msgSize, "Names can use letters, digits, '_' and '-'.");
            return false;
        }
        base[i] = c;
    }
    base[len] = 0;
    if (mib == 0 || mib > maxImageMiB())
    {
        snprintf(msg, msgSize, "Size must be 1-%lu MB on this SD card.", maxImageMiB());
        return false;
    }
    if (sEntryCount >= MAX_ENTRIES)
    {
        snprintf(msg, msgSize, "Too many images in the folder.");
        return false;
    }
    char fileName[NAME_LEN];
    snprintf(fileName, sizeof(fileName), "%s.img", base);
    if (findEntry(fileName) >= 0)
    {
        snprintf(msg, msgSize, "An image called %s already exists.", fileName);
        return false;
    }
    u64 total = (u64)mib * MiB;
    u64 freeBytes = queryFreeBytes();
    if (freeBytes && total + MiB > freeBytes)
    {
        char need[12], have[12];
        ui_formatSize(need, total);
        ui_formatSize(have, freeBytes);
        snprintf(msg, msgSize, "Not enough free space: need %s, %s free.", need, have);
        return false;
    }
    doCreateQuick(fileName, total);
    char ok[96];
    snprintf(ok, sizeof(ok), "Created %s", fileName);
    return webOutcome(false, ok, msg, msgSize) && findEntry(fileName) >= 0;
}

bool webBridge_rescan(char* msg, size_t msgSize)
{
    if (sMountedSd)
    {
        snprintf(msg, msgSize, "The SD card is shared with the PC.");
        return false;
    }
    showBusy("Scanning...", IMAGE_DIR);
    scanImages();
    snprintf(msg, msgSize, "%d image%s", sEntryCount, sEntryCount == 1 ? "" : "s");
    return true;
}

bool webBridge_shareSd(char* msg, size_t msgSize)
{
    if (!webPrepare(msg, msgSize))
        return false;
    if (sMounted)
    {
        snprintf(msg, msgSize, "Unmount the current image first.");
        return false;
    }
    doMountSd();
    return webOutcome(false, "Whole SD card shared over USB", msg, msgSize) && sMountedSd;
}

// ---------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    mem_setDsCartridgeCpu(EXMEMCNT_SLOT1_CPU_ARM7);

    rtos_initIrq();
    rtos_startMainThread();
    ipc_initFifoSystem();
    // libnds FIFO messages (DSWiFi, firmware reads) over libtwl IPC; without
    // this the ARM9's libnds messages are never sent.
    ndsFifoShim_init(nullptr);
    // In DSi mode libnds lets the heap grow to the end of the 16 MB, over
    // 0x02380000+ where the ARM7 keeps its main-RAM data (USB buffers, WiFi
    // thread stack, FIFO queues). Keep the heap below it.
    {
        u8* limit = getHeapLimit();
        if (limit > (u8*)0x02380000)
            reduceHeapSize(limit - (u8*)0x02380000);
    }
    // A crash (bad memory access) shows libnds's red register screen instead
    // of a silent freeze.
    defaultExceptionHandler();

    rtos_createEvent(&sVblankEvent);
    ui_init();

    while (ipc_getArm7SyncBits() != 7);

    bool dldiOk = dldi_init();
    ipc_setArm9SyncBits(6);

    rtos_setIrqFunc(RTOS_IRQ_VBLANK, vblankIrq);
    rtos_enableIrqMask(RTOS_IRQ_VBLANK);
    gfx_setVBlankIrqEnabled(true);

    mountClient_init();

    if (!dldiOk)
    {
        snprintf(sMsg[0], sizeof(sMsg[0]), "SD card driver failed.");
        snprintf(sMsg[1], sizeof(sMsg[1]), "Start this from Pico Launcher");
        snprintf(sMsg[2], sizeof(sMsg[2]), "on a DSpico.");
        sMsg[3][0] = 0;
        sMode = Mode::Fatal;
    }
    else if (!sd_mount())
    {
        snprintf(sMsg[0], sizeof(sMsg[0]), "Couldn't read the SD card.");
#ifdef IMAGE_MOUNTER_EXFAT
        snprintf(sMsg[1], sizeof(sMsg[1]), "Needs FAT16, FAT32 or exFAT.");
#else
        snprintf(sMsg[1], sizeof(sMsg[1]), "It must be FAT16 or FAT32.");
#endif
        sMsg[2][0] = 0;
        sMsg[3][0] = 0;
        sMode = Mode::Fatal;
    }
    else
    {
        mkdir(IMAGE_DIR, 0777);
        scanImages();
    }

    while (true)
    {
        waitFrame();
        mountClient_poll();
        updateReadRate();
        Buttons b = ui_readButtons(mountClient_status().keyXY);
        update(b);
        updateWifi();
        draw();
    }

    return 0;
}
