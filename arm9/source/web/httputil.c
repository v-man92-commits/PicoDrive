// httputil.c - small HTTP helpers for wifi-probe (portable, host-testable).
#include "httputil.h"

#include <ctype.h>
#include <string.h>

int http_header_complete(const char *buf, size_t len)
{
    if (len < 4)
        return 0;
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return 1;
    }
    return 0;
}

static void copy_bounded(char *dst, size_t dstsize, const char *src, size_t n)
{
    if (dstsize == 0)
        return;
    if (n >= dstsize)
        n = dstsize - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

int http_parse_request_line(const char *buf, size_t len, http_request_t *req)
{
    memset(req, 0, sizeof(*req));

    // End of the first line
    size_t eol = 0;
    while (eol < len && buf[eol] != '\r' && buf[eol] != '\n')
        eol++;
    if (eol == len)
        return -1;

    // Method
    size_t i = 0;
    while (i < eol && buf[i] != ' ')
        i++;
    if (i == 0 || i == eol || i >= sizeof(req->method))
        return -1;
    copy_bounded(req->method, sizeof(req->method), buf, i);

    // Target
    size_t t0 = i + 1;
    size_t t1 = t0;
    while (t1 < eol && buf[t1] != ' ')
        t1++;
    if (t1 == t0 || buf[t0] != '/')
        return -1;

    // Must be followed by " HTTP/"
    if (t1 + 6 > eol || memcmp(buf + t1, " HTTP/", 6) != 0)
        return -1;

    size_t q = t0;
    while (q < t1 && buf[q] != '?')
        q++;
    if (q - t0 >= sizeof(req->path))
        return -1;
    copy_bounded(req->path, sizeof(req->path), buf + t0, q - t0);
    if (q < t1)
        copy_bounded(req->query, sizeof(req->query), buf + q + 1, t1 - q - 1);
    return 0;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

size_t http_url_decode(const char *src, size_t srclen, char *dst, size_t dstsize)
{
    size_t o = 0;
    if (dstsize == 0)
        return 0;
    for (size_t i = 0; i < srclen && o + 1 < dstsize; i++) {
        char c = src[i];
        if (c == '+') {
            c = ' ';
        } else if (c == '%' && i + 2 < srclen && hexval(src[i + 1]) >= 0 && hexval(src[i + 2]) >= 0) {
            c = (char)(hexval(src[i + 1]) * 16 + hexval(src[i + 2]));
            i += 2;
        }
        dst[o++] = c;
    }
    dst[o] = '\0';
    return o;
}

int http_query_get(const char *query, const char *key, char *out, size_t outsize)
{
    size_t klen = strlen(key);
    const char *p = query;
    if (outsize)
        out[0] = '\0';
    while (*p) {
        const char *amp = strchr(p, '&');
        size_t plen = amp ? (size_t)(amp - p) : strlen(p);
        if (plen > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            http_url_decode(p + klen + 1, plen - klen - 1, out, outsize);
            return 1;
        }
        if (plen == klen && strncmp(p, key, klen) == 0)
            return 1; // key present with empty value
        if (!amp)
            break;
        p = amp + 1;
    }
    return 0;
}

size_t http_html_escape(const char *src, char *dst, size_t dstsize)
{
    size_t o = 0;
    if (dstsize == 0)
        return 0;
    for (; *src; src++) {
        const char *rep = NULL;
        switch (*src) {
            case '<': rep = "&lt;"; break;
            case '>': rep = "&gt;"; break;
            case '&': rep = "&amp;"; break;
            case '"': rep = "&quot;"; break;
            case '\'': rep = "&#39;"; break;
            default: break;
        }
        size_t n = rep ? strlen(rep) : 1;
        if (o + n + 1 > dstsize)
            break;
        if (rep)
            memcpy(dst + o, rep, n);
        else
            dst[o] = *src;
        o += n;
    }
    dst[o] = '\0';
    return o;
}

void http_sanitize_ascii(char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c > 0x7E)
            *s = '?';
    }
}

int http_header_get(const char *buf, size_t len, const char *name, char *out, size_t outsize)
{
    size_t nlen = strlen(name);
    if (outsize)
        out[0] = '\0';
    // skip the request line
    size_t i = 0;
    while (i < len && buf[i] != '\n')
        i++;
    i++;
    while (i < len)
    {
        size_t ls = i;
        while (i < len && buf[i] != '\n')
            i++;
        size_t le = i;
        i++;
        if (le > ls && buf[le - 1] == '\r')
            le--;
        if (le == ls)
            break; // end of headers
        if (le - ls > nlen && buf[ls + nlen] == ':')
        {
            size_t k = 0;
            while (k < nlen && tolower((unsigned char)buf[ls + k]) == tolower((unsigned char)name[k]))
                k++;
            if (k == nlen)
            {
                size_t vs = ls + nlen + 1;
                while (vs < le && (buf[vs] == ' ' || buf[vs] == '\t'))
                    vs++;
                size_t ve = le;
                while (ve > vs && (buf[ve - 1] == ' ' || buf[ve - 1] == '\t'))
                    ve--;
                copy_bounded(out, outsize, buf + vs, ve - vs);
                return 1;
            }
        }
    }
    return 0;
}

int http_cookie_get(const char *cookies, const char *name, char *out, size_t outsize)
{
    size_t nlen = strlen(name);
    const char *p = cookies;
    if (outsize)
        out[0] = '\0';
    while (*p)
    {
        while (*p == ' ' || *p == ';')
            p++;
        const char *end = strchr(p, ';');
        size_t plen = end ? (size_t)(end - p) : strlen(p);
        if (plen > nlen && strncmp(p, name, nlen) == 0 && p[nlen] == '=')
        {
            copy_bounded(out, outsize, p + nlen + 1, plen - nlen - 1);
            return 1;
        }
        if (!end)
            break;
        p = end + 1;
    }
    return 0;
}
