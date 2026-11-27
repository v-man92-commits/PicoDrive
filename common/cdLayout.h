#pragma once
#include <nds/ndstypes.h>

/// Layout of a CD built from a BIN/CUE image, shared between the ARM9 (which
/// parses the cue sheet) and the ARM7 (which serves it over USB).
///
/// The ARM7 sees all .bin files of the image as one "virtual image": the files
/// back to back, each padded to a 512-byte sector so the normal extent table
/// can map it. Byte offsets below are into that virtual image.

#define CD_MAX_TRACKS       99
#define CD_BOUNCE_BYTES     (20 * 1024)   // ARM7 scratch for reading whole frames
#define CD_TEXT_MAX_BYTES   (454 * 18)    // CD-Text that fits one 8 KiB USB response

typedef enum
{
    CD_TRACK_AUDIO = 0,
    CD_TRACK_MODE1 = 1,
    CD_TRACK_MODE2 = 2      // CD-ROM XA mode 2, read as form 1 (2048 data bytes)
} CdTrackMode;

typedef struct
{
    u8 number;          // 1..99
    u8 mode;            // CdTrackMode
    u16 sectorSize;     // bytes per frame in the image: 2352, 2048 or 2336
    u16 userOffset;     // where the 2048 data bytes start in a frame (16, 24, 8 or 0)
    u16 reserved;
    u32 index0;         // disc LBA of INDEX 00 (start of the pregap); = index1 if none
    u32 index1;         // disc LBA of INDEX 01: the track start listed in the TOC
    u32 dataLba;        // first disc LBA stored in the image for this track
    u32 frames;         // frames stored from dataLba on (gaps outside: silence/zeros)
    u32 byteOffset;     // virtual-image byte offset of the frame at dataLba
} cd_track_t;

typedef struct
{
    u32 trackCount;
    u32 leadout;            // disc LBA of the lead-out = disc length in frames
    const u8* cdText;       // raw CD-Text packs (18 bytes each) in main RAM, or null
    u32 cdTextLength;
    u8* bounce;             // CD_BOUNCE_BYTES of main RAM the ARM7 may use as scratch
    u32 bounceSize;
    u8 discType;            // 0x00 CD-DA / CD-ROM, 0x20 CD-ROM XA
    u8 reserved[3];
    cd_track_t tracks[CD_MAX_TRACKS];
} cd_layout_t;
