// POSIX file syscalls for the BlocksDS build, backed by the image mounter's own
// FatFs instance (FAT12/16/32 + exFAT, disk access through the ARM7 DLDI).
//
// BlocksDS normally provides these in libnds (syscalls_filesystem.c.o, which
// routes "fat:"/"sd:" to its own FatFs). Defining every symbol of that object
// (here and in fsDirent.cpp) means the linker never pulls it in, so picolibc's
// fopen/fread/stat/... land on our FatFs instead. Paths "sd:/x" and "fat:/x"
// both map to FatFs "/x".
//
// File descriptors:
//   0, 1, 2              stdin / stdout / stderr (libnds console)
//   FS_FD_FILE | slot    open files
//   other (bits 28-30 clear) sockets: forwarded to DSWiFi through socket_fn_*
//
// This file includes ff.h (FatFs's DIR) but not <dirent.h> (POSIX DIR); the
// directory functions live in fsDirent.cpp and use the ffdir_* helpers below.
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include "ff.h"
#include "fsSyscalls.h"

extern "C"
{
    ssize_t write_stdout_libnds(int unused, const void* ptr, size_t len);
    ssize_t write_stderr_libnds(int unused, const void* ptr, size_t len);

    // Hooks filled in by DSWiFi (dswifi_lwip_setup_io_posix) and the console.
    ssize_t (*socket_fn_write)(int, const void*, size_t);
    ssize_t (*socket_fn_read)(int, void*, size_t);
    int (*socket_fn_close)(int);
    int (*socket_fn_ioctl)(int, long, void*);
    int (*socket_fn_fcntl)(int, int, int);
    ssize_t (*stdin_fn_read)(int, void*, size_t);
    int (*stdin_fn_ioctl)(int, long, void*);
    int (*stdin_fn_fcntl)(int, int, int);
}

namespace
{
    struct FileSlot
    {
        bool used;
        bool append;
        FIL fil;
    };

    FileSlot sFiles[FS_MAX_FILES];

    struct DirSlot
    {
        bool used;
        DIR dir;
    };

    DirSlot sDirs[FS_MAX_DIRS];

    int setErrno(int e)
    {
        errno = e;
        return -1;
    }

    int fail(FRESULT r)
    {
        return setErrno(fs_errnoFromFresult(r));
    }

    bool isRoot(const char* p)
    {
        return p[0] == 0 || (p[0] == '/' && p[1] == 0);
    }

    FileSlot* fileFromFd(int fd)
    {
        if ((fd & FS_FD_KIND_MASK) != FS_FD_FILE)
            return nullptr;
        int slot = fd & 0xFFFF;
        if (slot < 0 || slot >= FS_MAX_FILES || !sFiles[slot].used)
            return nullptr;
        return &sFiles[slot];
    }

    bool isSocketFd(int fd)
    {
        return fd > 2 && (fd & FS_FD_KIND_MASK) == 0;
    }

    time_t fatTime(WORD date, WORD time)
    {
        struct tm t = {};
        t.tm_year = ((date >> 9) & 0x7F) + 80;
        t.tm_mon = ((date >> 5) & 0x0F) - 1;
        t.tm_mday = date & 0x1F;
        t.tm_hour = (time >> 11) & 0x1F;
        t.tm_min = (time >> 5) & 0x3F;
        t.tm_sec = (time & 0x1F) * 2;
        return mktime(&t);
    }

    void fillStat(struct stat* st, const FILINFO& fno)
    {
        memset(st, 0, sizeof(*st));
        st->st_mode = (fno.fattrib & AM_DIR) ? (S_IFDIR | 0777) : (S_IFREG | 0666);
        if (fno.fattrib & AM_RDO)
            st->st_mode &= ~0222;
        st->st_size = (off_t)fno.fsize;
        st->st_nlink = 1;
        st->st_blksize = 512;
        st->st_blocks = (fno.fsize + 511) / 512;
        st->st_mtime = st->st_atime = st->st_ctime = fatTime(fno.fdate, fno.ftime);
    }
}

extern "C" int fs_errnoFromFresult(int r)
{
    switch ((FRESULT)r)
    {
        case FR_OK:                  return 0;
        case FR_NO_FILE:
        case FR_NO_PATH:
        case FR_INVALID_NAME:        return ENOENT;
        case FR_DENIED:              return ENOSPC;
        case FR_EXIST:               return EEXIST;
        case FR_WRITE_PROTECTED:     return EROFS;
        case FR_INVALID_OBJECT:      return EBADF;
        case FR_NOT_ENOUGH_CORE:     return ENOMEM;
        case FR_TOO_MANY_OPEN_FILES: return EMFILE;
        case FR_INVALID_PARAMETER:   return EINVAL;
        case FR_LOCKED:              return EBUSY;
        case FR_NOT_ENABLED:
        case FR_NO_FILESYSTEM:       return ENODEV;
        default:                     return EIO;
    }
}

