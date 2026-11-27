#pragma once

/// Identifies what is inside an image file (shown on the top screen).
struct ImageFormat
{
    char name[30];     // e.g. "FATX (Xbox memory unit)"
    char detail[30];   // e.g. "16 KiB clusters"
    char label[16];    // volume label if known
    bool isIso;        // ISO9660 / UDF optical image
};

/// Reads the start of the file through libfat and identifies it.
/// Returns false if the file couldn't be read.
bool imageInfo_detect(const char* path, u32 fileSize, ImageFormat& out);
