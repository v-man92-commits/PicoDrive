// Host test for arm9/source/web/wifiNet.cpp's state machine with a scripted
// fake DSWiFi (scan results and how each connection attempt goes).
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include <functional>
#include "dswifi9.h"
#include "wifiNet.h"
#include "wfcslots.h"
static int pass, fails;
#define CHECK(c, ...) do { if (c) pass++; else { fails++; printf("  FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static std::vector<Wifi_AccessPoint> gAps;
static std::string gLastSsid, gLastKey, gLastMethod;
static int gFramesInAttempt, gAttempts, gInits, gSetIpCalls;
static u32 gSetIp;
// how an attempt behaves: returns assoc status given (ssid, method, frames since start)
static std::function<int(const std::string&, const std::string&, int)> gBehaviour;
static bool gWifiTxt;

extern "C" {
int readFirmware(u32 addr, void* buf, u32 len)
{
    memset(buf, 0, len);
    if (addr == 0x20) { *(u16*)buf = 0x3FC0; return 0; }
    u8* b = (u8*)buf;
    if (addr == 0x1F400) { memcpy(b + 0x40, "HOME", 4); b[0xE7] = 7; b[0xEF] = 1; memcpy(b + 0x120, "homepass", 8); }
    if (addr == 0x1F600) { memcpy(b + 0x40, "GUEST", 5); b[0xE7] = 7; b[0xEF] = 1; }
    return 0;
}
bool Wifi_InitDefault(unsigned) { gInits++; return true; }
bool Wifi_Deinit(void) { return true; }
void Wifi_DisableWifi(void) {}
void Wifi_EnableWifi(void) {}
void Wifi_ScanMode(void) {}
int Wifi_GetNumAP(void) { return (int)gAps.size(); }
int Wifi_GetAPData(int i, Wifi_AccessPoint* a) { *a = gAps[i]; return 0; }
int Wifi_ConnectSecureAP(Wifi_AccessPoint* a, const void* k, size_t l) { gLastSsid = a->ssid; gLastKey = k ? std::string((const char*)k, l) : ""; gLastMethod = "secure"; gFramesInAttempt = 0; gAttempts++; return 0; }
int Wifi_ConnectWfcAP(Wifi_AccessPoint* a) { gLastSsid = a->ssid; gLastMethod = "wfc"; gFramesInAttempt = 0; gAttempts++; return 0; }
int Wifi_AssocStatus(void) { return gLastSsid.empty() ? ASSOCSTATUS_DISCONNECTED : gBehaviour(gLastSsid, gLastMethod, gFramesInAttempt++); }
int Wifi_DisconnectAP(void) { gLastSsid.clear(); return 0; }
u32 Wifi_GetIP(void) { return 0x1D12A8C0; } // 192.168.18.29
int Wifi_GetData(int, int, unsigned char*) { return 200; }
void Wifi_SetIP(u32 ip, u32, u32, u32, u32) { gSetIp = ip; gSetIpCalls++; }
}
// (no sd:/wifi.txt on the host: fopen fails, so the saved DSi networks are used)
static Wifi_AccessPoint ap(const char* ssid, int rssi, u16 flags)
{
    Wifi_AccessPoint a = {}; strcpy(a.ssid, ssid); a.ssid_len = (u8)strlen(ssid); a.rssi = (s16)rssi; a.flags = flags; return a;
}
static int run(int frames) { int n = 0; while (n < frames && wifiNet_state() != WifiNetState::Connected) { wifiNet_poll(); n++; } return n; }

