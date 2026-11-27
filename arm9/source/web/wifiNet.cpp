// WiFi connection manager for the image mounter (ARM9, DSWiFi in DSi mode).
//
// Non-blocking: wifiNet_poll() is called once per frame and advances a small
// state machine, so the DS stays responsive (and USB keeps being served)
// while scanning and connecting. The connection logic is the one proven on
// hardware with wifi-probe:
//   1. sd:/wifi.txt (ssid=/password=, optional fixed ip=) if present;
//   2. otherwise the networks saved in the DSi's settings (connections 4-6),
//      strongest signal first; each is tried with its saved password handed
//      straight to DSWiFi, then with DSWiFi's own saved-settings path.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <nds.h>
#include <dswifi9.h>
#include "wificonfig.h"
#include "wfcslots.h"
#include "wifiNet.h"

volatile const char* gWifiStep;

namespace
{
    constexpr int SCAN_FRAMES = 4 * 60;
    constexpr int JOIN_FRAMES = 20 * 60;
    constexpr int DHCP_FRAMES = 45 * 60;
    constexpr int RETRY_FRAMES = 15 * 60;
    constexpr int MAX_CANDIDATES = 8;

    struct Attempt
    {
        Wifi_AccessPoint ap;
        int method; // 0 = wifi.txt, 1 = saved password, 2 = saved WFC settings
    };

    WifiNetState sState = WifiNetState::Off;
    bool sInitialised;
    bool sHaveConfig;
    wifi_config_t sConfig;
    wfc_slot_t sSlots[3];
    bool sSlotsRead;

    Attempt sAttempts[MAX_CANDIDATES * 2 + 1];
    int sAttemptCount;
    int sAttemptIndex;
    int sTimer;
    bool sInDhcp;
    u32 sIp;
    char sNetwork[33];
    char sStatus[40];
    char sFailReason[40];

    const char* securityName(u16 flags)
    {
        if (flags & WFLAG_APDATA_WPA)
            return "WPA";
        if (flags & WFLAG_APDATA_WEP)
            return "WEP";
        return "open";
    }

    void setStatus(const char* fmt, const char* arg = "")
    {
        snprintf(sStatus, sizeof(sStatus), fmt, arg);
    }

    void fail(const char* reason)
    {
        snprintf(sFailReason, sizeof(sFailReason), "%s", reason);
        setStatus("%s", reason);
        sState = WifiNetState::Failed;
        sTimer = RETRY_FRAMES;
        Wifi_DisconnectAP();
    }

    void readSlots()
    {
        if (sSlotsRead || !isDSiMode())
            return;
        sSlotsRead = true;
        u16 w = 0;
        gWifiStep = "reading firmware header";
        readFirmware(0x20, &w, sizeof(w));
        u32 base = wfc_dsi_slots_address(w);
        static u8 buf[WFC_SLOT_SIZE];
        for (int i = 0; i < 3; i++)
        {
            memset(buf, 0, sizeof(buf));
            gWifiStep = "reading saved WiFi (firmware)";
            readFirmware(base + (u32)i * WFC_SLOT_SIZE, buf, sizeof(buf));
            wfc_decode_slot(buf, &sSlots[i]);
        }
        gWifiStep = nullptr;
    }

    const wfc_slot_t* findSlot(const char* ssid)
    {
        for (const wfc_slot_t& s : sSlots)
            if (s.configured && strcmp(s.ssid, ssid) == 0)
                return &s;
        return nullptr;
    }

    void loadConfig()
    {
        sHaveConfig = false;
        gWifiStep = "reading sd:/wifi.txt";
        FILE* f = fopen("sd:/wifi.txt", "rb");
        gWifiStep = nullptr;
        if (!f)
            return;
        char buf[512];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = 0;
        sHaveConfig = wifi_config_parse(buf, n, &sConfig) == 0;
    }

