// Host test for arm9/source/cueImage.cpp against real files in a folder.
#include <stdio.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include "common.h"
#include "cueImage.h"

static int gPasses, gFails;
#define CHECK(c, ...) do { if (c) gPasses++; else { gFails++; printf("  FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void writeFile(const std::string& path, const void* data, size_t n)
{
    FILE* f = fopen(path.c_str(), "wb"); fwrite(data, 1, n, f); fclose(f);
}

int main(int argc, char** argv)
{
    std::string data = argv[1], dir = argv[2];
    mkdir(dir.c_str(), 0777);
    CHECK(cueImage_isCueName("Disc.CUE") && cueImage_isCueName("a.cue") && !cueImage_isCueName(".cue") && !cueImage_isCueName("x.bin"), "cue names");
    CHECK(cueImage_isLocalName("a b.bin") && !cueImage_isLocalName("sub/a.bin") && !cueImage_isLocalName("sub\\a.bin") && !cueImage_isLocalName("C:a.bin"), "local names");
    char path[256];
    cueImage_path(path, sizeof(path), "sd:/images", "sub\\t1.bin");
    CHECK(!strcmp(path, "sd:/images/sub/t1.bin"), "path %s", path);

    // the real cue + cdt, with a stand-in .bin of the right size (sparse)
    FILE* in = fopen((data + "/disney.cue").c_str(), "rb"); char cue[4096]; size_t n = fread(cue, 1, sizeof(cue), in); fclose(in);
    writeFile(dir + "/Disney_icon_princess.cue", cue, n);
    in = fopen((data + "/disney.cdt").c_str(), "rb"); char cdt[4096]; size_t m = fread(cdt, 1, sizeof(cdt), in); fclose(in);
    writeFile(dir + "/Disney_icon_princess.cdt", cdt, m);
    static CueSheet sheet;
    u64 sizes[CUE_MAX_FILES];
    int missing;
    CHECK(cueImage_load(dir.c_str(), "Disney_icon_princess.cue", sheet, sizes, missing) == CueError::MissingFile && missing == 0, "missing .bin reported");
    FILE* b = fopen((dir + "/Disney_icon_princess.bin").c_str(), "wb"); fseek(b, 282630432 - 1, SEEK_SET); fputc(0, b); fclose(b);
    CHECK(cueImage_load(dir.c_str(), "Disney_icon_princess.cue", sheet, sizes, missing) == CueError::None && sizes[0] == 282630432, "load with sizes");
    static u8 text[CD_TEXT_MAX_BYTES];
    u32 len = cueImage_loadCdText(dir.c_str(), sheet, text);
    CHECK(len == 63 * 18 && text[0] == 0x80, "CD-Text loaded and normalized (%u)", len);
    CHECK(cueImage_parse(dir.c_str(), "nope.cue", sheet) == CueError::Unreadable, "missing .cue");
    // a too-big .cue is refused rather than cut off
    std::string big(CUE_MAX_TEXT + 10, ' ');
    writeFile(dir + "/big.cue", big.data(), big.size());
    CHECK(cueImage_parse(dir.c_str(), "big.cue", sheet) == CueError::Unreadable, "oversized .cue refused");
    printf("cueimage_test: %d checks passed, %d failed\n", gPasses, gFails);
    return gFails ? 1 : 0;
}