extern "C" const char* fs_ffPath(const char* path)
{
    const char* colon = strchr(path, ':');
    return colon ? colon + 1 : path;
}

extern "C" int fs_fileSize64(const char* path, unsigned long long* size, int* isDir)
{
    const char* p = fs_ffPath(path);
    if (isRoot(p))
    {
        *size = 0;
        if (isDir)
            *isDir = 1;
        return 0;
    }
    FILINFO fno;
    FRESULT res = f_stat(p, &fno);
    if (res != FR_OK)
        return fail(res);
    *size = (unsigned long long)fno.fsize;
    if (isDir)
        *isDir = (fno.fattrib & AM_DIR) ? 1 : 0;
    return 0;
}

// ---------------------------------------------------------------------------
// Directory helpers for fsDirent.cpp

extern "C" int ffdir_open(const char* path)
{
    for (int i = 0; i < FS_MAX_DIRS; i++)
    {
        if (sDirs[i].used)
            continue;
        FRESULT res = f_opendir(&sDirs[i].dir, fs_ffPath(path));
        if (res != FR_OK)
            return fail(res);
        sDirs[i].used = true;
        return i;
    }
    return setErrno(EMFILE);
}

extern "C" int ffdir_next(int handle, char* name, size_t nameSize, int* isDir)
{
    if (handle < 0 || handle >= FS_MAX_DIRS || !sDirs[handle].used)
        return setErrno(EBADF);
    FILINFO fno;
    FRESULT res = f_readdir(&sDirs[handle].dir, &fno);
    if (res != FR_OK)
        return fail(res);
    if (fno.fname[0] == 0)
        return 0; // end of directory
    strncpy(name, fno.fname, nameSize - 1);
    name[nameSize - 1] = 0;
    *isDir = (fno.fattrib & AM_DIR) ? 1 : 0;
    return 1;
}

extern "C" int ffdir_rewind(int handle)
{
    if (handle < 0 || handle >= FS_MAX_DIRS || !sDirs[handle].used)
        return setErrno(EBADF);
    FRESULT res = f_rewinddir(&sDirs[handle].dir);
    return res == FR_OK ? 0 : fail(res);
}

extern "C" int ffdir_close(int handle)
{
    if (handle < 0 || handle >= FS_MAX_DIRS || !sDirs[handle].used)
        return setErrno(EBADF);
    f_closedir(&sDirs[handle].dir);
    sDirs[handle].used = false;
    return 0;
}

// ---------------------------------------------------------------------------
// File syscalls

