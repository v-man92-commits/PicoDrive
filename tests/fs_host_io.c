// Host side of fs_syscalls_test: the disk image file and FatFs setup
// (compiled without fs_rename.h, so it uses the real libc).
#include <fcntl.h>
#include <unistd.h>
#include "ff.h"
#include "diskio.h"
static int gDisk = -1;
static unsigned long long gSectors;
DSTATUS disk_initialize(BYTE p) { (void)p; return 0; }
DSTATUS disk_status(BYTE p) { (void)p; return 0; }
DRESULT disk_read(BYTE p, BYTE* b, LBA_t s, UINT n) { (void)p; return pread(gDisk, b, n * 512, (off_t)s * 512) == (ssize_t)(n * 512) ? RES_OK : RES_ERROR; }
DRESULT disk_write(BYTE p, const BYTE* b, LBA_t s, UINT n) { (void)p; return pwrite(gDisk, b, n * 512, (off_t)s * 512) == (ssize_t)(n * 512) ? RES_OK : RES_ERROR; }
DRESULT disk_ioctl(BYTE p, BYTE cmd, void* buf)
{
    (void)p;
    switch (cmd) {
        case CTRL_SYNC: return RES_OK;
        case GET_SECTOR_COUNT: *(LBA_t*)buf = (LBA_t)gSectors; return RES_OK;
        case GET_BLOCK_SIZE: *(DWORD*)buf = 8192; return RES_OK;
    }
    return RES_PARERR;
}
DWORD get_fattime(void) { return ((DWORD)(2026 - 1980) << 25) | (11 << 21) | (27 << 16); }

static FATFS sFs;
static BYTE sWork[64 * 1024];

// Creates the image, formats it (exfat=1: exFAT 32 KiB clusters, else FAT32 4 KiB)
// and mounts it. Returns the FatFs fs_type, or -1.
int host_setup(const char* path, unsigned long long mib, int exfat)
{
    gSectors = mib * 2048;
    gDisk = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (gDisk < 0 || ftruncate(gDisk, (off_t)gSectors * 512) != 0)
        return -1;
    MKFS_PARM opt = { (BYTE)(exfat ? FM_EXFAT : FM_FAT32), 0, 0, 0, exfat ? 32768u : 4096u };
    if (f_mkfs("", &opt, sWork, sizeof(sWork)) != FR_OK)
        return -1;
    if (f_mount(&sFs, "", 1) != FR_OK)
        return -1;
    return sFs.fs_type;
}
int host_type_exfat(void) { return FS_EXFAT; }
int host_type_fat32(void) { return FS_FAT32; }
void host_teardown(void) { f_unmount(""); close(gDisk); }

// Makes a file of `bytes` by seeking past its end (FatFs allocates clusters
// without writing data). Returns 0 on success.
int host_make_sized(const char* path, unsigned long long bytes)
{
    FIL f;
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        return -1;
    FRESULT r = f_lseek(&f, (FSIZE_t)bytes);
    if (r == FR_OK && f_tell(&f) != bytes)
        r = FR_DENIED;
    return (f_close(&f) == FR_OK && r == FR_OK) ? 0 : -1;
}
