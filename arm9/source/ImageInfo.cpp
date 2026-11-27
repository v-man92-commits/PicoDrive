#include "common.h"
#include <stdio.h>
#include <string.h>
#include "ImageInfo.h"

alignas(32) static u8 sBuf[512];

static inline u16 rd16(const u8* p) { return p[0] | (p[1] << 8); }
static inline u32 rd32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
static inline u32 rd32be(const u8* p) { return ((u32)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

static bool readAt(FILE* f, u32 offset, u32 fileSize, u8* out, u32 len)
{
    if (offset >= 0x7FFFFFFF || offset + len > fileSize)
        return false;
    if (fseek(f, (long)offset, SEEK_SET) != 0)
        return false;
    return fread(out, 1, len, f) == len;
}

static void copyLabel(char* dst, const u8* src, u32 len)
{
    u32 n = 0;
    for (u32 i = 0; i < len && n < 15; i++)
    {
        char c = (char)src[i];
        dst[n++] = (c >= 32 && c < 127) ? c : ' ';
    }
    while (n > 0 && dst[n - 1] == ' ')
        n--;
    dst[n] = 0;
    if (!strcmp(dst, "NO NAME"))
        dst[0] = 0;
}

static void setFormat(ImageFormat& out, const char* name, const char* detail = "")
{
    strncpy(out.name, name, sizeof(out.name) - 1);
    out.name[sizeof(out.name) - 1] = 0;
    strncpy(out.detail, detail, sizeof(out.detail) - 1);
    out.detail[sizeof(out.detail) - 1] = 0;
}

static void sizeString(char* out, u32 bytes)
{
    if (bytes >= 1024)
        sprintf(out, "%lu KiB clusters", bytes / 1024);
    else
        sprintf(out, "%lu byte clusters", bytes);
}

static const char* partitionTypeName(u8 type)
{
    switch (type)
    {
        case 0x01: return "FAT12";
        case 0x04: case 0x06: case 0x0E: return "FAT16";
        case 0x0B: case 0x0C: return "FAT32";
        case 0x07: return "NTFS/exFAT";
        case 0x05: case 0x0F: return "Extended";
        case 0x83: return "Linux";
        case 0x82: return "Linux swap";
        case 0xAF: return "HFS+";
        case 0xEE: return "GPT";
        default: return nullptr;
    }
}

// Identify a FAT/exFAT/NTFS volume boot record. Returns false if it isn't one.
static bool detectVolume(const u8* s, ImageFormat& out, const char* prefix)
{
    char name[30];
    if (!memcmp(s + 3, "EXFAT   ", 8))
    {
        sprintf(name, "%sexFAT", prefix);
        setFormat(out, name);
        return true;
    }
    if (!memcmp(s + 3, "NTFS    ", 8))
    {
        sprintf(name, "%sNTFS", prefix);
        setFormat(out, name);
        return true;
    }
    if (s[0x1FE] != 0x55 || s[0x1FF] != 0xAA)
        return false;
    if (s[0] != 0xEB && s[0] != 0xE9)
        return false;
    u32 bps = rd16(s + 0x0B);
    u32 spc = s[0x0D];
    if (bps < 512 || bps > 4096 || spc == 0)
        return false;

    char detail[30];
    sizeString(detail, bps * spc);
    if (!memcmp(s + 0x52, "FAT32", 5) || (rd16(s + 0x16) == 0 && rd32(s + 0x24) != 0))
    {
        sprintf(name, "%sFAT32", prefix);
        copyLabel(out.label, s + 0x47, 11);
    }
    else if (!memcmp(s + 0x36, "FAT12", 5))
    {
        sprintf(name, "%sFAT12", prefix);
        copyLabel(out.label, s + 0x2B, 11);
    }
    else
    {
        sprintf(name, "%sFAT16", prefix);
        copyLabel(out.label, s + 0x2B, 11);
    }
    setFormat(out, name, detail);
    return true;
}

bool imageInfo_detect(const char* path, u32 fileSize, ImageFormat& out)
{
    memset(&out, 0, sizeof(out));
    FILE* f = fopen(path, "rb");
    if (!f)
    {
        setFormat(out, "Unreadable");
        return false;
    }

    bool ok = true;
    do
    {
        if (fileSize < 512 || !readAt(f, 0, fileSize, sBuf, 512))
        {
            setFormat(out, fileSize < 512 ? "Too small" : "Read error");
            ok = fileSize < 512;
            break;
        }

        // Original Xbox: FATX superblock at offset 0 (memory units, USB drives)
        if (!memcmp(sBuf, "FATX", 4))
        {
            char detail[30];
            sizeString(detail, rd32(sBuf + 8) * 512);
            setFormat(out, "FATX (Xbox memory unit)", detail);
            break;
        }
        // Xbox 360: big-endian FATX ("XTAF")
        if (!memcmp(sBuf, "XTAF", 4))
        {
            char detail[30];
            sizeString(detail, rd32be(sBuf + 8) * 512);
            setFormat(out, "XTAF (Xbox 360 FATX)", detail);
            break;
        }

        // ISO9660 / UDF volume descriptors at 32 KiB. Checked before boot
        // records and partition tables: most Linux installer ISOs are
        // "hybrid" (an MBR/GPT at sector 0 so the same file also boots from a
        // USB stick) and must still default to the CD/DVD drive.
        {
            alignas(4) static u8 vd[512];
            bool hybrid = sBuf[0x1FE] == 0x55 && sBuf[0x1FF] == 0xAA;
            if (readAt(f, 0x8000, fileSize, vd, 512))
            {
                if (!memcmp(vd + 1, "CD001", 5))
                {
                    setFormat(out, hybrid ? "ISO9660 (hybrid)" : "ISO9660 disc image",
                        hybrid ? "Also boots as USB disk: L+A" : "");
                    copyLabel(out.label, vd + 40, 15);
                    out.isIso = true;
                    break;
                }
                if (!memcmp(vd + 1, "BEA01", 5) || !memcmp(vd + 1, "NSR0", 4))
                {
                    setFormat(out, hybrid ? "UDF disc image (hybrid)" : "UDF disc image");
                    out.isIso = true;
                    break;
                }
            }
        }

        // Volume boot record without a partition table ("superfloppy")
        u8 first = sBuf[0];
        bool blank = true;
        for (u32 i = 0; i < 512; i++)
        {
            if (sBuf[i] != first)
            {
                blank = false;
                break;
            }
        }
        if (detectVolume(sBuf, out, ""))
            break;

        // Partition table
        if (sBuf[0x1FE] == 0x55 && sBuf[0x1FF] == 0xAA)
        {
            const u8* pt = sBuf + 0x1BE;
            u8 firstType = 0;
            u32 firstLba = 0;
            int count = 0;
            for (int i = 0; i < 4; i++)
            {
                u8 type = pt[i * 16 + 4];
                if (type == 0)
                    continue;
                if (count == 0)
                {
                    firstType = type;
                    firstLba = rd32(pt + i * 16 + 8);
                }
                count++;
            }
            if (count > 0)
            {
                if (firstType == 0xEE)
                {
                    setFormat(out, "GPT disk");
                    break;
                }
                char name[30];
                char detail[30];
                // look at the first partition's boot record for the real filesystem
                u64 off = (u64)firstLba * 512;
                ImageFormat inner;
                memset(&inner, 0, sizeof(inner));
                if (off + 512 <= fileSize && readAt(f, (u32)off, fileSize, sBuf, 512) && detectVolume(sBuf, inner, ""))
                {
                    sprintf(name, "MBR: %s", inner.name);
                    strcpy(out.label, inner.label);
                }
                else
                {
                    const char* tn = partitionTypeName(firstType);
                    if (tn)
                        sprintf(name, "MBR: %s", tn);
                    else
                        sprintf(name, "MBR: type %02X", firstType);
                }
                sprintf(detail, "%d partition%s", count, count == 1 ? "" : "s");
                setFormat(out, name, detail);
                break;
            }
        }

        // Original Xbox hard disk layout: FATX partitions at fixed offsets
        if (readAt(f, 0x80000, fileSize, sBuf, 512) && !memcmp(sBuf, "FATX", 4))
        {
            setFormat(out, "Xbox HDD (FATX partitions)");
            break;
        }

        // Xbox game disc (XDVDFS) magic at sector 32 of the game partition
        if (readAt(f, 0x10000, fileSize, sBuf, 20) && !memcmp(sBuf, "MICROSOFT*XBOX*MEDIA", 20))
        {
            setFormat(out, "XISO (Xbox game disc)");
            out.isIso = true;
            break;
        }

        if (blank)
            setFormat(out, first == 0 ? "Blank (unformatted)" : "Blank (filled)", "Format it on the host");
        else
            setFormat(out, "Unknown / raw data");
    } while (false);

    fclose(f);
    return ok;
}