extern "C" int open(const char* path, int flags, ...)
{
    BYTE fm = 0;
    switch (flags & O_ACCMODE)
    {
        case O_RDONLY: fm = FA_READ; break;
        case O_WRONLY: fm = FA_WRITE; break;
        case O_RDWR:   fm = FA_READ | FA_WRITE; break;
        default: return setErrno(EINVAL);
    }
    if ((flags & O_CREAT) && (flags & O_EXCL))
        fm |= FA_CREATE_NEW;
    else if ((flags & O_CREAT) && (flags & O_TRUNC))
        fm |= FA_CREATE_ALWAYS;
    else if (flags & O_CREAT)
        fm |= FA_OPEN_ALWAYS;
    else
        fm |= FA_OPEN_EXISTING;

    int slot = -1;
    for (int i = 0; i < FS_MAX_FILES; i++)
    {
        if (!sFiles[i].used)
        {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return setErrno(EMFILE);

    FileSlot* fs = &sFiles[slot];
    FRESULT res = f_open(&fs->fil, fs_ffPath(path), fm);
    if (res != FR_OK)
        return fail(res);
    if ((flags & O_TRUNC) && !(flags & O_CREAT) && (fm & FA_WRITE))
    {
        res = f_truncate(&fs->fil);
        if (res != FR_OK)
        {
            f_close(&fs->fil);
            return fail(res);
        }
    }
    fs->append = (flags & O_APPEND) != 0;
    fs->used = true;
    return FS_FD_FILE | slot;
}

extern "C" int close(int fd)
{
    if (isSocketFd(fd))
        return socket_fn_close ? socket_fn_close(fd) : setErrno(EBADF);
    if (fd >= 0 && fd <= 2)
        return 0;
    FileSlot* fs = fileFromFd(fd);
    if (!fs)
        return setErrno(EBADF);
    FRESULT res = f_close(&fs->fil);
    fs->used = false;
    return res == FR_OK ? 0 : fail(res);
}

extern "C" ssize_t read(int fd, void* ptr, size_t len)
{
    if (fd == 0)
        return stdin_fn_read ? stdin_fn_read(fd, ptr, len) : 0;
    if (isSocketFd(fd))
        return socket_fn_read ? socket_fn_read(fd, ptr, len) : setErrno(EBADF);
    FileSlot* fs = fileFromFd(fd);
    if (!fs)
        return setErrno(EBADF);
    UINT br = 0;
    FRESULT res = f_read(&fs->fil, ptr, len, &br);
    return res == FR_OK ? (ssize_t)br : fail(res);
}

extern "C" ssize_t write(int fd, const void* ptr, size_t len)
{
    if (fd == 1)
        return write_stdout_libnds(0, ptr, len);
    if (fd == 2)
        return write_stderr_libnds(0, ptr, len);
    if (fd == 0)
        return setErrno(EBADF);
    if (isSocketFd(fd))
        return socket_fn_write ? socket_fn_write(fd, ptr, len) : setErrno(EBADF);
    FileSlot* fs = fileFromFd(fd);
    if (!fs)
        return setErrno(EBADF);
    if (fs->append)
    {
        FRESULT res = f_lseek(&fs->fil, f_size(&fs->fil));
        if (res != FR_OK)
            return fail(res);
    }
    UINT bw = 0;
    FRESULT res = f_write(&fs->fil, ptr, len, &bw);
    if (res != FR_OK)
        return fail(res);
    if (bw == 0 && len > 0)
        return setErrno(ENOSPC); // volume full (or the 4 GiB limit on FAT)
    return bw;
}

extern "C" off_t lseek(int fd, off_t pos, int whence)
{
    FileSlot* fs = fileFromFd(fd);
    if (!fs)
        return setErrno(fd >= 0 && fd <= 2 ? ESPIPE : EBADF);
    FIL* f = &fs->fil;
    int64_t base;
    switch (whence)
    {
        case SEEK_SET: base = 0; break;
        case SEEK_CUR: base = (int64_t)f_tell(f); break;
        case SEEK_END: base = (int64_t)f_size(f); break;
        default: return setErrno(EINVAL);
    }
    int64_t target = base + pos;
    if (target < 0)
        return setErrno(EINVAL);
    // FatFs extends a writable file when seeking past its end.
    FRESULT res = f_lseek(f, (FSIZE_t)target);
    if (res != FR_OK)
        return fail(res);
    return (off_t)f_tell(f);
}

extern "C" int fstat(int fd, struct stat* st)
{
    memset(st, 0, sizeof(*st));
    if (fd >= 0 && fd <= 2)
    {
        st->st_mode = S_IFCHR | 0666;
        return 0;
    }
    if (isSocketFd(fd))
    {
        st->st_mode = S_IFSOCK | 0666;
        return 0;
    }
    FileSlot* fs = fileFromFd(fd);
    if (!fs)
        return setErrno(EBADF);
    FIL* f = &fs->fil;
    st->st_mode = S_IFREG | 0666;
    st->st_size = (off_t)f_size(f);
    st->st_ino = f->obj.sclust;
    st->st_nlink = 1;
    st->st_blksize = 512;
    st->st_blocks = (f_size(f) + 511) / 512;
    return 0;
}

extern "C" int stat(const char* path, struct stat* st)
{
    const char* p = fs_ffPath(path);
    if (isRoot(p))
    {
        memset(st, 0, sizeof(*st));
        st->st_mode = S_IFDIR | 0777;
        return 0;
    }
    FILINFO fno;
    FRESULT res = f_stat(p, &fno);
    if (res != FR_OK)
        return fail(res);
    fillStat(st, fno);
    return 0;
}

extern "C" int lstat(const char* path, struct stat* st)
{
    return stat(path, st);
}

extern "C" int access(const char* path, int mode)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    if ((mode & W_OK) && !(st.st_mode & 0222))
        return setErrno(EACCES);
    return 0;
}

extern "C" int unlink(const char* path)
{
    FRESULT res = f_unlink(fs_ffPath(path));
    return res == FR_OK ? 0 : fail(res);
}

extern "C" int rmdir(const char* path)
{
    FRESULT res = f_unlink(fs_ffPath(path));
    return res == FR_OK ? 0 : fail(res);
}

extern "C" int rename(const char* oldPath, const char* newPath)
{
    FRESULT res = f_rename(fs_ffPath(oldPath), fs_ffPath(newPath));
    return res == FR_OK ? 0 : fail(res);
}

extern "C" int mkdir(const char* path, mode_t mode)
{
    (void)mode;
    FRESULT res = f_mkdir(fs_ffPath(path));
    return res == FR_OK ? 0 : fail(res);
}

extern "C" int statvfs(const char* path, struct statvfs* buf)
{
    (void)path;
    DWORD freeClusters;
    FATFS* fs;
    FRESULT res = f_getfree("", &freeClusters, &fs);
    if (res != FR_OK)
        return fail(res);
    memset(buf, 0, sizeof(*buf));
    buf->f_bsize = fs->csize * 512;
    buf->f_frsize = fs->csize * 512;
    buf->f_blocks = fs->n_fatent - 2;
    buf->f_bfree = freeClusters;
    buf->f_bavail = freeClusters;
    buf->f_namemax = FF_MAX_LFN;
    return 0;
}

extern "C" int fstatvfs(int fd, struct statvfs* buf)
{
    if (!fileFromFd(fd))
        return setErrno(EBADF);
    return statvfs("", buf);
}

extern "C" int ftruncate(int fd, off_t len)
{
    FileSlot* fs = fileFromFd(fd);
    if (!fs)
        return setErrno(EBADF);
    if (len < 0)
        return setErrno(EINVAL);
    FIL* f = &fs->fil;
    FSIZE_t pos = f_tell(f);
    FRESULT res = f_lseek(f, (FSIZE_t)len);
    if (res == FR_OK && (FSIZE_t)len < f_size(f))
        res = f_truncate(f);
    if (res == FR_OK)
        res = f_lseek(f, pos < (FSIZE_t)len ? pos : (FSIZE_t)len);
    return res == FR_OK ? 0 : fail(res);
}

extern "C" int truncate(const char* path, off_t len)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return -1;
    int r = ftruncate(fd, len);
    int e = errno;
    close(fd);
    errno = e;
    return r;
}

