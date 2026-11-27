// FatFs disk I/O for the image mounter's exFAT build: the SD card is reached
// through the ARM7's DLDI driver over IPC (same path libfat uses).
#include "common.h"
#include <string.h>
#include <time.h>
#include "ff.h"
#include "diskio.h"
#include "dldiIpc.h"

extern "C" {

DSTATUS disk_initialize(BYTE pdrv) { return pdrv == 0 ? 0 : STA_NOINIT; }
DSTATUS disk_status(BYTE pdrv) { return pdrv == 0 ? 0 : STA_NOINIT; }

// DLDI IPC transfers go through a cache-aligned bounce buffer when the
// caller's buffer isn't 32-byte aligned (FatFs passes its own buffers and
// user buffers straight through).
alignas(32) static u8 sBounce[16 * 512];

static bool transfer(BYTE* buff, LBA_t sector, UINT count, bool write)
{
    if (((u32)buff & 31) == 0)
        return write ? dldi_writeSectors(buff, sector, count) : dldi_readSectors(buff, sector, count);
    while (count)
    {
        UINT n = count > 16 ? 16 : count;
        if (write)
        {
            memcpy(sBounce, buff, n * 512);
            if (!dldi_writeSectors(sBounce, sector, n))
                return false;
        }
        else
        {
            if (!dldi_readSectors(sBounce, sector, n))
                return false;
            memcpy(buff, sBounce, n * 512);
        }
        buff += n * 512;
        sector += n;
        count -= n;
    }
    return true;
}

DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count)
{
    if (pdrv != 0)
        return RES_PARERR;
    return transfer(buff, sector, count, false) ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count)
{
    if (pdrv != 0)
        return RES_PARERR;
    return transfer((BYTE*)buff, sector, count, true) ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff)
{
    if (pdrv != 0)
        return RES_PARERR;
    return cmd == CTRL_SYNC ? RES_OK : RES_PARERR; // every write is already on the card
}

DWORD get_fattime(void)
{
    time_t now = time(nullptr);
    struct tm* t = gmtime(&now);
    if (!t || t->tm_year < 80)
        return ((DWORD)(2026 - 1980) << 25) | (1 << 21) | (1 << 16);
    return ((DWORD)(t->tm_year - 80) << 25) | ((DWORD)(t->tm_mon + 1) << 21) | ((DWORD)t->tm_mday << 16) |
        ((DWORD)t->tm_hour << 11) | ((DWORD)t->tm_min << 5) | ((DWORD)t->tm_sec >> 1);
}

}