    // Builds the list of connection attempts from the scan results.
    void planAttempts()
    {
        sAttemptCount = 0;
        sAttemptIndex = 0;
        int num = Wifi_GetNumAP();

        if (sHaveConfig && sConfig.has_ssid)
        {
            for (int i = 0; i < num; i++)
            {
                Wifi_AccessPoint ap;
                Wifi_GetAPData(i, &ap);
                if (ap.ssid_len && strcmp(ap.ssid, sConfig.ssid) == 0 && (ap.flags & WFLAG_APDATA_COMPATIBLE))
                {
                    sAttempts[sAttemptCount++] = { ap, 0 };
                    return;
                }
            }
            return;
        }

        // saved networks, strongest first
        Wifi_AccessPoint cand[MAX_CANDIDATES];
        int ncand = 0;
        for (int i = 0; i < num; i++)
        {
            Wifi_AccessPoint ap;
            Wifi_GetAPData(i, &ap);
            bool hidden = ap.ssid_len == 0 || ap.ssid[0] == 0;
            if (hidden || !(ap.flags & WFLAG_APDATA_CONFIG_IN_WFC) || !(ap.flags & WFLAG_APDATA_COMPATIBLE))
                continue;
            if (ncand >= MAX_CANDIDATES)
                break;
            int k = ncand++;
            while (k > 0 && cand[k - 1].rssi < ap.rssi)
            {
                cand[k] = cand[k - 1];
                k--;
            }
            cand[k] = ap;
        }
        for (int i = 0; i < ncand; i++)
        {
            const wfc_slot_t* slot = findSlot(cand[i].ssid);
            if (slot && slot->passphrase_len && (cand[i].flags & WFLAG_APDATA_WPA))
                sAttempts[sAttemptCount++] = { cand[i], 1 };
            sAttempts[sAttemptCount++] = { cand[i], 2 };
        }
    }

    bool startAttempt()
    {
        while (sAttemptIndex < sAttemptCount)
        {
            Attempt& a = sAttempts[sAttemptIndex];
            snprintf(sNetwork, sizeof(sNetwork), "%s", a.ap.ssid);
            int rc;
            if (a.method == 0)
            {
                size_t len = strlen(sConfig.password);
                gWifiStep = "Wifi_ConnectSecureAP (wifi.txt)";
                rc = Wifi_ConnectSecureAP(&a.ap, len ? sConfig.password : nullptr, len);
            }
            else if (a.method == 1)
            {
                const wfc_slot_t* slot = findSlot(a.ap.ssid);
                gWifiStep = "Wifi_ConnectSecureAP (DS slot)";
                rc = slot ? Wifi_ConnectSecureAP(&a.ap, slot->passphrase, (size_t)slot->passphrase_len) : -1;
            }
            else
            {
                gWifiStep = "Wifi_ConnectWfcAP";
                rc = Wifi_ConnectWfcAP(&a.ap);
            }
            gWifiStep = "wifiNet_poll";
            if (rc == 0)
            {
                setStatus("Joining %.24s", a.ap.ssid);
                sState = WifiNetState::Connecting;
                sTimer = JOIN_FRAMES;
                sInDhcp = false;
                return true;
            }
            sAttemptIndex++;
        }
        return false;
    }

    void nextAttempt(const char* why)
    {
        snprintf(sFailReason, sizeof(sFailReason), "%s", why);
        Wifi_DisconnectAP();
        sAttemptIndex++;
        if (!startAttempt())
            fail(why);
    }

    void beginScan()
    {
        sIp = 0;
        sNetwork[0] = 0;
        Wifi_DisconnectAP();
        Wifi_ScanMode();
        setStatus("Looking for networks");
        sState = WifiNetState::Scanning;
        sTimer = SCAN_FRAMES;
    }
}

void wifiNet_start()
{
    if (sState != WifiNetState::Off)
        return;
    sFailReason[0] = 0;
    if (!isDSiMode())
    {
        // DS mode only does open/WEP networks; still try (wifi.txt / WFC 1-3)
    }
    loadConfig();
    readSlots();
    setStatus("Starting WiFi");
    sState = WifiNetState::Starting;
}

void wifiNet_stop()
{
    if (sState == WifiNetState::Off)
        return;
    if (sInitialised)
    {
        Wifi_DisconnectAP();
        Wifi_DisableWifi();
    }
    sState = WifiNetState::Off;
    sIp = 0;
    setStatus("Off");
}

