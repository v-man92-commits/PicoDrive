#include <string.h>
#include "ndsFifoProto.h"

#define HDR_FLAG (1u << 26)

int ndsfifo_encode(int type, uint32_t channel, const uint8_t* payload, uint32_t length, uint32_t* words)
{
    if (channel >= NDSFIFO_CHANNELS || type < 0 || type > NDSFIFO_TYPE_DATAMSG)
        return 0;
    if (type != NDSFIFO_TYPE_DATAMSG && length != 4)
        return 0;
    if (length > NDSFIFO_MAX_DATAMSG)
        return 0;
    int n = 0;
    words[n++] = HDR_FLAG | ((uint32_t)type << 24) | (channel << 20) | length;
    for (uint32_t i = 0; i < length; i += 2)
    {
        uint32_t lo = payload[i];
        uint32_t hi = (i + 1 < length) ? payload[i + 1] : 0;
        words[n++] = lo | (hi << 8);
    }
    return n;
}

void ndsfifo_decoderReset(ndsfifo_decoder_t* d)
{
    memset(d, 0, sizeof(*d));
}

bool ndsfifo_decode(ndsfifo_decoder_t* d, uint32_t word)
{
    if (word & HDR_FLAG)
    {
        // A new header always starts a new message (an unfinished one is lost).
        uint32_t type = (word >> 24) & 3;
        uint32_t length = word & 0xFFFF;
        d->active = false;
        if (type > NDSFIFO_TYPE_DATAMSG || length > NDSFIFO_MAX_DATAMSG ||
            (type != NDSFIFO_TYPE_DATAMSG && length != 4))
            return false;
        d->type = (uint8_t)type;
        d->channel = (uint8_t)((word >> 20) & 0xF);
        d->length = (uint16_t)length;
        d->received = 0;
        d->active = length != 0;
        return length == 0; // an empty datamsg is complete at once
    }
    if (!d->active)
        return false; // stray payload word
    d->data[d->received++] = (uint8_t)word;
    if (d->received < d->length)
        d->data[d->received++] = (uint8_t)(word >> 8);
    if (d->received >= d->length)
    {
        d->active = false;
        return true;
    }
    return false;
}
