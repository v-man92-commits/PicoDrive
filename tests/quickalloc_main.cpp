// Host harness: runs quickAlloc_allocateRaw() on a disk image.
// Usage: quickalloc_main disk.img dirCluster placeholderCluster bytes
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "common.h"
#include "QuickAlloc.h"

static int gDisk;
extern "C" bool dldi_readSectors(void* b, u32 s, u32 n) { return pread(gDisk, b, n * 512, (off_t)s * 512) == (ssize_t)(n * 512); }
// QA_CRASH_AFTER=N simulates power loss: every write after the first N is lost
static long gCrashAfter = -1, gWrites = 0;
extern "C" bool dldi_writeSectors(const void* b, u32 s, u32 n)
{
    long index = gWrites++;
    if (gCrashAfter >= 0 && index >= gCrashAfter) return false;
    return pwrite(gDisk, b, n * 512, (off_t)s * 512) == (ssize_t)(n * 512);
}
sec_t sd_findPartitionStart(u8* buf)
{
    dldi_readSectors(buf, 0, 1);
    if (!memcmp(buf + 0x36, "FAT", 3) || !memcmp(buf + 0x52, "FAT", 3)) return 0;
    return buf[0x1C6] | (buf[0x1C7] << 8) | (buf[0x1C8] << 16) | ((u32)buf[0x1C9] << 24);
}
static const char* names[] = { "None", "IoError", "Unsupported", "NoSpace", "TooFragmented", "CreateFailed", "EntryNotFound", "VerifyFailed" };
int main(int argc, char** argv)
{
    gDisk = open(argv[1], O_RDWR);
    if (getenv("QA_CRASH_AFTER")) gCrashAfter = atol(getenv("QA_CRASH_AFTER"));
    AllocResult r;
    AllocError e = quickAlloc_allocateRaw(strtoul(argv[2], 0, 0), strtoul(argv[3], 0, 0), strtoull(argv[4], 0, 0), r, nullptr);
    printf("%s %u %u %u %ld\n", names[(int)e], r.firstCluster, r.clusters, r.runs, gWrites);
    return 0;
}
