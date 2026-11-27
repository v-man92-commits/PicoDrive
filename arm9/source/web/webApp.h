#pragma once
#include <nds/ndstypes.h>

/// Starts the web UI on port 80 and picks a new PIN. now = frame counter.
bool webApp_start(u32 now);
void webApp_stop();
void webApp_poll(u32 now);
bool webApp_running();
const char* webApp_pin();
