#pragma once
// What the web UI needs from the image mounter. Implemented in main.cpp, so
// web actions go through exactly the same code as the buttons on the DS.
#include <stddef.h>
#include <nds/ndstypes.h>

struct WebImageInfo
{
    const char* name;
    u64 size;
    const char* kind;   ///< short description ("FATX", "ISO 9660", "BIN/CUE"...) or ""
    bool mounted;
    bool isCue;
    bool isDisc;        ///< mounts as a CD/DVD drive by default
};

struct WebStatus
{
    bool mounted;
    bool sdShared;
    bool readOnly;
    bool cd;
    bool hostEjected;
    bool safeToUnmount;
    bool busy;          ///< host is using the drive
    const char* mountedName;
    const char* usbState;
    u32 readKBs;
    const char* fsType;
    u32 maxImageMiB;    // largest image that can be created now (file limit and free space)
    u32 fsLimitMiB;     // the filesystem's own per-file limit
};

enum class WebMountMode
{
    Auto,       ///< like A on the DS
    ReadOnly,   ///< like L+A
    Disc        ///< like R+A
};

int webBridge_imageCount();
bool webBridge_image(int index, WebImageInfo& out);
void webBridge_status(WebStatus& out);

// Actions return true on success; msg gets a short human-readable result.
bool webBridge_mount(const char* name, WebMountMode mode, char* msg, size_t msgSize);
bool webBridge_unmount(bool force, char* msg, size_t msgSize);
bool webBridge_reinsert(char* msg, size_t msgSize);
bool webBridge_delete(const char* name, char* msg, size_t msgSize);
bool webBridge_create(const char* name, u32 mib, char* msg, size_t msgSize);
bool webBridge_rescan(char* msg, size_t msgSize);
bool webBridge_shareSd(char* msg, size_t msgSize);
