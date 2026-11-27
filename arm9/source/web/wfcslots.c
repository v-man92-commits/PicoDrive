// wfcslots.c - decode the DSi's saved WiFi connections 4-6 (portable).
//
// Layout as read by DSWiFi's Wifi_TWL_GetWfcSettings():
//   040h 32 bytes  SSID
//   0C0h u32 x4    IP, gateway, DNS1, DNS2 (0 = automatic)
//   0D0h u8        subnet mask bits
//   0E6h u8        WEP mode
//   0E7h u8        WPA mode
//   0EFh u8        nonzero = slot in use
//   100h 32 bytes  PMK (precomputed key)
//   120h 64 bytes  passphrase
#include "wfcslots.h"

#include <string.h>

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int wfc_decode_slot(const uint8_t *b, wfc_slot_t *out)
{
    memset(out, 0, sizeof(*out));
    out->configured = b[0xEF] != 0;
    if (!out->configured)
        return -1;

    memcpy(out->ssid, b + 0x40, 32);
    out->ssid[32] = '\0';
    out->wep_mode = b[0xE6];
    out->wpa_mode = b[0xE7];
    out->ip = rd32(b + 0xC0);
    out->gateway = rd32(b + 0xC4);
    out->dns1 = rd32(b + 0xC8);
    out->dns2 = rd32(b + 0xCC);
    out->mask_bits = b[0xD0];

    int n = 0;
    while (n < 64 && b[0x120 + n] != 0)
        n++;
    memcpy(out->passphrase, b + 0x120, (size_t)n);
    out->passphrase[n] = '\0';
    out->passphrase_len = n;
    return 0;
}

uint32_t wfc_dsi_slots_address(uint16_t w)
{
    return (uint32_t)w * 8 - 0xA00;
}

const char *wfc_wpa_name(uint8_t mode)
{
    switch (mode) {
        case 0: return "none/WEP";
        case 4: return "WPA-TKIP";
        case 5: return "WPA2-TKIP";
        case 6: return "WPA-AES";
        case 7: return "WPA2-AES";
        default: return "unknown";
    }
}
