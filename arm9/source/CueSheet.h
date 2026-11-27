#pragma once
#include <nds/ndstypes.h>
#include "cdLayout.h"

/// CDRWIN cue sheet parser for BIN/CUE CD images (single .bin or one per
/// track), and the disc layout the ARM7 serves (see cdLayout.h).

#define CUE_MAX_FILES     CD_MAX_TRACKS
#define CUE_NAME_LEN      128

enum class CueError
{
    None,
    Syntax,
    NoTracks,
    TooManyTracks,
    TooManyFiles,
    NameTooLong,
    UnsupportedFile,    // FILE type other than BINARY/MOTOROLA (e.g. WAVE, MP3)
    UnsupportedMode,    // e.g. CDG, MODE2/2324
    TrackBeforeFile,
    MissingIndex1,
    BadIndexOrder,
    TrackPastEndOfFile,
    EmptyTrack,
    MissingFile,        // a FILE the sheet names isn't on the SD card
    Unreadable          // the .cue itself couldn't be read (or is too big)
};

const char* cue_errorString(CueError e);

struct CueTrack
{
    u8 number;
    u8 mode;            // CdTrackMode
    u16 sectorSize;
    u16 userOffset;
    u8 file;            // index into CueSheet::files
    bool motorola;      // big-endian audio (FILE ... MOTOROLA)
    s32 index0;         // frames from the start of its file, -1 if absent
    s32 index1;
    u32 pregap;         // PREGAP frames (silence not in the file)
    u32 postgap;        // POSTGAP frames
};

struct CueSheet
{
    u32 fileCount;
    char files[CUE_MAX_FILES][CUE_NAME_LEN];
    u32 trackCount;
    CueTrack tracks[CD_MAX_TRACKS];
    char cdTextFile[CUE_NAME_LEN];  // CDTEXTFILE, or empty
    char title[32];                 // TITLE (disc), or empty
};

/// Parses cue sheet text. errorLine (optional) gets the 1-based line of a
/// syntax problem.
CueError cue_parse(const char* text, u32 length, CueSheet& out, u32* errorLine = nullptr);

/// Builds the disc layout from a parsed sheet and the sizes of its files.
/// fileStartSector[i] receives where file i starts in the virtual image (in
/// 512-byte sectors); totalSectors the virtual image size.
CueError cue_buildLayout(const CueSheet& sheet, const u64* fileSizes, cd_layout_t& layout,
    u32* fileStartSector, u32& totalSectors);

/// Number of audio tracks in a layout.
u32 cue_audioTracks(const cd_layout_t& layout);

/// Puts CD-Text read from a .cdt file into order (some tools store the pack
/// stream rotated) and fixes pack CRCs. scratch must hold `length` bytes.
/// Returns the new length (whole packs), or 0 if it isn't CD-Text.
u32 cue_normalizeCdText(u8* data, u32 length, u8* scratch);

/// Album title from CD-Text packs (pack type 0x80, track 0), or empty.
void cue_cdTextTitle(const u8* packs, u32 length, char* out, u32 outSize);