extern "C" int fsync(int fd)
{
    FileSlot* fs = fileFromFd(fd);
    if (!fs)
        return (fd >= 0 && fd <= 2) ? 0 : setErrno(EBADF);
    FRESULT res = f_sync(&fs->fil);
    return res == FR_OK ? 0 : fail(res);
}

extern "C" int fdatasync(int fd)
{
    return fsync(fd);
}

extern "C" int isatty(int fd)
{
    if (fd >= 0 && fd <= 2)
        return 1;
    errno = ENOTTY;
    return 0;
}

extern "C" int fcntl(int fd, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    int arg = va_arg(ap, int);
    va_end(ap);
    if (isSocketFd(fd))
        return socket_fn_fcntl ? socket_fn_fcntl(fd, cmd, arg) : setErrno(EBADF);
    if (fd == 0 && stdin_fn_fcntl)
        return stdin_fn_fcntl(fd, cmd, arg);
    if (cmd == F_GETFL)
        return fileFromFd(fd) ? O_RDWR : 0;
    return 0;
}

extern "C" int ioctl(int fd, int request, ...)
{
    va_list ap;
    va_start(ap, request);
    void* arg = va_arg(ap, void*);
    va_end(ap);
    if (isSocketFd(fd))
        return socket_fn_ioctl ? socket_fn_ioctl(fd, request, arg) : setErrno(EBADF);
    if (fd == 0 && stdin_fn_ioctl)
        return stdin_fn_ioctl(fd, request, arg);
    return setErrno(ENOTTY);
}

// Unsupported on FAT: succeed quietly (permissions/ownership/times) or fail.
extern "C" int chmod(const char*, mode_t) { return 0; }
extern "C" int fchmod(int, mode_t) { return 0; }
extern "C" int fchmodat(int, const char*, mode_t, int) { return 0; }
extern "C" int chown(const char*, uid_t, gid_t) { return 0; }
extern "C" int fchown(int, uid_t, gid_t) { return 0; }
extern "C" int fchownat(int, const char*, uid_t, gid_t, int) { return 0; }
extern "C" int link(const char*, const char*) { return setErrno(ENOTSUP); }
extern "C" int symlink(const char*, const char*) { return setErrno(ENOTSUP); }
extern "C" ssize_t readlink(const char*, char*, size_t) { return setErrno(EINVAL); }
extern "C" int utime(const char*, const void*) { return 0; }
extern "C" int utimes(const char*, const void*) { return 0; }
extern "C" int lutimes(const char*, const void*) { return 0; }
