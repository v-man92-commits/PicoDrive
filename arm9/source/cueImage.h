#pragma once
#include <nds/ndstypes.h>
#include <stddef.h>
#include "CueSheet.h"

/// BIN/CUE images on the SD card: reading the .cue, finding its files.

#define CUE_MAX_TEXT (32 * 1024)    // bigger .cue files are refused

/// True for "*.cue".
bool cueImage_isCueName(const char* name);

/// True if a FILE/CDTEXTFILE name refers to the same folder as the .cue
/// (no path separators), so the file list can hide it.
bool cueImage_isLocalName(const char* name);

/// dir + "/" + name, with '\\' turned into '/'.
void cueImage_path(char* out, size_t outSize, const char* dir, const char* name);

/// Reads and parses dir/cueName. Only the sheet (no file sizes).
CueError cueImage_parse(const char* dir, const char* cueName, CueSheet& sheet);

/// cueImage_parse plus the size of every file it names. missingFile gets the
/// index of a missing file (CueError::MissingFile).
CueError cueImage_load(const char* dir, const char* cueName, CueSheet& sheet, u64* sizes, int& missingFile);

/// Loads and normalizes the sheet's CDTEXTFILE (see cue_normalizeCdText) into
/// buf (CD_TEXT_MAX_BYTES). Returns its length, 0 if none or unusable.
u32 cueImage_loadCdText(const char* dir, const CueSheet& sheet, u8* buf);
