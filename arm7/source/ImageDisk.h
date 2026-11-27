#pragma once
#include "mountIpc.h"

/// Serves a mounted image file as a USB mass storage device.
/// The image is described by a list of SD card extents built by the ARM9.

void imageDisk_init();

/// Sets the status block (in main RAM) that is published to the ARM9.
void imageDisk_setStatus(mount_status_t* status);
mount_status_t* imageDisk_status();

/// Attaches an image and connects to the USB host. Returns a MountResult.
u32 imageDisk_mount(const mount_ipc_cmd_t* cmd);

/// Detaches the image (waits for any in-flight transfer) and disconnects
/// from the USB host. Returns a MountResult.
u32 imageDisk_unmount();

/// Undoes a host eject ("closes the tray"): the same image becomes ready
/// again and the host is told the medium changed.
u32 imageDisk_reload();

/// Unmounts (if needed) and disconnects from USB for good, then takes the
/// card bus so nothing touches the SD card any more. For leaving the app.
void imageDisk_shutdown();

/// Called once per frame from the ARM7 main thread.
void imageDisk_updateFrame();
