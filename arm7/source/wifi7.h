#pragma once
#include <libtwl/rtos/rtosMutex.h>

/// Held by anything using the SPI or I2C buses (PMIC backlights/shutdown,
/// DSi MCU, DSWiFi, firmware reads).
extern rtos_mutex_t gSpiMutex;

/// Call after ipc_initFifoSystem().
void wifi7_init();

/// Call every frame (VBlank): lets DSWiFi run Wifi_Update().
void wifi7_tick();