int main()
{
    const u16 WPA = WFLAG_APDATA_WPA | WFLAG_APDATA_COMPATIBLE | WFLAG_APDATA_CONFIG_IN_WFC;
    gAps = { ap("", -30, WPA), ap("GUEST", -70, WPA), ap("HOME", -40, WPA), ap("NEIGHBOUR", -50, WFLAG_APDATA_WPA | WFLAG_APDATA_COMPATIBLE) };

    // 1. HOME works with its saved password; DHCP takes 3 s
    gBehaviour = [](const std::string& s, const std::string& m, int f) {
        if (s != "HOME" || m != "secure") return (int)ASSOCSTATUS_CANNOTCONNECT;
        return f < 30 ? (int)ASSOCSTATUS_ASSOCIATING : f < 210 ? (int)ASSOCSTATUS_ACQUIRINGDHCP : (int)ASSOCSTATUS_ASSOCIATED; };
    wifiNet_start();
    CHECK(wifiNet_state() == WifiNetState::Starting, "starting");
    int n = run(60 * 30);
    CHECK(wifiNet_state() == WifiNetState::Connected && gLastSsid == "HOME" && gLastKey == "homepass" && gAttempts == 1, "connected to HOME first try (%d attempts)", gAttempts);
    CHECK(n < 60 * 10, "connected in %d frames", n);
    char ip[16]; wifiNet_ipString(wifiNet_ip(), ip, sizeof ip);
    CHECK(!strcmp(ip, "192.168.18.29") && gSetIp == 0 && gSetIpCalls == 1, "ip %s, DHCP mode", ip);
    CHECK(!strcmp(wifiNet_network(), "HOME"), "network name");

    // 2. connection lost -> scans again and reconnects by itself
    gLastSsid.clear();
    wifiNet_poll();
    CHECK(wifiNet_state() == WifiNetState::Scanning && !strcmp(wifiNet_lastError(), "Connection lost"), "lost -> rescan");
    run(60 * 30);
    CHECK(wifiNet_state() == WifiNetState::Connected, "reconnected");
    wifiNet_stop();
    CHECK(wifiNet_state() == WifiNetState::Off && wifiNet_ip() == 0, "stopped");

    // 3. saved password refused -> WFC method -> then GUEST
    gAttempts = 0;
    std::vector<std::string> tried;
    gBehaviour = [&](const std::string& s, const std::string& m, int f) {
        if (f == 0) tried.push_back(s + "/" + m);
        if (s == "GUEST") return f < 60 ? (int)ASSOCSTATUS_ACQUIRINGDHCP : (int)ASSOCSTATUS_ASSOCIATED;
        return (int)ASSOCSTATUS_CANNOTCONNECT; };
    wifiNet_start();
    run(60 * 60);
    CHECK(wifiNet_state() == WifiNetState::Connected && gLastSsid == "GUEST", "fell back to GUEST");
    CHECK(tried.size() == 3 && tried[0] == "HOME/secure" && tried[1] == "HOME/wfc" && tried[2] == "GUEST/wfc", "order %s,%s,%s",
          tried.size() > 0 ? tried[0].c_str() : "-", tried.size() > 1 ? tried[1].c_str() : "-", tried.size() > 2 ? tried[2].c_str() : "-");
    wifiNet_stop();

    // 4. DHCP never answers: each attempt times out, then Failed, then retries later
    gBehaviour = [](const std::string&, const std::string&, int) { return (int)ASSOCSTATUS_ACQUIRINGDHCP; };
    wifiNet_start();
    int frames = 0;
    while (wifiNet_state() != WifiNetState::Failed && frames < 60 * 400) { wifiNet_poll(); frames++; }
    CHECK(wifiNet_state() == WifiNetState::Failed && strstr(wifiNet_lastError(), "DHCP"), "failed: %s after %d s", wifiNet_lastError(), frames / 60);
    CHECK(frames > 60 * 45 * 3 - 60 && frames < 60 * 45 * 3 + 60 * 70, "3 attempts x 45 s DHCP wait (%d s)", frames / 60);
    for (int i = 0; i < 60 * 16; i++) wifiNet_poll();
    CHECK(wifiNet_state() == WifiNetState::Scanning || wifiNet_state() == WifiNetState::Connecting, "retries by itself");
    wifiNet_stop();

    // 5. no saved network in range
    gAps = { ap("NEIGHBOUR", -50, WFLAG_APDATA_WPA | WFLAG_APDATA_COMPATIBLE) };
    wifiNet_start();
    for (int i = 0; i < 60 * 6; i++) wifiNet_poll();
    CHECK(wifiNet_state() == WifiNetState::Failed && strstr(wifiNet_lastError(), "No saved network"), "no saved network: %s", wifiNet_lastError());
    wifiNet_shutdown();
    CHECK(gInits == 1, "DSWiFi initialised once (%d)", gInits);

    printf("wifinet: %d checks passed, %d failed\n", pass, fails);
    return fails ? 1 : 0;
}
