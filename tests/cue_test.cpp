// Host test for arm9/source/CueSheet.cpp: cue parsing and the disc layout.
#include <stdio.h>
#include <string.h>
#include <vector>
#include <string>
#include "common.h"
#include "CueSheet.h"

static int gPasses, gFails;
#define CHECK(c, ...) do { if (c) gPasses++; else { gFails++; printf("  FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static std::vector<u8> readFile(const char* path)
{
    std::vector<u8> v;
    FILE* f = fopen(path, "rb");
    if (!f) return v;
    int c;
    while ((c = fgetc(f)) != EOF) v.push_back((u8)c);
    fclose(f);
    return v;
}

static CueSheet gSheet;
static cd_layout_t gLayout;

static CueError build(const char* text, std::vector<u64> sizes, u32* total = nullptr)
{
    CueError e = cue_parse(text, strlen(text), gSheet);
    if (e != CueError::None) return e;
    u32 starts[CUE_MAX_FILES], tot;
    memset(&gLayout, 0, sizeof(gLayout));
    e = cue_buildLayout(gSheet, sizes.data(), gLayout, starts, tot);
    if (total) *total = tot;
    return e;
}

static u32 msf(u32 m, u32 s, u32 f) { return (m * 60 + s) * 75 + f; }

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : "data";
    // ---- the real Disney Princess audio CD cue (CRLF, CDTEXTFILE, ISRC, INDEX 00) ----
    {
        std::string p = std::string(dir) + "/disney.cue";
        auto text = readFile(p.c_str());
        CHECK(!text.empty(), "read %s", p.c_str());
        u32 line = 0;
        CueError e = cue_parse((const char*)text.data(), text.size(), gSheet, &line);
        CHECK(e == CueError::None, "parse disney.cue: %s (line %u)", cue_errorString(e), line);
        CHECK(gSheet.fileCount == 1 && !strcmp(gSheet.files[0], "Disney_icon_princess.bin"), "FILE name");
        CHECK(!strcmp(gSheet.cdTextFile, "Disney_icon_princess.cdt"), "CDTEXTFILE");
        CHECK(gSheet.trackCount == 10, "10 tracks (%u)", gSheet.trackCount);
        u64 size = 282630432; // 120166 frames
        u32 starts[CUE_MAX_FILES], total;
        memset(&gLayout, 0, sizeof(gLayout));
        e = cue_buildLayout(gSheet, &size, gLayout, starts, total);
        CHECK(e == CueError::None, "layout: %s", cue_errorString(e));
        CHECK(gLayout.leadout == 120166 && total == (282630432 + 511) / 512, "leadout %u", gLayout.leadout);
        CHECK(gLayout.tracks[0].index1 == 0 && gLayout.tracks[0].mode == CD_TRACK_AUDIO, "track 1 at 0");
        CHECK(gLayout.tracks[1].index0 == msf(4, 34, 36) && gLayout.tracks[1].index1 == msf(4, 35, 51), "track 2 index 0/1");
        CHECK(gLayout.tracks[1].dataLba == msf(4, 34, 36) && gLayout.tracks[1].byteOffset == msf(4, 34, 36) * 2352, "track 2 data from INDEX 00");
        CHECK(gLayout.tracks[0].frames == msf(4, 34, 36), "track 1 frames up to track 2's pregap");
        CHECK(gLayout.tracks[9].index1 == msf(25, 39, 0) && gLayout.tracks[9].dataLba + gLayout.tracks[9].frames == 120166, "last track runs to the end");
        CHECK(cue_audioTracks(gLayout) == 10 && gLayout.discType == 0x00, "all audio, CD-DA");
        u32 sum = 0; for (u32 i = 0; i < 10; i++) sum += gLayout.tracks[i].frames;
        CHECK(sum == 120166, "track frames cover the whole bin (%u)", sum);
    }
    // ---- CD-Text from the real .cdt (stored rotated by 12 bytes) ----
    {
        std::string p = std::string(dir) + "/disney.cdt";
        auto cdt = readFile(p.c_str());
        std::vector<u8> scratch(cdt.size());
        u32 n = cue_normalizeCdText(cdt.data(), cdt.size(), scratch.data());
        CHECK(n == 63 * 18, "63 packs (%u bytes)", n);
        CHECK(cdt[0] == 0x80 && cdt[1] == 0 && cdt[2] == 0, "first pack: title, track 0, seq 0");
        bool seqOk = true;
        for (u32 i = 0; i < n / 18; i++) if (cdt[i * 18 + 2] != i) seqOk = false;
        CHECK(seqOk, "packs in sequence order");
        char title[40];
        cue_cdTextTitle(cdt.data(), n, title, sizeof(title));
        CHECK(!strcmp(title, "ICON: Disney Princess"), "album title '%s'", title);
        std::vector<u8> junk(100, 0x55);
        CHECK(cue_normalizeCdText(junk.data(), junk.size(), scratch.data()) == 0, "junk isn't CD-Text");
    }
    // ---- mixed-mode game CD, one bin per track (redump style) ----
    {
        const char* cue =
            "FILE \"Game (Track 1).bin\" BINARY\n"
            "  TRACK 01 MODE1/2352\n"
            "    INDEX 01 00:00:00\n"
            "FILE \"Game (Track 2).bin\" BINARY\n"
            "  TRACK 02 AUDIO\n"
            "    INDEX 00 00:00:00\n"
            "    INDEX 01 00:02:00\n"
            "FILE \"Game (Track 3).bin\" BINARY\n"
            "  TRACK 03 AUDIO\n"
            "    INDEX 01 00:00:00\n";
        u32 total;
        CueError e = build(cue, { 1000 * 2352, 500 * 2352, 300 * 2352 + 100 }, &total);
        CHECK(e == CueError::None, "multi-file: %s", cue_errorString(e));
        CHECK(!strcmp(gSheet.files[1], "Game (Track 2).bin"), "quoted name with spaces");
        CHECK(gLayout.tracks[0].mode == CD_TRACK_MODE1 && gLayout.tracks[0].userOffset == 16 && gLayout.tracks[0].frames == 1000, "data track");
        CHECK(gLayout.tracks[1].index0 == 1000 && gLayout.tracks[1].index1 == 1150 && gLayout.tracks[1].dataLba == 1000, "track 2 follows file 1");
        u32 f1 = (1000 * 2352 + 511) / 512, f2 = (500 * 2352 + 511) / 512;
        CHECK(gLayout.tracks[1].byteOffset == f1 * 512, "file 2 starts at its padded sector (%u)", gLayout.tracks[1].byteOffset);
        CHECK(gLayout.tracks[2].byteOffset == (f1 + f2) * 512 && gLayout.tracks[2].index1 == 1500, "file 3");
        CHECK(gLayout.tracks[2].frames == 300 && gLayout.leadout == 1800, "partial last frame ignored, leadout %u", gLayout.leadout);
        CHECK(total == f1 + f2 + (300 * 2352 + 100 + 511) / 512, "virtual size");
        CHECK(cue_audioTracks(gLayout) == 2, "2 audio tracks");
    }
    // ---- single-bin mixed CD with PREGAP (silence not in the file), MODE2 ----
    {
        const char* cue =
            "FILE game.bin BINARY\r\n"
            "  TRACK 1 MODE2/2352\r\n"
            "    INDEX 1 00:00:00\r\n"
            "  TRACK 2 AUDIO\r\n"
            "    PREGAP 00:02:00\r\n"
            "    INDEX 1 00:10:00\r\n";
        CueError e = build(cue, { 2000ull * 2352 });
        CHECK(e == CueError::None, "pregap: %s", cue_errorString(e));
        CHECK(gLayout.discType == 0x20 && gLayout.tracks[0].userOffset == 24, "XA, mode 2 user data at 24");
        CHECK(gLayout.tracks[0].frames == 750, "track 1 = 10 s");
        CHECK(gLayout.tracks[1].index0 == 750 && gLayout.tracks[1].index1 == 900 && gLayout.tracks[1].dataLba == 900, "PREGAP shifts track 2 (%u/%u/%u)", gLayout.tracks[1].index0, gLayout.tracks[1].index1, gLayout.tracks[1].dataLba);
        CHECK(gLayout.tracks[1].byteOffset == 750 * 2352 && gLayout.tracks[1].frames == 1250, "track 2 data");
        CHECK(gLayout.leadout == 2150, "leadout includes the pregap (%u)", gLayout.leadout);
    }
    // ---- cooked ISO-style cue ----
    {
        CueError e = build("FILE \"disc.iso\" BINARY\nTRACK 01 MODE1/2048\nINDEX 01 00:00:00\n", { 4096 * 2048 });
        CHECK(e == CueError::None && gLayout.tracks[0].sectorSize == 2048 && gLayout.tracks[0].userOffset == 0 && gLayout.leadout == 4096, "MODE1/2048");
    }
    // ---- refusals ----
    {
        CHECK(build("FILE x.wav WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n", { 1000 }) == CueError::UnsupportedFile, "WAVE refused");
        CHECK(build("FILE x.bin BINARY\nTRACK 01 CDG\nINDEX 01 00:00:00\n", { 1000 }) == CueError::UnsupportedMode, "CDG refused");
        CHECK(build("TRACK 01 AUDIO\nINDEX 01 00:00:00\n", { 1000 }) == CueError::TrackBeforeFile, "track before file");
        CHECK(build("FILE x.bin BINARY\nTRACK 01 AUDIO\n", { 23520 }) == CueError::MissingIndex1, "no INDEX 01");
        CHECK(build("FILE x.bin BINARY\nTRACK 01 AUDIO\nINDEX 01 00:00:80\n", { 23520 }) == CueError::Syntax, "bad MSF");
        CHECK(build("FILE x.bin BINARY\nTRACK 01 AUDIO\nINDEX 01 00:00:00\nTRACK 02 AUDIO\nINDEX 01 01:00:00\n", { 10 * 2352 }) == CueError::TrackPastEndOfFile, "track past end");
        CHECK(build("FILE x.bin BINARY\nTRACK 01 AUDIO\nINDEX 01 00:05:00\nTRACK 02 AUDIO\nINDEX 01 00:01:00\n", { 1000 * 2352 }) == CueError::BadIndexOrder, "out of order");
        CHECK(build("", {}) == CueError::NoTracks, "empty");
        u32 line = 0;
        const char* bad = "FILE x.bin BINARY\nTRACK 01 AUDIO\nINDEX 01 bogus\n";
        CHECK(cue_parse(bad, strlen(bad), gSheet, &line) == CueError::Syntax && line == 3, "error line %u", line);
    }
    printf("cue_test: %d checks passed, %d failed\n", gPasses, gFails);
    return gFails ? 1 : 0;
}
