#include "common.h"
#include <string.h>
#include <ctype.h>
#include "CueSheet.h"

namespace
{
    struct ModeInfo
    {
        const char* name;
        u8 mode;
        u16 sectorSize;
        u16 userOffset;
    };

    const ModeInfo sModes[] =
    {
        { "AUDIO",      CD_TRACK_AUDIO, 2352, 0 },
        { "MODE1/2352", CD_TRACK_MODE1, 2352, 16 },
        { "MODE1/2048", CD_TRACK_MODE1, 2048, 0 },
        { "MODE2/2352", CD_TRACK_MODE2, 2352, 24 },
        { "MODE2/2336", CD_TRACK_MODE2, 2336, 8 },
        { "MODE2/2048", CD_TRACK_MODE2, 2048, 0 },
        { "CDI/2352",   CD_TRACK_MODE2, 2352, 24 },
        { "CDI/2336",   CD_TRACK_MODE2, 2336, 8 },
    };

    bool eqNoCase(const char* a, const char* b)
    {
        while (*a && *b)
        {
            if (toupper((u8)*a) != toupper((u8)*b))
                return false;
            a++;
            b++;
        }
        return *a == *b;
    }

    // Splits a line into up to 4 words; a word may be "quoted with spaces".
    u32 splitWords(const char* line, u32 len, char words[4][CUE_NAME_LEN], bool& tooLong)
    {
        u32 count = 0, i = 0;
        tooLong = false;
        while (i < len && count < 4)
        {
            while (i < len && (line[i] == ' ' || line[i] == '\t'))
                i++;
            if (i >= len)
                break;
            u32 n = 0;
            if (line[i] == '"')
            {
                i++;
                while (i < len && line[i] != '"')
                {
                    if (n + 1 < CUE_NAME_LEN)
                        words[count][n++] = line[i];
                    else
                        tooLong = true;
                    i++;
                }
                i++; // closing quote
            }
            else
            {
                while (i < len && line[i] != ' ' && line[i] != '\t')
                {
                    if (n + 1 < CUE_NAME_LEN)
                        words[count][n++] = line[i];
                    else
                        tooLong = true;
                    i++;
                }
            }
            words[count][n] = 0;
            count++;
        }
        return count;
    }

    // "MM:SS:FF" -> frames, -1 if malformed
    s32 parseMsf(const char* s)
    {
        u32 v[3] = { 0, 0, 0 };
        u32 part = 0;
        bool digit = false;
        for (; *s; s++)
        {
            if (*s >= '0' && *s <= '9')
            {
                v[part] = v[part] * 10 + (*s - '0');
                digit = true;
                if (v[part] > 9999)
                    return -1;
            }
            else if (*s == ':' && digit && part < 2)
            {
                part++;
                digit = false;
            }
            else
                return -1;
        }
        if (part != 2 || !digit || v[1] >= 60 || v[2] >= 75)
            return -1;
        return (s32)((v[0] * 60 + v[1]) * 75 + v[2]);
    }

    s32 parseNumber(const char* s)
    {
        if (!*s)
            return -1;
        s32 v = 0;
        for (; *s; s++)
        {
            if (*s < '0' || *s > '9' || v > 100000)
                return -1;
            v = v * 10 + (*s - '0');
        }
        return v;
    }
}

const char* cue_errorString(CueError e)
{
    switch (e)
    {
        case CueError::None:               return "OK";
        case CueError::Syntax:             return "Cue sheet syntax error";
        case CueError::NoTracks:           return "Cue sheet has no tracks";
        case CueError::TooManyTracks:      return "More than 99 tracks";
        case CueError::TooManyFiles:       return "Too many files";
        case CueError::NameTooLong:        return "File name too long";
        case CueError::UnsupportedFile:    return "Only BINARY files supported";
        case CueError::UnsupportedMode:    return "Unsupported track mode";
        case CueError::TrackBeforeFile:    return "TRACK before FILE";
        case CueError::MissingIndex1:      return "Track without INDEX 01";
        case CueError::BadIndexOrder:      return "Track positions out of order";
        case CueError::TrackPastEndOfFile: return "Track past end of .bin";
        case CueError::EmptyTrack:         return "Empty track";
        case CueError::MissingFile:        return "A .bin file is missing";
        case CueError::Unreadable:         return "Can't read the .cue";
    }
    return "?";
}

