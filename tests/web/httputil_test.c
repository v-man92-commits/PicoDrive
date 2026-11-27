// Host test for arm9/source/web/httputil.c (request parsing, headers, cookies).
#include <stdio.h>
#include <string.h>
#include "httputil.h"
static int pass, fails;
#define CHECK(c) do { if (c) pass++; else { fails++; printf("  FAIL %d: %s\n", __LINE__, #c); } } while (0)
int main(void)
{
    const char *r = "POST /api/mount?name=A%20B.img&mode=cd HTTP/1.1\r\nHost: 192.168.1.5\r\ncookie:  a=1; imsess=abcdef0123456789 ; z=9\r\nX-Empty:\r\n\r\nbody";
    size_t n = strlen(r);
    http_request_t q;
    CHECK(http_header_complete(r, n));
    CHECK(http_parse_request_line(r, n, &q) == 0 && !strcmp(q.method, "POST") && !strcmp(q.path, "/api/mount"));
    char v[64];
    CHECK(http_query_get(q.query, "name", v, sizeof v) && !strcmp(v, "A B.img"));
    CHECK(http_query_get(q.query, "mode", v, sizeof v) && !strcmp(v, "cd"));
    CHECK(http_header_get(r, n, "Host", v, sizeof v) && !strcmp(v, "192.168.1.5"));
    CHECK(http_header_get(r, n, "COOKIE", v, sizeof v) && !strcmp(v, "a=1; imsess=abcdef0123456789 ; z=9"));
    CHECK(http_header_get(r, n, "X-Empty", v, sizeof v) && v[0] == 0);
    CHECK(!http_header_get(r, n, "Content-Length", v, sizeof v) && v[0] == 0);
    CHECK(!http_header_get(r, n, "Hos", v, sizeof v));                 // prefix isn't a match
    CHECK(!http_header_get(r, n, "body", v, sizeof v));                // body isn't a header
    char c[80];
    http_header_get(r, n, "Cookie", c, sizeof c);
    CHECK(http_cookie_get(c, "imsess", v, sizeof v) && !strcmp(v, "abcdef0123456789 "));
    CHECK(http_cookie_get(c, "a", v, sizeof v) && !strcmp(v, "1"));
    CHECK(http_cookie_get(c, "z", v, sizeof v) && !strcmp(v, "9"));
    CHECK(!http_cookie_get(c, "imses", v, sizeof v));
    CHECK(!http_cookie_get("", "a", v, sizeof v));
    char small[4];
    CHECK(http_header_get(r, n, "Host", small, sizeof small) && !strcmp(small, "192"));  // truncated safely
    // header lookup stops at the end of the buffer even without a blank line
    const char *cut = "GET / HTTP/1.1\r\nHost: x";
    CHECK(http_header_get(cut, strlen(cut), "Host", v, sizeof v) && !strcmp(v, "x"));
    CHECK(!http_header_get("GET / HTTP/1.1", 14, "Host", v, sizeof v));
    printf("httputil: %d checks passed, %d failed\n", pass, fails);
    return fails ? 1 : 0;
}
