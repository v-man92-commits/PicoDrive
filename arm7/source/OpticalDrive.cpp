#include "common.h"
#include <string.h>
#include "OpticalDrive.h"

// References: SCSI Multimedia Commands (MMC-5/MMC-6), SPC-3.

namespace
{
    constexpr u32 CD_MAX_BLOCKS = 360000;      // 80-minute CD (~703 MB)
    constexpr u16 PROFILE_CD_ROM = 0x0008;
    constexpr u16 PROFILE_DVD_ROM = 0x0010;
    constexpr u32 DVD_START_PSN = 0x030000;    // first physical sector of DVD data zone

    inline void be16(u8* p, u32 v) { p[0] = v >> 8; p[1] = v; }
    inline void be24(u8* p, u32 v) { p[0] = v >> 16; p[1] = v >> 8; p[2] = v; }
    inline void be32(u8* p, u32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
    inline u32 rdBe16(const u8* p) { return (p[0] << 8) | p[1]; }
    inline u32 rdBe32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

    // LBA -> MSF address (with the 2-second / 150-frame lead-in offset)
    void putMsf(u8* p, u32 lba)
    {
        u32 f = lba + 150;
        if (f >= 256 * 60 * 75)
            f = 256 * 60 * 75 - 1; // beyond 255:59:74 (DVD sizes): clamp
        p[0] = 0;
        p[1] = f / (75 * 60);
        p[2] = (f / 75) % 60;
        p[3] = f % 75;
    }

    void putAddress(u8* p, u32 lba, bool msf)
    {
        if (msf)
            putMsf(p, lba);
        else
            be32(p, lba);
    }

    int32_t clampLen(int32_t len, u32 alloc, u32 bufsize)
    {
        if ((u32)len > alloc)
            len = alloc;
        if ((u32)len > bufsize)
            len = bufsize;
        return len;
    }

    // ---- tracks: a BIN/CUE layout, or one data track for an ISO --------------
    u32 trackCount(const OpticalState& st)
    {
        return st.layout ? st.layout->trackCount : 1;
    }

    cd_track_t trackAt(const OpticalState& st, u32 i)
    {
        if (st.layout)
            return st.layout->tracks[i];
        cd_track_t t;
        memset(&t, 0, sizeof(t));
        t.number = 1;
        t.mode = CD_TRACK_MODE1;
        t.sectorSize = 2048;
        t.frames = st.blocks;
        return t;
    }

    u8 controlOf(const cd_track_t& t)
    {
        return t.mode == CD_TRACK_AUDIO ? 0x00 : 0x04; // audio: 2 channels; data track
    }

    /// The track an LBA belongs to (from its pregap on).
    u32 ownerTrack(const OpticalState& st, u32 lba)
    {
        u32 n = trackCount(st), owner = 0;
        for (u32 i = 1; i < n; i++)
            if (st.layout->tracks[i].index0 <= lba)
                owner = i;
        return owner;
    }

    u8 sResp[1280]; // TOC responses (full TOC of 99 tracks: 4 + 11 * 102 bytes)

    // ---- READ TOC/PMA/ATIP (0x43) -------------------------------------------
    int32_t readToc(const OpticalState& st, const u8* cdb, u8* out, u32 bufsize,
        void (*setSense)(u8, u8, u8))
    {
        bool msf = cdb[1] & 0x02;
        u32 format = cdb[2] & 0x0F;
        if (format == 0 && (cdb[9] & 0xC0))
            format = cdb[9] >> 6; // legacy (SFF-8020) format location
        u32 startTrack = cdb[6];
        u32 alloc = rdBe16(cdb + 7);
        u32 n = trackCount(st);
        cd_track_t first = trackAt(st, 0), last = trackAt(st, n - 1);
        u8* resp = sResp;
        memset(resp, 0, sizeof(sResp));
        int32_t len;

        switch (format)
        {
            case 0: // formatted TOC: tracks from startTrack on, then the lead-out
            {
                if (startTrack > last.number && startTrack != 0xAA)
                {
                    setSense(0x05, 0x24, 0x00); // invalid field in CDB
                    return -1;
                }
                u8* d = resp + 4;
                for (u32 i = 0; i < n && startTrack != 0xAA; i++)
                {
                    cd_track_t t = trackAt(st, i);
                    if (t.number < startTrack)
                        continue;
                    d[1] = 0x10 | controlOf(t);  // ADR 1
                    d[2] = t.number;
                    putAddress(d + 4, t.index1, msf);
                    d += 8;
                }
                d[1] = 0x10 | controlOf(last);
                d[2] = 0xAA; // lead-out
                putAddress(d + 4, st.blocks, msf);
                d += 8;
                len = d - resp;
                resp[2] = first.number;
                resp[3] = last.number;
                break;
            }
            case 1: // session information: one session
            {
                resp[2] = 1;
                resp[3] = 1;
                resp[5] = 0x10 | controlOf(first);
                resp[6] = first.number;
                putAddress(resp + 8, first.index1, msf);
                len = 12;
                break;
            }
            case 2: // full TOC (raw), always MSF
            {
                resp[2] = 1;
                resp[3] = 1;
                u8* d = resp + 4;
                auto entry = [&](u8 control, u8 point, u8 pmin, u8 psec, u8 pframe)
                {
                    d[0] = 1;              // session
                    d[1] = 0x10 | control; // ADR 1
                    d[2] = 0;              // TNO
                    d[3] = point;
                    d[8] = pmin;
                    d[9] = psec;
                    d[10] = pframe;
                    d += 11;
                };
                u8 m[4];
                entry(controlOf(first), 0xA0, first.number, st.layout ? st.layout->discType : 0x00, 0);
                entry(controlOf(last), 0xA1, last.number, 0, 0);
                putMsf(m, st.blocks);
                entry(controlOf(last), 0xA2, m[1], m[2], m[3]);  // lead-out start
                for (u32 i = 0; i < n; i++)
                {
                    cd_track_t t = trackAt(st, i);
                    putMsf(m, t.index1);
                    entry(controlOf(t), t.number, m[1], m[2], m[3]);
                }
                len = d - resp;
                break;
            }
            case 5: // CD-Text
            {
                const cd_layout_t* l = st.layout;
                if (!l || !l->cdText || l->cdTextLength == 0)
                {
                    setSense(0x05, 0x24, 0x00); // no CD-Text on this disc
                    return -1;
                }
                // packs are copied straight into the response buffer
                u32 packs = l->cdTextLength;
                if (bufsize < 4)
                    return 0;
                if (packs > bufsize - 4)
                    packs = (bufsize - 4) / 18 * 18;
                memset(out, 0, 4);
                be16(out, l->cdTextLength + 2);
                memcpy(out + 4, l->cdText, packs);
                len = 4 + packs;
                return clampLen(len, alloc, bufsize);
            }
            default:
                setSense(0x05, 0x24, 0x00);
                return -1;
        }
        be16(resp, len - 2);
        len = clampLen(len, alloc, bufsize);
        memcpy(out, resp, len);
        return len;
    }

    // ---- GET CONFIGURATION (0x46) -------------------------------------------
    int32_t getConfiguration(const OpticalState& st, const u8* cdb, u8* out, u32 bufsize)
    {
        u32 rt = cdb[1] & 0x03;
        u32 start = rdBe16(cdb + 2);
        u32 alloc = rdBe16(cdb + 7);
        bool dvd = optical_isDvd(st);
        u16 current = st.present ? (dvd ? PROFILE_DVD_ROM : PROFILE_CD_ROM) : 0;

        u8 resp[128];
        memset(resp, 0, sizeof(resp));
        u8* p = resp + 8;

        auto feature = [&](u16 code, u8 version, bool persistent, bool isCurrent, u8 addLen) -> u8*
        {
            bool include;
            if (rt == 2)
                include = code == start;
            else if (rt == 1)
                include = code >= start && isCurrent;
            else
                include = code >= start;
            if (!include)
                return nullptr;
            be16(p, code);
            p[2] = (version << 2) | (persistent ? 2 : 0) | (isCurrent ? 1 : 0);
            p[3] = addLen;
            u8* data = p + 4;
            p += 4 + addLen;
            return data;
        };

        // 0x0000 Profile List
        if (u8* d = feature(0x0000, 0, true, true, 8))
        {
            be16(d + 0, PROFILE_DVD_ROM);
            d[2] = current == PROFILE_DVD_ROM ? 1 : 0;
            be16(d + 4, PROFILE_CD_ROM);
            d[6] = current == PROFILE_CD_ROM ? 1 : 0;
        }
        // 0x0001 Core: USB physical interface, DBE
        if (u8* d = feature(0x0001, 1, true, true, 8))
        {
            be32(d, 0x00000008);
            d[4] = 0x01;
        }
        // 0x0002 Morphing: supports operational change events
        if (u8* d = feature(0x0002, 1, true, true, 4))
            d[0] = 0x02;
        // 0x0003 Removable Medium: tray, eject, lock
        if (u8* d = feature(0x0003, 0, true, true, 4))
            d[0] = 0x29;
        // 0x0010 Random Readable
        if (u8* d = feature(0x0010, 0, false, st.present, 8))
        {
            be32(d, 2048);
            be16(d + 4, dvd ? 16 : 1);
        }
        // 0x001E CD Read
        if (u8* d = feature(0x001E, 2, false, st.present && !dvd, 4))
            d[0] = (st.layout && st.layout->cdText) ? 0x01 : 0x00; // CD-Text readable
        // 0x001F DVD Read
        if (u8* d = feature(0x001F, 0, false, st.present && dvd, 0))
            (void)d;

        int32_t len = p - resp;
        be32(resp, len - 4);
        be16(resp + 6, current);
        len = clampLen(len, alloc, bufsize);
        memcpy(out, resp, len);
        return len;
    }

    // ---- GET EVENT STATUS NOTIFICATION (0x4A) -------------------------------
    int32_t getEventStatus(OpticalState& st, const u8* cdb, u8* out, u32 bufsize,
        void (*setSense)(u8, u8, u8))
    {
        if (!(cdb[1] & 0x01))
        {
            setSense(0x05, 0x24, 0x00); // only polled mode is supported
            return -1;
        }
        u32 classes = cdb[4];
        u32 alloc = rdBe16(cdb + 7);
        u8 resp[8];
        memset(resp, 0, sizeof(resp));
        int32_t len;
        resp[3] = 0x10; // supported: media class
        if (classes & 0x10)
        {
            resp[2] = 0x04; // media class
            u8* d = resp + 4;
            d[0] = st.pendingEvent;       // 0 no change, 2 new media, 3 removal
            d[1] = st.present ? 0x02 : 0x00; // media present (tray closed)
            st.pendingEvent = 0;
            be16(resp, 6);
            len = 8;
        }
        else
        {
            resp[2] = 0x80; // no event available
            be16(resp, 2);
            len = 4;
        }
        len = clampLen(len, alloc, bufsize);
        memcpy(out, resp, len);
        return len;
    }

    // ---- READ DISC INFORMATION (0x51) ---------------------------------------
    int32_t readDiscInfo(const OpticalState& st, const u8* cdb, u8* out, u32 bufsize)
    {
        u32 alloc = rdBe16(cdb + 7);
        u8 resp[34];
        memset(resp, 0, sizeof(resp));
        be16(resp, 32);
        resp[2] = 0x0E;  // last session complete, disc complete (not erasable)
        u32 n = trackCount(st);
        resp[3] = trackAt(st, 0).number;      // first track
        resp[4] = 1;                          // sessions
        resp[5] = trackAt(st, 0).number;      // first track in last session
        resp[6] = trackAt(st, n - 1).number;  // last track in last session
        resp[7] = 0x20;  // unrestricted use
        resp[8] = st.layout ? st.layout->discType : 0x00; // CD-DA / CD-ROM, or CD-ROM XA
        memset(resp + 16, 0xFF, 8); // lead-in / last possible lead-out: n/a
        int32_t len = clampLen(sizeof(resp), alloc, bufsize);
        memcpy(out, resp, len);
        return len;
    }

    // ---- READ TRACK INFORMATION (0x52) --------------------------------------
    int32_t readTrackInfo(const OpticalState& st, const u8* cdb, u8* out, u32 bufsize,
        void (*setSense)(u8, u8, u8))
    {
        u32 type = cdb[1] & 0x03;
        u32 addr = rdBe32(cdb + 2);
        u32 alloc = rdBe16(cdb + 7);
        u32 n = trackCount(st);
        s32 idx = -1;
        if (type == 0 && addr < st.blocks)
            idx = st.layout ? (s32)ownerTrack(st, addr) : 0;
        else if (type == 1)
        {
            for (u32 i = 0; i < n; i++)
                if (trackAt(st, i).number == addr)
                    idx = i;
        }
        else if (type == 2 && addr == 1)
            idx = 0;    // session 1 starts with the first track
        if (idx < 0)
        {
            setSense(0x05, 0x24, 0x00);
            return -1;
        }
        cd_track_t t = trackAt(st, idx);
        u32 end = (u32)idx + 1 < n ? trackAt(st, idx + 1).index1 : st.blocks;
        u8 resp[36];
        memset(resp, 0, sizeof(resp));
        be16(resp, 34);
        resp[2] = t.number;   // track number
        resp[3] = 1;          // session
        resp[5] = controlOf(t);                                  // track mode (control nibble)
        resp[6] = t.mode == CD_TRACK_AUDIO ? 0x0F : t.mode;      // data mode 1/2, 0Fh for audio
        be32(resp + 8, t.index1);           // track start
        be32(resp + 24, end - t.index1);    // track size
        int32_t len = clampLen(sizeof(resp), alloc, bufsize);
        memcpy(out, resp, len);
        return len;
    }

    // ---- MODE SENSE (10) for the optical drive ------------------------------
    int32_t modeSense10(const OpticalState& st, const u8* cdb, u8* out, u32 bufsize,
        void (*setSense)(u8, u8, u8))
    {
        u32 page = cdb[2] & 0x3F;
        u32 alloc = rdBe16(cdb + 7);
        u8 resp[64];
        memset(resp, 0, sizeof(resp));
        u8* p = resp + 8; // header, no block descriptors

        bool any = false;
        if (page == 0x01 || page == 0x3F)
        {
            // Read/Write Error Recovery
            p[0] = 0x01;
            p[1] = 0x0A;
            p += 12;
            any = true;
        }
        if (page == 0x2A || page == 0x3F)
        {
            // MM Capabilities and Mechanical Status
            bool dvd = optical_isDvd(st);
            p[0] = 0x2A;
            p[1] = 0x14;
            p[2] = dvd ? 0x08 : 0x00;   // DVD-ROM read
            p[3] = 0x00;                // no writing
            p[4] = 0x00;                // no analog audio play
            p[5] = dvd ? 0x00 : 0x03;   // CD-DA commands supported, CD-DA stream accurate
            p[6] = 0x29;                // tray, eject, lock supported
            be16(p + 8, 1385);          // max read speed (kB/s, obsolete)
            be16(p + 14, 1385);         // current read speed
            p += 22;
            any = true;
        }
        if (!any)
        {
            setSense(0x05, 0x24, 0x00); // page not supported
            return -1;
        }
        int32_t len = p - resp;
        be16(resp, len - 2);
        len = clampLen(len, alloc, bufsize);
        memcpy(out, resp, len);
        return len;
    }

    // ---- READ DVD STRUCTURE (0xAD) ------------------------------------------
    int32_t readDvdStructure(const OpticalState& st, const u8* cdb, u8* out, u32 bufsize,
        void (*setSense)(u8, u8, u8))
    {
        u32 format = cdb[7];
        u32 alloc = rdBe16(cdb + 8);
        if (!optical_isDvd(st) && format != 0xFF)
        {
            setSense(0x05, 0x30, 0x02); // cannot read medium: incompatible format
            return -1;
        }
        int32_t len;
        switch (format)
        {
            case 0x00: // physical format information
            {
                len = 4 + 2048;
                if ((u32)len > bufsize)
                    len = bufsize;
                memset(out, 0, len);
                be16(out, 2048 + 2);
                u8* d = out + 4;
                d[0] = 0x01;                       // DVD-ROM, part version 1
                d[1] = 0x02;                       // 120 mm, 10.08 Mbps
                d[2] = 0x01;                       // 1 layer, PTP, embossed
                d[3] = 0x00;
                be24(d + 5, DVD_START_PSN);
                be24(d + 9, DVD_START_PSN + st.blocks - 1);
                break;
            }
            case 0x01: // copyright information: none
            {
                u8 resp[8] = { 0, 6, 0, 0, 0x00, 0x00, 0, 0 };
                len = sizeof(resp);
                memcpy(out, resp, len < (int32_t)bufsize ? len : bufsize);
                break;
            }
            case 0xFF: // list of supported structures
            {
                u8 resp[12] = { 0, 10, 0, 0,
                                0x00, 0x40, 0x08, 0x04,  // format 0, readable, 2052 bytes
                                0x01, 0x40, 0x00, 0x08 }; // format 1, readable, 8 bytes
                len = sizeof(resp);
                memcpy(out, resp, len < (int32_t)bufsize ? len : bufsize);
                break;
            }
            default:
                setSense(0x05, 0x24, 0x00);
                return -1;
        }
        return clampLen(len, alloc, bufsize);
    }

    // ---- data reads not handled by TinyUSB (READ 12, READ CD) ---------------
    int32_t readBlocks(const OpticalState& st, u32 lba, u32 count, u8* out, u32 bufsize,
        OpticalReadFn read, void (*setSense)(u8, u8, u8))
    {
        if (count == 0)
            return 0;
        if (lba >= st.blocks || count > st.blocks - lba)
        {
            setSense(0x05, 0x21, 0x00); // LBA out of range
            return -1;
        }
        if (count * 2048 > bufsize)
        {
            setSense(0x05, 0x24, 0x00); // larger than one transfer buffer
            return -1;
        }
        if (!read(lba, count, out))
        {
            setSense(0x03, 0x11, 0x00); // unrecovered read error
            return -1;
        }
        return count * 2048;
    }
}

void optical_insert(OpticalState& state, u32 blocks, const cd_layout_t* layout)
{
    state.blocks = layout ? layout->leadout : blocks;
    state.layout = layout;
    state.present = true;
    state.pendingEvent = 2; // new media
}

void optical_remove(OpticalState& state)
{
    state.present = false;
    state.pendingEvent = 3; // media removal
}

// ---- reads on BIN/CUE discs ---------------------------------------------------

namespace
{
    enum class ReadKind { Cooked, ReadCd };

