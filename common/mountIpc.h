#pragma once
// Shared ARM9 <-> ARM7 definitions for mounting an image file over USB.
//
// The ARM9 owns the FAT filesystem (libfat). When an image is mounted it
// resolves the image file's cluster chain into a list of extents (runs of
// contiguous SD sectors) and hands that list to the ARM7. The ARM7 then
// serves USB mass storage reads/writes straight to those SD sectors, so no
// ARM9 round trip is needed per USB transfer.

#define MOUNT_MAX_EXTENTS       2048
#define MOUNT_SECTOR_SIZE       512

typedef struct
{
    u32 fileSector;   // first 512-byte sector of the image covered by this run
    u32 sdSector;     // absolute SD card sector where the run starts
    u32 count;        // number of sectors in the run
} mount_extent_t;

typedef enum
{
    MOUNT_MEDIA_DISK  = 0,  // direct-access block device, 512-byte sectors
    MOUNT_MEDIA_CDROM = 1   // reserved for ISO / optical emulation
} MountMediaType;

typedef enum
{
    MOUNT_IPC_CMD_INIT,      // buffer = mount_status_t* to publish status into
    MOUNT_IPC_CMD_MOUNT,
    MOUNT_IPC_CMD_UNMOUNT,
    MOUNT_IPC_CMD_RELOAD,    // "close the tray": undo a host eject, keep the image
    MOUNT_IPC_CMD_EXIT       // leave for the DSpico menu: mediaType = MountExitMode,
                             // buffer = ARM7 entry point (Pico Loader mode). The ARM7
                             // replies, then never comes back.
} MountIpcCommand;

typedef enum
{
    MOUNT_EXIT_BOOTSTUB = 0,    // answer the bootstub's libnds reset handshake
    MOUNT_EXIT_PICO_LOADER = 1  // jump to picoLoader7 (already in VRAM C/D)
} MountExitMode;

typedef enum
{
    MOUNT_RESULT_OK = 0,
    MOUNT_RESULT_BAD_ARGS = 1,
    MOUNT_RESULT_ALREADY_MOUNTED = 2,
    MOUNT_RESULT_NOT_MOUNTED = 3
} MountResult;

typedef struct alignas(32)
{
    u32 cmd;
    void* buffer;                     // INIT: status struct
    const mount_extent_t* extents;    // MOUNT: extent table (main RAM)
    u32 extentCount;
    u32 totalSectors;                 // image size in 512-byte sectors
    u32 mediaType;
    u32 readOnly;
    const void* cdLayout;             // MOUNT, CD-ROM: cd_layout_t of a BIN/CUE disc, or null (ISO)
} mount_ipc_cmd_t;

// USB connection state as seen by the ARM7
typedef enum
{
    MOUNT_USB_IDLE = 0,          // no image mounted, USB pull-up off
    MOUNT_USB_WAITING = 1,       // pull-up on, no host has configured us yet
    MOUNT_USB_CONFIGURED = 2,    // host enumerated the device
    MOUNT_USB_SUSPENDED = 3
} MountUsbState;

// Published by the ARM7 every frame. The ARM9 only ever reads it
// (after a cache invalidate), so it never needs to be flushed back.
typedef struct alignas(32)
{
    vu32 frame;              // ARM7 vblank counter
    vu32 usbState;           // MountUsbState
    vu32 mounted;            // an image is attached
    vu32 readOnly;
    vu32 hostPrevent;        // host has locked the medium (PREVENT MEDIUM REMOVAL)
    vu32 hostEjected;        // host sent START STOP UNIT with eject
    vu32 lastWriteFrame;     // frame of the last completed write
    vu32 lastIoFrame;        // frame of the last read or write
    vu32 writesSinceSync;    // writes since the host last synced/ejected
    vu32 sectorsRead;
    vu32 sectorsWritten;
    vu32 ioErrors;
    vu32 keyXY;              // REG_KEYXY (ARM7 only register: X/Y buttons, lid)
    vu32 everConfigured;     // host has configured us at least once this mount
    vu32 usbBadWrites;       // writes refused because their data was a USB command block
    vu32 usbSpurious;        // DSpico OUT completions with no transfer armed (ignored)
    vu32 scsiRefused;        // host commands answered with an error (diagnostics)
    vu32 lastRefusedOp;      // opcode of the last one
    vu32 lastRefusedSense;   // its sense: key << 16 | ASC << 8 | ASCQ
    // WiFi diagnostics (ARM7 side of DSWiFi)
    vu32 wifiFifoRx;         // libnds FIFO messages received from the ARM9
    vu32 wifiFifoDone;       // ... dispatched to their handlers
    vu32 wifiFifoLast;       // last one: type << 8 | libnds channel
    vu32 wifiUpdates;        // Wifi_Update() calls
    vu32 wifiThreadState;    // 0 not started, 1 waiting, 2 dispatching, 3 in Wifi_Update
    vu32 arm7IrqIn;          // libnds-style (DSWiFi) IRQ handlers running now
    vu32 arm7Irq2In;         // same, DSi IRQ2 sources
    vu32 arm7Step;           // ARM7 main thread: 1 waiting for VBlank, 2 working
    vu32 arm7IrqNow;         // IRQ bit whose (libtwl-dispatched) handler is running, 0 = none
    vu32 wifiStackFree;      // bytes of the ARM7 WiFi thread stack never used
    vu32 arm7Thread;         // 0x100 | priority of the ARM7 thread last switched to
    vu32 arm7IE, arm7IE2, arm7IME; // ARM7 interrupt enables, as last seen by its main loop
} mount_status_t;
