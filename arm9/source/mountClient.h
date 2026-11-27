#pragma once
#include "mountIpc.h"
#include "cdLayout.h"

/// ARM9 side of the mount IPC channel.

void mountClient_init();

/// Latest status published by the ARM7 (cache-invalidated copy).
const mount_status_t& mountClient_status();

/// Refreshes the status copy. Call once per frame.
void mountClient_poll();
// Copy of the ARM7 status without touching the cache (for interrupt handlers).
void mountClient_peekStatus(mount_status_t* out);

u32 mountClient_mount(const mount_extent_t* extents, u32 extentCount, u32 totalSectors,
    MountMediaType mediaType, bool readOnly, const cd_layout_t* cdLayout = nullptr);

u32 mountClient_unmount();

/// Undoes a host eject ("closes the tray") without unmounting.
u32 mountClient_reload();

/// Hands the ARM7 over for leaving the app (see MOUNT_IPC_CMD_EXIT). Returns
/// MOUNT_RESULT_OK once the ARM7 has disconnected USB and let go of the card.
u32 mountClient_exit(MountExitMode mode, u32 arm7Entry);