CueError cue_parse(const char* text, u32 length, CueSheet& out, u32* errorLine)
{
    memset(&out, 0, sizeof(out));
    u32 lineNo = 0;     // 1-based number of the line being parsed
    u32 nextLine = 1;
    u32 pos = 0;
    bool haveFile = false;
    bool motorola = false;
    CueTrack* track = nullptr;
    auto fail = [&](CueError e) { if (errorLine) *errorLine = lineNo; return e; };

    // skip a UTF-8 byte order mark
    if (length >= 3 && (u8)text[0] == 0xEF && (u8)text[1] == 0xBB && (u8)text[2] == 0xBF)
        pos = 3;

    while (pos < length)
    {
        u32 start = pos;
        lineNo = nextLine;
        while (pos < length && text[pos] != '\n' && text[pos] != '\r')
            pos++;
        u32 len = pos - start;
        while (pos < length && (text[pos] == '\n' || text[pos] == '\r'))
        {
            if (text[pos] == '\n')
                nextLine++;
            pos++;
        }
        if (len == 0)
            continue;
        char w[4][CUE_NAME_LEN];
        bool tooLong;
        u32 n = splitWords(text + start, len, w, tooLong);
        if (n == 0)
            continue;
        const char* cmd = w[0];

        if (eqNoCase(cmd, "FILE"))
        {
            if (n < 2)
                return fail(CueError::Syntax);
            if (tooLong)
                return fail(CueError::NameTooLong);
            const char* type = n >= 3 ? w[2] : "BINARY";
            if (eqNoCase(type, "BINARY"))
                motorola = false;
            else if (eqNoCase(type, "MOTOROLA"))
                motorola = true;
            else
                return fail(CueError::UnsupportedFile);
            if (out.fileCount >= CUE_MAX_FILES)
                return fail(CueError::TooManyFiles);
            strcpy(out.files[out.fileCount++], w[1]);
            haveFile = true;
            track = nullptr;
        }
        else if (eqNoCase(cmd, "TRACK"))
        {
            if (!haveFile)
                return fail(CueError::TrackBeforeFile);
            if (n < 3)
                return fail(CueError::Syntax);
            if (out.trackCount >= CD_MAX_TRACKS)
                return fail(CueError::TooManyTracks);
            s32 number = parseNumber(w[1]);
            if (number < 1 || number > 99)
                return fail(CueError::Syntax);
            const ModeInfo* mi = nullptr;
            for (const ModeInfo& m : sModes)
                if (eqNoCase(w[2], m.name))
                    mi = &m;
            if (!mi)
                return fail(CueError::UnsupportedMode);
            track = &out.tracks[out.trackCount++];
            track->number = (u8)number;
            track->mode = mi->mode;
            track->sectorSize = mi->sectorSize;
            track->userOffset = mi->userOffset;
            track->file = (u8)(out.fileCount - 1);
            track->motorola = motorola;
            track->index0 = -1;
            track->index1 = -1;
        }
        else if (eqNoCase(cmd, "INDEX"))
        {
            if (!track || n < 3)
                return fail(CueError::Syntax);
            s32 idx = parseNumber(w[1]);
            s32 frames = parseMsf(w[2]);
            if (idx < 0 || idx > 99 || frames < 0)
                return fail(CueError::Syntax);
            if (idx == 0)
                track->index0 = frames;
            else if (idx == 1)
                track->index1 = frames;
            // INDEX 02+ (sub-indexes) don't change the layout
        }
        else if (eqNoCase(cmd, "PREGAP") || eqNoCase(cmd, "POSTGAP"))
        {
            if (!track || n < 2)
                return fail(CueError::Syntax);
            s32 frames = parseMsf(w[1]);
            if (frames < 0)
                return fail(CueError::Syntax);
            if (eqNoCase(cmd, "PREGAP"))
                track->pregap = frames;
            else
                track->postgap = frames;
        }
        else if (eqNoCase(cmd, "CDTEXTFILE"))
        {
            if (n < 2 || tooLong)
                return fail(CueError::Syntax);
            strcpy(out.cdTextFile, w[1]);
        }
        else if (eqNoCase(cmd, "TITLE"))
        {
            if (!track && n >= 2)
            {
                strncpy(out.title, w[1], sizeof(out.title) - 1);
                out.title[sizeof(out.title) - 1] = 0;
            }
        }
        // REM, CATALOG, PERFORMER, SONGWRITER, ISRC, FLAGS, ... don't affect the data
    }

    if (out.trackCount == 0)
        return fail(CueError::NoTracks);
    for (u32 i = 0; i < out.trackCount; i++)
    {
        const CueTrack& t = out.tracks[i];
        if (t.index1 < 0)
        {
            if (errorLine)
                *errorLine = 0;
            return CueError::MissingIndex1;
        }
        if (t.index0 > t.index1)
            return CueError::BadIndexOrder;
    }
    return CueError::None;
}