void wifiNet_shutdown()
{
    wifiNet_stop();
    if (sInitialised)
    {
        Wifi_Deinit();
        sInitialised = false;
    }
}

void wifiNet_poll()
{
    switch (sState)
    {
        case WifiNetState::Off:
            break;

        case WifiNetState::Starting:
            if (!sInitialised)
            {
                gWifiStep = "Wifi_InitDefault (ARM7 WiFi start)";
                bool ok = Wifi_InitDefault(INIT_ONLY | WIFI_ATTEMPT_DSI_MODE);
                gWifiStep = nullptr;
                if (!ok)
                {
                    fail("WiFi didn't start");
                    break;
                }
                sInitialised = true;
            }
            else
            {
                gWifiStep = "Wifi_EnableWifi";
                Wifi_EnableWifi();
                gWifiStep = nullptr;
            }
            gWifiStep = "Wifi_SetIP";
            if (sHaveConfig && sConfig.has_static_ip)
                Wifi_SetIP(sConfig.ip, sConfig.gateway, sConfig.netmask, sConfig.dns, 0);
            else
                Wifi_SetIP(0, 0, 0, 0, 0);
            gWifiStep = "Wifi_ScanMode";
            beginScan();
            gWifiStep = nullptr;
            break;

        case WifiNetState::Scanning:
            if (--sTimer > 0)
                break;
            planAttempts();
            if (sAttemptCount == 0)
            {
                if (sHaveConfig && sConfig.has_ssid)
                    fail("Network in wifi.txt not seen");
                else
                    fail("No saved network in range");
                break;
            }
            if (!startAttempt())
                fail("Couldn't start joining");
            break;

        case WifiNetState::Connecting:
        {
            gWifiStep = "Wifi_AssocStatus";
            int st = Wifi_AssocStatus();
            gWifiStep = "wifiNet_poll";
            if (st == ASSOCSTATUS_ASSOCIATED)
            {
                gWifiStep = "Wifi_GetIP";
                sIp = Wifi_GetIP();
                gWifiStep = "wifiNet_poll";
                sState = WifiNetState::Connected;
                setStatus("Connected to %.20s", sNetwork);
                break;
            }
            if (st == ASSOCSTATUS_ACQUIRINGDHCP && !sInDhcp)
            {
                sInDhcp = true;
                sTimer = DHCP_FRAMES;
                setStatus("Getting an address");
            }
            if (st == ASSOCSTATUS_CANNOTCONNECT)
            {
                nextAttempt(sInDhcp ? "Joined, but no address" : "Password refused?");
                break;
            }
            if (--sTimer <= 0)
                nextAttempt(sInDhcp ? "No address (DHCP timed out)" : "Timed out joining");
            break;
        }

        case WifiNetState::Connected:
            if (Wifi_AssocStatus() != ASSOCSTATUS_ASSOCIATED)
            {
                snprintf(sFailReason, sizeof(sFailReason), "Connection lost");
                beginScan();
            }
            break;

        case WifiNetState::Failed:
            if (--sTimer <= 0)
                beginScan(); // try again by itself
            break;
    }
}

WifiNetState wifiNet_state()
{
    return sState;
}

u32 wifiNet_ip()
{
    return sState == WifiNetState::Connected ? sIp : 0;
}

const char* wifiNet_network()
{
    return sNetwork;
}

const char* wifiNet_status()
{
    return sStatus;
}

const char* wifiNet_lastError()
{
    return sFailReason;
}

int wifiNet_signal()
{
    return sState == WifiNetState::Connected ? Wifi_GetData(WIFIGETDATA_RSSI, 0, nullptr) : 0;
}

void wifiNet_ipString(u32 ip, char* out, size_t size)
{
    snprintf(out, size, "%lu.%lu.%lu.%lu", (unsigned long)(ip & 0xFF), (unsigned long)((ip >> 8) & 0xFF),
             (unsigned long)((ip >> 16) & 0xFF), (unsigned long)((ip >> 24) & 0xFF));
}

const char* wifiNet_securityName(u16 flags)
{
    return securityName(flags);
}