    struct Piece { u16 start, end; u8 flag; };

    /// Bytes of one block for track t: false with sense if t can't give them.
    bool blockShape(const cd_track_t& t, ReadKind kind, u32 expectedType, u32 flags,
        u16& from, u32& size, u8& key, u8& asc)
    {
        key = 0x05;
        asc = 0x24; // invalid field in CDB
        if (kind == ReadKind::Cooked)
        {
            if (t.mode == CD_TRACK_AUDIO)
            {
                asc = 0x64; // illegal mode for this track
                return false;
            }
            from = t.userOffset;
            size = 2048;
            return true;
        }

        // READ CD: expected sector type
        bool typeOk;
        switch (expectedType)
        {
            case 0: typeOk = true; break;                                   // any
            case 1: typeOk = t.mode == CD_TRACK_AUDIO; break;               // CD-DA
            case 2: typeOk = t.mode == CD_TRACK_MODE1; break;               // mode 1
            case 3: case 4: typeOk = t.mode == CD_TRACK_MODE2; break;       // mode 2 formless / form 1
            default: typeOk = false; break;                                 // form 2: not kept apart
        }
        if (!typeOk)
        {
            asc = expectedType > 4 ? 0x24 : 0x64;
            return false;
        }
        if (flags & 0x06)
            return false; // C2 error information: not available

        if (t.mode == CD_TRACK_AUDIO)
        {
            if (!(flags & 0x10))
                return false;
            from = 0;     // CD-DA: the "user data" is the whole 2352-byte frame
            size = 2352;
            return true;
        }

        // data track: pick the requested fields out of the stored frame
        Piece pieces[5];
        u32 n = 0;
        bool formless = expectedType == 3;
        if (t.sectorSize == 2352)
        {
            pieces[n++] = { 0, 12, 0x80 };                      // sync
            pieces[n++] = { 12, 16, 0x20 };                     // header
            if (t.mode == CD_TRACK_MODE2)
            {
                if (formless)
                    pieces[n++] = { 16, 2352, 0x10 };           // mode 2 formless user data
                else
                {
                    pieces[n++] = { 16, 24, 0x40 };             // subheader
                    pieces[n++] = { 24, 2072, 0x10 };           // form 1 user data
                    pieces[n++] = { 2072, 2352, 0x08 };         // EDC/ECC
                }
            }
            else
            {
                pieces[n++] = { 16, 2064, 0x10 };               // mode 1 user data
                pieces[n++] = { 2064, 2352, 0x08 };             // EDC/ECC
            }
        }
        else if (t.sectorSize == 2336)
        {
            if (flags & 0xA0)
                return false; // sync/header aren't in a 2336-byte image
            if (formless)
                pieces[n++] = { 0, 2336, 0x10 };
            else
            {
                pieces[n++] = { 0, 8, 0x40 };
                pieces[n++] = { 8, 2056, 0x10 };
                pieces[n++] = { 2056, 2336, 0x08 };
            }
        }
        else // 2048: only the user data is stored
        {
            if ((flags & 0xF8) != 0x10)
                return false;
            pieces[n++] = { 0, 2048, 0x10 };
        }

        // "all headers" (header codes 11b) means header + subheader
        u32 want = flags & 0xF8;
        if ((flags & 0x60) == 0x60)
            want |= 0x60;
        s32 start = -1, end = -1;
        bool gap = false;
        for (u32 i = 0; i < n; i++)
        {
            bool sel = (want & pieces[i].flag) != 0;
            if (!sel)
            {
                if (start >= 0)
                    gap = true; // something was selected before this
                continue;
            }
            if (gap)
                return false; // the fields asked for aren't contiguous
            if (start < 0)
                start = pieces[i].start;
            end = pieces[i].end;
        }
        if (start < 0)
            return false;
        from = (u16)start;
        size = (u32)(end - start);
        return true;
    }