CueError cue_buildLayout(const CueSheet& sheet, const u64* fileSizes, cd_layout_t& layout,
    u32* fileStartSector, u32& totalSectors)
{
    // Keep the pointers the caller set up (CD-Text, bounce buffer).
    const u8* cdText = layout.cdText;
    u32 cdTextLength = layout.cdTextLength;
    u8* bounce = layout.bounce;
    u32 bounceSize = layout.bounceSize;
    memset(&layout, 0, sizeof(layout));
    layout.cdText = cdText;
    layout.cdTextLength = cdTextLength;
    layout.bounce = bounce;
    layout.bounceSize = bounceSize;

    u64 sectors = 0;
    for (u32 f = 0; f < sheet.fileCount; f++)
    {
        fileStartSector[f] = (u32)sectors;
        sectors += (fileSizes[f] + 511) / 512;
    }
    if (sectors > 0xFFFFFFFFull)
        return CueError::TrackPastEndOfFile;
    totalSectors = (u32)sectors;

    u32 discLba = 0;
    u32 t = 0;
    while (t < sheet.trackCount)
    {
        u32 file = sheet.tracks[t].file;
        u32 fileFirstTrack = t;
        u32 fileEnd = t;
        while (fileEnd < sheet.trackCount && sheet.tracks[fileEnd].file == file)
            fileEnd++;

        u32 fileStartLba = discLba;
        u32 gapShift = 0;       // PREGAP/POSTGAP silence inserted inside this file
        u64 byteInFile = 0;
        s32 prevFirst = -1;
        u32 prevSize = 0;
        for (u32 i = fileFirstTrack; i < fileEnd; i++)
        {
            const CueTrack& ct = sheet.tracks[i];
            cd_track_t& lt = layout.tracks[i];
            s32 first = ct.index0 >= 0 ? ct.index0 : ct.index1;  // first frame stored in the file
            if (first < prevFirst)
                return CueError::BadIndexOrder;
            if (prevFirst >= 0)
                byteInFile += (u64)(first - prevFirst) * prevSize;
            gapShift += ct.pregap;

            lt.number = ct.number;
            lt.mode = ct.mode;
            lt.sectorSize = ct.sectorSize;
            lt.userOffset = ct.userOffset;
            lt.dataLba = fileStartLba + gapShift + (u32)first;
            lt.index1 = fileStartLba + gapShift + (u32)ct.index1;
            lt.index0 = ct.index0 >= 0 ? fileStartLba + gapShift + (u32)ct.index0 : lt.index1 - ct.pregap;
            if (byteInFile > fileSizes[file])
                return CueError::TrackPastEndOfFile;
            lt.byteOffset = fileStartSector[file] * 512 + (u32)byteInFile;

            if (i + 1 < fileEnd)
            {
                const CueTrack& next = sheet.tracks[i + 1];
                s32 nextFirst = next.index0 >= 0 ? next.index0 : next.index1;
                if (nextFirst < first)
                    return CueError::BadIndexOrder;
                lt.frames = (u32)(nextFirst - first);
                if (byteInFile + (u64)lt.frames * ct.sectorSize > fileSizes[file])
                    return CueError::TrackPastEndOfFile;
            }
            else
            {
                lt.frames = (u32)((fileSizes[file] - byteInFile) / ct.sectorSize); // a partial last frame is ignored
            }
            if (lt.frames == 0 || lt.index1 >= lt.dataLba + lt.frames)
                return CueError::EmptyTrack;
            prevFirst = first;
            prevSize = ct.sectorSize;
            if (ct.mode == CD_TRACK_MODE2)
                layout.discType = 0x20;
            gapShift += ct.postgap;
        }
        const cd_track_t& last = layout.tracks[fileEnd - 1];
        discLba = last.dataLba + last.frames + sheet.tracks[fileEnd - 1].postgap;
        t = fileEnd;
    }
    layout.trackCount = sheet.trackCount;
    layout.leadout = discLba;
    return CueError::None;
}

