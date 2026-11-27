// wificonfig.c - parse wifi.txt (portable, host-testable).
#include "wificonfig.h"

#include <ctype.h>
#include <string.h>

static int key_is(const char *k, size_t klen, const char *name)
{
    size_t n = strlen(name);
    if (klen != n)
        return 0;
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)k[i]) != name[i])
            return 0;
    }
    return 1;
}

static void store(char *dst, size_t dstsize, const char *v, size_t vlen)
{
    if (vlen >= dstsize)
        vlen = dstsize - 1;
    memcpy(dst, v, vlen);
    dst[vlen] = '\0';
}

int wifi_parse_ipv4(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    for (int part = 0; part < 4; part++) {
        if (!isdigit((unsigned char)*s))
            return -1;
        unsigned n = 0;
        int digits = 0;
        while (isdigit((unsigned char)*s)) {
            n = n * 10 + (unsigned)(*s - '0');
            if (++digits > 3 || n > 255)
                return -1;
            s++;
        }
        v |= (uint32_t)n << (8 * part);
        if (part < 3) {
            if (*s != '.')
                return -1;
            s++;
        }
    }
    if (*s != '\0')
        return -1;
    *out = v;
    return 0;
}

static int parse_ip_value(const char *v, size_t vlen, uint32_t *out)
{
    char tmp[20];
    if (vlen == 0 || vlen >= sizeof(tmp))
        return -1;
    memcpy(tmp, v, vlen);
    tmp[vlen] = '\0';
    return wifi_parse_ipv4(tmp, out);
}

int wifi_config_parse(const char *text, size_t len, wifi_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    size_t i = 0;

    // Skip a UTF-8 BOM (Notepad likes adding one)
    if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF)
        i = 3;

    while (i < len) {
        size_t ls = i;
        while (i < len && text[i] != '\n' && text[i] != '\r')
            i++;
        size_t le = i;
        while (i < len && (text[i] == '\n' || text[i] == '\r'))
            i++;

        // trim
        while (ls < le && isspace((unsigned char)text[ls]))
            ls++;
        while (le > ls && isspace((unsigned char)text[le - 1]))
            le--;
        if (ls == le || text[ls] == '#' || text[ls] == ';')
            continue;

        size_t eq = ls;
        while (eq < le && text[eq] != '=')
            eq++;
        if (eq == le)
            continue;

        size_t ke = eq;
        while (ke > ls && isspace((unsigned char)text[ke - 1]))
            ke--;
        size_t vs = eq + 1;
        while (vs < le && isspace((unsigned char)text[vs]))
            vs++;
        size_t ve = le;
        if (ve - vs >= 2 && text[vs] == '"' && text[ve - 1] == '"') {
            vs++;
            ve--;
        }

        const char *k = text + ls;
        size_t klen = ke - ls;
        if (key_is(k, klen, "ssid")) {
            store(cfg->ssid, sizeof(cfg->ssid), text + vs, ve - vs);
            cfg->has_ssid = cfg->ssid[0] != '\0';
        } else if (key_is(k, klen, "password") || key_is(k, klen, "pass") ||
                   key_is(k, klen, "key")) {
            store(cfg->password, sizeof(cfg->password), text + vs, ve - vs);
        } else if (key_is(k, klen, "ip")) {
            if (parse_ip_value(text + vs, ve - vs, &cfg->ip) == 0 && cfg->ip != 0)
                cfg->has_static_ip = 1;
        } else if (key_is(k, klen, "gateway")) {
            parse_ip_value(text + vs, ve - vs, &cfg->gateway);
        } else if (key_is(k, klen, "netmask")) {
            parse_ip_value(text + vs, ve - vs, &cfg->netmask);
        } else if (key_is(k, klen, "dns")) {
            parse_ip_value(text + vs, ve - vs, &cfg->dns);
        }
    }
    if (cfg->has_static_ip) {
        if (cfg->gateway == 0)
            cfg->gateway = (cfg->ip & 0x00FFFFFFu) | (1u << 24);
        if (cfg->netmask == 0)
            cfg->netmask = 0x00FFFFFFu; // 255.255.255.0
        if (cfg->dns == 0)
            cfg->dns = cfg->gateway;
    } else {
        cfg->gateway = cfg->netmask = cfg->dns = 0;
    }
    return (cfg->has_ssid || cfg->has_static_ip) ? 0 : -1;
}
