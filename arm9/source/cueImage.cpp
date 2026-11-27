#include "common.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "cueImage.h"
#ifdef IMAGE_MOUNTER_FATFS_IO
#include "fsSyscalls.h"
#endif

namespace
{
    char sText[CUE_MAX_TEXT];
    u8 sCdTextScratch[CD_TEXT_MAX_BYTES + 18];
}

bool cueImage_isCueName(const char* name)
{
    size_t n = strlen(name);
    return n > 4 && !strcasecmp(name + n - 4, ".cue");
}

bool cueImage_isLocalName(const char* name)
{
    return name[0] && !strchr(name, '/') && !strchr(name, '\\') && !strchr(name, ':');
}

void cueImage_path(char* out, size_t outSize, const char* dir, const char* name)
{
    snprintf(out, outSize, "%s/%s", dir, name);
    for (char* p = out + strlen(dir) + 1; *p; p++)
        if (*p == '\\')
            *p = '/';
}

CueError cueImage_parse(const char* dir, const char* cueName, CueSheet& sheet)
{
    char path[CUE_NAME_LEN + 32];
    cueImage_path(path, sizeof(path), dir, cueName);
    FILE* f = fopen(path, "rb");
    if (!f)
        return CueError::Unreadable;
    size_t n = fread(sText, 1, sizeof(sText), f);
    bool tooBig = n == sizeof(sText) && fgetc(f) != EOF;
    fclose(f);
    if (tooBig)
        return CueError::Unreadable;
    return cue_parse(sText, (u32)n, sheet);
}

CueError cueImage_load(const char* dir, const char* cueName, CueSheet& sheet, u64* sizes, int& missingFile)
{
    missingFile = -1;
    CueError e = cueImage_parse(dir, cueName, sheet);
    if (e != CueError::None)
        return e;
    for (u32 i = 0; i < sheet.fileCount; i++)
    {
        char path[CUE_NAME_LEN + 32];
        cueImage_path(path, sizeof(path), dir, sheet.files[i]);
#ifdef IMAGE_MOUNTER_FATFS_IO
        // 64-bit size straight from FatFs (stat's st_size is 32-bit here)
        unsigned long long fileSize;
        int isDir;
        if (fs_fileSize64(path, &fileSize, &isDir) != 0 || isDir)
        {
            missingFile = (int)i;
            return CueError::MissingFile;
        }
        sizes[i] = (u64)fileSize;
#else
        struct stat st;
        if (stat(path, &st) != 0 || S_ISDIR(st.st_mode))
        {
            missingFile = (int)i;
            return CueError::MissingFile;
        }
        sizes[i] = (u64)st.st_size;
#endif
    }
    return CueError::None;
}

u32 cueImage_loadCdText(const char* dir, const CueSheet& sheet, u8* buf)
{
    if (!sheet.cdTextFile[0])
        return 0;
    char path[CUE_NAME_LEN + 32];
    cueImage_path(path, sizeof(path), dir, sheet.cdTextFile);
    FILE* f = fopen(path, "rb");
    if (!f)
        return 0;
    size_t n = fread(sCdTextScratch, 1, CD_TEXT_MAX_BYTES, f);
    fclose(f);
    memcpy(buf, sCdTextScratch, n);
    return cue_normalizeCdText(buf, (u32)n, sCdTextScratch);
}
