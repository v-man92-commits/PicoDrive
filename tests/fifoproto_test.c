// Host test for common/shim/ndsFifoProto.c (libnds FIFO over libtwl wire format).
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ndsFifoProto.h"
static int pass, fails;
#define CHECK(c, ...) do { if (c) pass++; else { fails++; printf("  FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int roundtrip(int type, unsigned ch, const uint8_t* p, unsigned len, ndsfifo_decoder_t* out)
{
    uint32_t w[NDSFIFO_MAX_WORDS];
    int n = ndsfifo_encode(type, ch, p, len, w);
    if (!n) return -1;
    ndsfifo_decoder_t d; ndsfifo_decoderReset(&d);
    int done = 0;
    for (int i = 0; i < n; i++) {
        if (w[i] >> 27) return -2;            // must fit libtwl's 27 data bits
        if (ndsfifo_decode(&d, w[i])) { done++; if (i != n - 1) return -3; }
    }
    *out = d;
    return done == 1 ? n : -4;
}

int main(void)
{
    ndsfifo_decoder_t d;
    uint8_t v[4] = { 0x78, 0x56, 0x34, 0x12 };
    CHECK(roundtrip(NDSFIFO_TYPE_VALUE32, 15, v, 4, &d) == 3 && d.type == 0 && d.channel == 15 && ndsfifo_u32(d.data) == 0x12345678, "value32");
    uint8_t a[4] = { 0xFC, 0xFF, 0xFF, 0x02 };
    CHECK(roundtrip(NDSFIFO_TYPE_ADDRESS, 3, a, 4, &d) == 3 && d.type == 1 && ndsfifo_u32(d.data) == 0x02FFFFFC, "address");
    uint8_t all[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    CHECK(roundtrip(NDSFIFO_TYPE_VALUE32, 0, all, 4, &d) == 3 && ndsfifo_u32(d.data) == 0xFFFFFFFF, "all ones");
    for (unsigned len = 0; len <= NDSFIFO_MAX_DATAMSG; len++) {
        uint8_t buf[NDSFIFO_MAX_DATAMSG];
        for (unsigned i = 0; i < len; i++) buf[i] = (uint8_t)(i * 37 + len);
        int n = roundtrip(NDSFIFO_TYPE_DATAMSG, len % 16, buf, len, &d);
        CHECK(n == (int)(1 + (len + 1) / 2) && n <= 16 && d.length == len && !memcmp(d.data, buf, len) && d.channel == len % 16, "datamsg len %u n %d", len, n);
    }
    uint32_t w[NDSFIFO_MAX_WORDS];
    uint8_t big[64] = {0};
    CHECK(ndsfifo_encode(NDSFIFO_TYPE_DATAMSG, 1, big, NDSFIFO_MAX_DATAMSG + 1, w) == 0, "too long datamsg refused");
    CHECK(ndsfifo_encode(NDSFIFO_TYPE_VALUE32, 16, v, 4, w) == 0, "bad channel refused");
    CHECK(ndsfifo_encode(NDSFIFO_TYPE_VALUE32, 1, v, 3, w) == 0, "value32 wrong length refused");
    CHECK(ndsfifo_encode(3, 1, v, 4, w) == 0, "bad type refused");

    // a stream of messages decodes back in order; stray words are ignored
    ndsfifo_decoder_t dec; ndsfifo_decoderReset(&dec);
    CHECK(!ndsfifo_decode(&dec, 0x1234), "stray payload ignored");
    srand(7);
    uint32_t sent[200], got[200]; int ns = 0, ng = 0;
    for (int m = 0; m < 200; m++) {
        uint32_t val = (uint32_t)rand() * 2654435761u;
        uint8_t b[4] = { (uint8_t)val, (uint8_t)(val >> 8), (uint8_t)(val >> 16), (uint8_t)(val >> 24) };
        int n = ndsfifo_encode(m & 1, m % 16, b, 4, w);
        sent[ns++] = val;
        for (int i = 0; i < n; i++)
            if (ndsfifo_decode(&dec, w[i])) got[ng++] = ndsfifo_u32(dec.data);
    }
    CHECK(ng == ns && !memcmp(sent, got, sizeof(uint32_t) * ns), "stream %d/%d", ng, ns);
    // a header interrupting a half message starts over cleanly
    int n = ndsfifo_encode(0, 2, v, 4, w);
    ndsfifo_decoderReset(&dec);
    ndsfifo_decode(&dec, w[0]); ndsfifo_decode(&dec, w[1]);
    int ok = 0;
    for (int i = 0; i < n; i++) ok += ndsfifo_decode(&dec, w[i]);
    CHECK(ok == 1 && ndsfifo_u32(dec.data) == 0x12345678, "resync after cut");
    // malformed headers are refused
    ndsfifo_decoderReset(&dec);
    CHECK(!ndsfifo_decode(&dec, (1u << 26) | (3u << 24) | 4) && !dec.active, "type 3 header");
    CHECK(!ndsfifo_decode(&dec, (1u << 26) | (0u << 24) | 5) && !dec.active, "value32 len 5");
    CHECK(!ndsfifo_decode(&dec, (1u << 26) | (2u << 24) | 999) && !dec.active, "huge datamsg");
    CHECK(ndsfifo_decode(&dec, (1u << 26) | (2u << 24) | 0) && dec.length == 0, "empty datamsg");
    printf("fifoproto: %d checks passed, %d failed\n", pass, fails);
    return fails ? 1 : 0;
}