u32 cue_audioTracks(const cd_layout_t& layout)
{
    u32 n = 0;
    for (u32 i = 0; i < layout.trackCount; i++)
        if (layout.tracks[i].mode == CD_TRACK_AUDIO)
            n++;
    return n;
}

void cue_cdTextTitle(const u8* packs, u32 length, char* out, u32 outSize)
{
    u32 n = 0;
    out[0] = 0;
    for (u32 p = 0; p + 18 <= length && n + 1 < outSize; p += 18)
    {
        const u8* pk = packs + p;
        if (pk[0] != 0x80 || (pk[3] & 0x80))   // title packs, single-byte text only
            continue;
        u32 trackOfFirstChar = pk[1];
        u32 charPos = pk[3] & 0x0F;            // characters of the previous string
        for (u32 i = 0; i < 12 && n + 1 < outSize; i++)
        {
            // the title of track 0 (the album) is the first string of block 0
            if (trackOfFirstChar != 0 && n == 0)
                return;
            u8 c = pk[4 + i];
            if (c == 0)
            {
                if (n > 0 || charPos == 0)
                {
                    out[n] = 0;
                    return;
                }
                continue;
            }
            out[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
        }
    }
    out[n] = 0;
}

static u16 cdTextCrc(const u8* p)
{
    u16 c = 0;
    for (u32 i = 0; i < 16; i++)
    {
        c ^= (u16)p[i] << 8;
        for (u32 b = 0; b < 8; b++)
            c = (c & 0x8000) ? (u16)((c << 1) ^ 0x1021) : (u16)(c << 1);
    }
    return c ^ 0xFFFF;
}

u32 cue_normalizeCdText(u8* data, u32 length, u8* scratch)
{
    u32 packs = length / 18;
    if (packs == 0)
        return 0;
    length = packs * 18;
    // Find where the packs start: some tools write the stream rotated, with
    // the last pack wrapping around to the start of the file.
    u32 bestOffset = 0, bestValid = 0;
    for (u32 off = 0; off < 18; off++)
    {
        u32 valid = 0;
        for (u32 p = 0; p < packs; p++)
        {
            u32 at = (off + p * 18) % length;
            u8 pk[18];
            for (u32 i = 0; i < 18; i++)
                pk[i] = data[(at + i) % length];
            if (cdTextCrc(pk) == ((pk[16] << 8) | pk[17]))
                valid++;
        }
        if (valid > bestValid)
        {
            bestValid = valid;
            bestOffset = off;
        }
    }
    if (bestValid == 0)
        return 0;   // not CD-Text
    for (u32 i = 0; i < length; i++)
        scratch[i] = data[(bestOffset + i) % length];
    // keep packs with a CD-Text pack type, with correct CRCs
    u32 out = 0;
    for (u32 p = 0; p < packs; p++)
    {
        u8* pk = scratch + p * 18;
        if (pk[0] < 0x80 || pk[0] > 0x8F)
            continue;
        u16 crc = cdTextCrc(pk);
        pk[16] = crc >> 8;
        pk[17] = crc;
        memmove(data + out, pk, 18);
        out += 18;
    }
    return out;
}
