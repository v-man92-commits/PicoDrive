#pragma once
#include <stddef.h>
#include <nds/ndstypes.h>

enum class WifiNetState
{
    Off,
    Starting,
    Scanning,
    Connecting,
    Connected,
    Failed      // retries by itself after a few seconds
};

void wifiNet_start();
void wifiNet_stop();
/// Stops WiFi and releases DSWiFi (before leaving the app).
void wifiNet_shutdown();
/// Call once per frame.
void wifiNet_poll();

WifiNetState wifiNet_state();
u32 wifiNet_ip();                 ///< 0 unless connected (network byte order)
const char* wifiNet_network();
const char* wifiNet_status();     ///< short text for the screen
const char* wifiNet_lastError();
int wifiNet_signal();
void wifiNet_ipString(u32 ip, char* out, size_t size);

/// Diagnostics: the step being performed right now (blocking library calls),
/// or nullptr when idle. Shown by main.cpp's watchdog if the DS seems stuck.
extern volatile const char* gWifiStep;
