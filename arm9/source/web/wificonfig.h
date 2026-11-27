// wificonfig.h - parse wifi.txt (portable, host-testable).
#ifndef WIFICONFIG_H
#define WIFICONFIG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef struct {
    char ssid[33];
    char password[65];
    int  has_ssid;
    // Optional fixed address (all in network byte order, like Wifi_GetIP()).
    int      has_static_ip;
    uint32_t ip, gateway, netmask, dns;
} wifi_config_t;

// Parses "a.b.c.d" into network byte order (first octet in the lowest byte).
// Returns 0 on success, -1 if malformed.
int wifi_parse_ipv4(const char *s, uint32_t *out);

// Parses text such as:
//   ssid=MyNetwork
//   password=hunter22
// Lines starting with '#' or ';' are comments. Keys are case-insensitive.
// Whitespace around keys and the value ends is trimmed; a value may be
// wrapped in double quotes to keep leading/trailing spaces.
// Optional keys: ip=, gateway=, netmask=, dns= (fixed address instead of
// DHCP). Missing gateway defaults to a.b.c.1, netmask to 255.255.255.0 and
// dns to the gateway.
// Returns 0 if an SSID or a fixed ip= was found, -1 otherwise.
int wifi_config_parse(const char *text, size_t len, wifi_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif
