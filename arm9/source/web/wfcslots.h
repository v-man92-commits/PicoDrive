// wfcslots.h - decode the DSi's saved WiFi connections 4-6 (portable).
#ifndef WFCSLOTS_H
#define WFCSLOTS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WFC_SLOT_SIZE 0x200

typedef struct {
    int      configured;
    char     ssid[33];
    uint8_t  wep_mode;
    uint8_t  wpa_mode;      // 0 = none/WEP, 4..7 = WPA/WPA2 with TKIP/AES (see wfc_wpa_name)
    char     passphrase[65];
    int      passphrase_len;
    uint32_t ip, gateway, dns1, dns2; // raw (network byte order), 0 = automatic
    uint8_t  mask_bits;
} wfc_slot_t;

// Decodes one 512-byte slot. Returns 0 if it holds a saved connection.
int wfc_decode_slot(const uint8_t *b, wfc_slot_t *out);

// Firmware address of DSi connection slot 4 given the u16 at firmware 0x20.
uint32_t wfc_dsi_slots_address(uint16_t fw_word_0x20);

const char *wfc_wpa_name(uint8_t mode);

#ifdef __cplusplus
}
#endif

#endif