    u32 msfToLba(const u8* p)
    {
        return (p[0] * 60 + p[1]) * 75 + p[2] - 150;
    }
}

bool optical_planRead(const OpticalState& st, const u8 cdb[16], OpticalReadPlan& plan,
    void (*setSense)(u8, u8, u8))
{
    ReadKind kind;
    u32 lba, count, expectedType = 0, flags = 0x10, sub = 0;
    switch (cdb[0])
    {
        case 0x28: // READ (10)
            kind = ReadKind::Cooked;
            lba = rdBe32(cdb + 2);
            count = rdBe16(cdb + 7);
            break;
        case 0xA8: // READ (12)
            kind = ReadKind::Cooked;
            lba = rdBe32(cdb + 2);
            count = rdBe32(cdb + 6);
            break;
        case 0xBE: // READ CD
            kind = ReadKind::ReadCd;
            lba = rdBe32(cdb + 2);
            count = (cdb[6] << 16) | (cdb[7] << 8) | cdb[8];
            expectedType = (cdb[1] >> 2) & 0x07;
            flags = cdb[9];
            sub = cdb[10] & 0x07;
            break;
        case 0xB9: // READ CD MSF
        {
            kind = ReadKind::ReadCd;
            if (cdb[3] * 60 * 75 + cdb[4] * 75 + cdb[5] < 150 || cdb[4] >= 60 || cdb[5] >= 75 ||
                cdb[7] >= 60 || cdb[8] >= 75)
            {
                setSense(0x05, 0x24, 0x00);
                return false;
            }
            lba = msfToLba(cdb + 3);
            u32 end = msfToLba(cdb + 6);
            if (end < lba)
            {
                setSense(0x05, 0x24, 0x00);
                return false;
            }
            count = end - lba;
            expectedType = (cdb[1] >> 2) & 0x07;
            flags = cdb[9];
            sub = cdb[10] & 0x07;
            break;
        }
        default:
            setSense(0x05, 0x20, 0x00);
            return false;
    }
    if (sub != 0)
    {
        setSense(0x05, 0x24, 0x00); // sub-channel data: not available
        return false;
    }
    if (lba > st.blocks || count > st.blocks - lba)
    {
        setSense(0x05, 0x21, 0x00); // LBA out of range
        return false;
    }
    plan.lba = lba;
    plan.count = count;
    plan.cmdLbaField = rdBe32(cdb + 2);
    plan.from = 0;
    plan.blockSize = 2048;
    if (count == 0)
        return true;

    // every track the range touches must give blocks of the same shape
    u32 n = trackCount(st);
    u32 firstTrack = st.layout ? ownerTrack(st, lba) : 0;
    bool have = false;
    for (u32 i = firstTrack; i < n; i++)
    {
        cd_track_t t = trackAt(st, i);
        if (i > firstTrack && t.index0 >= lba + count)
            break;
        u16 from;
        u32 size;
        u8 key, asc;
        if (!blockShape(t, kind, expectedType, flags, from, size, key, asc))
        {
            setSense(key, asc, 0x00);
            return false;
        }
        if (have && (from != plan.from || size != plan.blockSize))
        {
            setSense(0x05, 0x64, 0x00); // crosses into a track of another kind
            return false;
        }
        plan.from = from;
        plan.blockSize = size;
        have = true;
    }
    return true;
}

bool optical_produce(const OpticalState& st, const OpticalReadPlan& plan, u32 blockIndex,
    u32 offsetInBlock, u8* out, u32 length, OpticalSpanFn span, u32 maxSpan)
{
    const u32 bs = plan.blockSize;
    u32 pos = 0;
    u32 blk = blockIndex + offsetInBlock / bs;
    u32 inBlock = offsetInBlock % bs;
    u32 n = trackCount(st);
    while (pos < length)
    {
        u32 lba = plan.lba + blk;
        // the track storing this frame, if any
        const cd_track_t* t = nullptr;
        cd_track_t iso;
        if (!st.layout)
        {
            iso = trackAt(st, 0);
            t = &iso;
        }
        else
        {
            for (u32 i = 0; i < n; i++)
            {
                const cd_track_t& c = st.layout->tracks[i];
                if (lba >= c.dataLba && lba - c.dataLba < c.frames)
                {
                    t = &c;
                    break;
                }
            }
        }
        if (!t)
        {
            // a gap that isn't stored in the image (PREGAP/POSTGAP): silence
            u32 chunk = bs - inBlock;
            if (chunk > length - pos)
                chunk = length - pos;
            memset(out + pos, 0, chunk);
            pos += chunk;
            inBlock += chunk;
            if (inBlock == bs)
            {
                inBlock = 0;
                blk++;
            }
            continue;
        }
        // read the frames this request still needs from this track in one go
        u32 lastBlk = blk + (inBlock + (length - pos) - 1) / bs;
        u32 lastLba = plan.lba + lastBlk;
        u32 trackLast = t->dataLba + t->frames - 1;
        if (lastLba > trackLast)
            lastLba = trackLast;
        u32 frames = lastLba - lba + 1;
        if (frames * t->sectorSize > maxSpan)
            frames = maxSpan / t->sectorSize;
        if (frames == 0)
            return false;
        const u8* data = span(t->byteOffset + (lba - t->dataLba) * t->sectorSize, frames * t->sectorSize);
        if (!data)
            return false;
        for (u32 f = 0; f < frames && pos < length; f++)
        {
            const u8* frame = data + f * t->sectorSize;
            u32 chunk = bs - inBlock;
            if (chunk > length - pos)
                chunk = length - pos;
            memcpy(out + pos, frame + plan.from + inBlock, chunk);
            pos += chunk;
            inBlock += chunk;
            if (inBlock == bs)
            {
                inBlock = 0;
                blk++;
            }
        }
    }
    return true;
}

u32 optical_streamBlocks(const OpticalState& st, const u8 cdb[16], u32 totalBytes)
{
    if (st.layout)
    {
        if (!st.present || (cdb[0] != 0xA8 && cdb[0] != 0xBE && cdb[0] != 0xB9))
            return 0;
        OpticalReadPlan plan;
        if (!optical_planRead(st, cdb, plan, [](u8, u8, u8) {}))
            return 0;
        if (plan.count == 0 || (u64)plan.count * plan.blockSize != totalBytes)
            return 0;
        return plan.count;
    }

    if (!st.present)
        return 0;
    u32 lba = rdBe32(cdb + 2), count;
    if (cdb[0] == 0xA8)
        count = rdBe32(cdb + 6);
    else if (cdb[0] == 0xBE)
    {
        u32 sectorType = (cdb[1] >> 2) & 0x07;
        if ((sectorType != 0 && sectorType != 2) || cdb[9] != 0x10 || (cdb[10] & 0x07))
            return 0;
        count = (cdb[6] << 16) | (cdb[7] << 8) | cdb[8];
    }
    else
        return 0;
    if (count == 0 || lba >= st.blocks || count > st.blocks - lba)
        return 0;
    if (count > 0xFFFFFFFFu / 2048 || totalBytes != count * 2048)
        return 0;
    return count;
}

bool optical_isDvd(const OpticalState& state)
{
    return state.blocks > CD_MAX_BLOCKS;
}

static OpticalSpanFn sSpanFn;
static u32 sSpanMax;

void optical_setSpanReader(OpticalSpanFn span, u32 maxSpan)
{
    sSpanFn = span;
    sSpanMax = maxSpan;
}

/// A BIN/CUE read that wasn't streamed (it fits one buffer, or the request
/// didn't match its transfer length).
static int32_t layoutRead(const OpticalState& st, const u8 cdb[16], u8* buffer, u32 bufsize,
    void (*setSense)(u8, u8, u8))
{
    OpticalReadPlan plan;
    if (!optical_planRead(st, cdb, plan, setSense))
        return -1;
    u32 bytes = plan.count * plan.blockSize;
    if (bytes > bufsize || !sSpanFn)
    {
        setSense(0x05, 0x24, 0x00); // transfer length doesn't match the request
        return -1;
    }
    if (bytes && !optical_produce(st, plan, 0, 0, buffer, bytes, sSpanFn, sSpanMax))
    {
        setSense(0x03, 0x11, 0x00); // unrecovered read error
        return -1;
    }
    return bytes;
}

int32_t optical_scsi(OpticalState& st, const u8 cdb[16], u8* buffer, u32 bufsize,
    OpticalReadFn read, void (*setSense)(u8 key, u8 asc, u8 ascq), bool& handled)
{
    handled = true;
    u8 op = cdb[0];

    // Commands that need a disc
    bool needsMedium = op == 0x43 || op == 0x51 || op == 0x52 || op == 0xA8 || op == 0xBE || op == 0xB9 || op == 0xAD;
    if (needsMedium && !st.present)
    {
        setSense(0x02, 0x3A, 0x00); // medium not present
        return -1;
    }

    switch (op)
    {
        case 0x43: return readToc(st, cdb, buffer, bufsize, setSense);
        case 0x46: return getConfiguration(st, cdb, buffer, bufsize);
        case 0x4A: return getEventStatus(st, cdb, buffer, bufsize, setSense);
        case 0x51: return readDiscInfo(st, cdb, buffer, bufsize);
        case 0x52: return readTrackInfo(st, cdb, buffer, bufsize, setSense);
        case 0x5A: return modeSense10(st, cdb, buffer, bufsize, setSense);
        case 0xAD: return readDvdStructure(st, cdb, buffer, bufsize, setSense);

        case 0xBD: // MECHANISM STATUS: no changer, idle
        {
            u32 alloc = rdBe16(cdb + 8);
            u8 resp[8];
            memset(resp, 0, sizeof(resp));
            resp[1] = st.present ? 0x00 : 0x10; // door open when ejected
            int32_t len = clampLen(sizeof(resp), alloc, bufsize);
            memcpy(buffer, resp, len);
            return len;
        }
        case 0x42: // READ SUB-CHANNEL: no audio
        {
            u32 alloc = rdBe16(cdb + 7);
            u8 resp[4] = { 0, 0x15, 0, 0 }; // no current audio status
            int32_t len = clampLen(sizeof(resp), alloc, bufsize);
            memcpy(buffer, resp, len);
            return len;
        }
        case 0xAC: // GET PERFORMANCE: no descriptors
        {
            u32 alloc = rdBe16(cdb + 8) * 16 + 8;
            u8 resp[8] = { 0, 0, 0, 4, 0, 0, 0, 0 };
            int32_t len = clampLen(sizeof(resp), alloc, bufsize);
            memcpy(buffer, resp, len);
            return len;
        }
        case 0xBB: // SET CD SPEED
        case 0xB6: // SET STREAMING (parameter data already received)
            return op == 0xB6 ? (int32_t)bufsize : 0;

        case 0xA8: // READ (12)
            if (st.layout)
                return layoutRead(st, cdb, buffer, bufsize, setSense);
            return readBlocks(st, rdBe32(cdb + 2), rdBe32(cdb + 6), buffer, bufsize, read, setSense);

        case 0xB9: // READ CD MSF (BIN/CUE discs; normally streamed)
            if (st.layout)
                return layoutRead(st, cdb, buffer, bufsize, setSense);
            break;

        case 0xBE: // READ CD: user data (2048 bytes/sector) only
        {
            if (st.layout)
                return layoutRead(st, cdb, buffer, bufsize, setSense);
            u32 sectorType = (cdb[1] >> 2) & 0x07;   // 0 any, 2 mode 1
            u32 flags = cdb[9];
            if ((sectorType != 0 && sectorType != 2) || flags != 0x10 || (cdb[10] & 0x07))
            {
                setSense(0x05, 0x24, 0x00); // only cooked user data is supported
                return -1;
            }
            u32 count = (cdb[6] << 16) | (cdb[7] << 8) | cdb[8];
            return readBlocks(st, rdBe32(cdb + 2), count, buffer, bufsize, read, setSense);
        }
    }

    handled = false;
    return -1;
}
