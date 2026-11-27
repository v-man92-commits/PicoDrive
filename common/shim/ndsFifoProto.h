// Wire format for carrying libnds FIFO messages over one libtwl IPC channel.
// Portable (no hardware access) so it can be tested on the host.
//
// libtwl messages carry 27 data bits. A libnds message is sent as:
//   header : bit26 = 1, bits 25-24 = type, bits 23-20 = libnds channel,
//            bits 15-0 = payload length in bytes (datamsg) or 4
//   payload: bit26 = 0, bits 15-0 = next 16 payload bits (little endian)
// Value32 and address messages are 3 words; a datamsg is 1 + ceil(len/2).
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define NDSFIFO_TYPE_VALUE32   0
#define NDSFIFO_TYPE_ADDRESS   1
#define NDSFIFO_TYPE_DATAMSG   2

#define NDSFIFO_CHANNELS       16
#define NDSFIFO_MAX_DATAMSG    24   // keeps a message within the 16-word hardware FIFO
#define NDSFIFO_MAX_WORDS      (1 + (NDSFIFO_MAX_DATAMSG + 1) / 2)

#ifdef __cplusplus
extern "C" {
#endif

// Encodes one message into words[] (at least NDSFIFO_MAX_WORDS long).
// Returns the number of words, or 0 if the message can't be encoded.
int ndsfifo_encode(int type, uint32_t channel, const uint8_t* payload, uint32_t length, uint32_t* words);

typedef struct
{
    bool     active;
    uint8_t  type;
    uint8_t  channel;
    uint16_t length;
    uint16_t received;
    uint8_t  data[NDSFIFO_MAX_DATAMSG];
} ndsfifo_decoder_t;

void ndsfifo_decoderReset(ndsfifo_decoder_t* d);

// Feeds one 27-bit word. Returns true when a complete message is in d
// (type/channel/length/data); it stays there until the next word is fed.
// Words that don't fit the expected sequence are dropped.
bool ndsfifo_decode(ndsfifo_decoder_t* d, uint32_t word);

static inline uint32_t ndsfifo_u32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#ifdef __cplusplus
}
#endif
