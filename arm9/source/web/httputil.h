// httputil.h - small HTTP helpers for wifi-probe (portable, host-testable).
#ifndef HTTPUTIL_H
#define HTTPUTIL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char method[8];     // "GET", "HEAD", ...
    char path[128];     // path without query, e.g. "/msg"
    char query[256];    // raw query without '?', may be empty
} http_request_t;

// Returns 1 when buf (len bytes) holds a complete request header ("\r\n\r\n").
int http_header_complete(const char *buf, size_t len);

// Parses the request line. Returns 0 on success, -1 if malformed.
int http_parse_request_line(const char *buf, size_t len, http_request_t *req);

// Finds key in an urlencoded query and URL-decodes its value into out
// (always NUL-terminated, truncated to outsize-1). Returns 1 if found.
int http_query_get(const char *query, const char *key, char *out, size_t outsize);

// URL-decodes src into dst (dstsize includes NUL). '+' becomes a space.
// Returns the decoded length.
size_t http_url_decode(const char *src, size_t srclen, char *dst, size_t dstsize);

// HTML-escapes src into dst. Returns the number of bytes written (excl. NUL).
size_t http_html_escape(const char *src, char *dst, size_t dstsize);

// Finds header `name` (case-insensitive) in a request and copies its value
// (trimmed, truncated to outsize-1). Returns 1 if found.
int http_header_get(const char *buf, size_t len, const char *name, char *out, size_t outsize);

// Finds cookie `name` in a Cookie header value. Returns 1 if found.
int http_cookie_get(const char *cookies, const char *name, char *out, size_t outsize);

// Replaces anything that is not printable ASCII with '?', in place.
void http_sanitize_ascii(char *s);

#ifdef __cplusplus
}
#endif

#endif
