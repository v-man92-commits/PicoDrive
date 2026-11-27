// Renames the BlocksDS file syscalls so they can be tested on the host
// without replacing the host libc. All system headers are pulled in first
// (un-renamed, include-guarded); only code after this point sees the names.
#pragma once
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#ifdef FS_RENAME_WITH_DIRENT
#include <dirent.h>
#endif
#ifdef __cplusplus
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <set>
#endif
#define open fsx_open
#define close fsx_close
#define read fsx_read
#define write fsx_write
#define lseek fsx_lseek
#define fstat fsx_fstat
#define stat(p, s) fsx_stat(p, s)
#define lstat fsx_lstat
#define access fsx_access
#define unlink fsx_unlink
#define rmdir fsx_rmdir
#define rename fsx_rename
#define mkdir fsx_mkdir
#define statvfs(p, b) fsx_statvfs(p, b)
#define fstatvfs fsx_fstatvfs
#define ftruncate fsx_ftruncate
#define truncate fsx_truncate
#define fsync fsx_fsync
#define fdatasync fsx_fdatasync
#define isatty fsx_isatty
#define fcntl fsx_fcntl
#define ioctl fsx_ioctl
#define chmod fsx_chmod
#define fchmod fsx_fchmod
#define fchmodat fsx_fchmodat
#define chown fsx_chown
#define fchown fsx_fchown
#define fchownat fsx_fchownat
#define link fsx_link
#define symlink fsx_symlink
#define readlink fsx_readlink
#define utime fsx_utime
#define utimes fsx_utimes
#define lutimes fsx_lutimes
#define opendir fsx_opendir
#define readdir fsx_readdir
#define closedir fsx_closedir
#define rewinddir fsx_rewinddir
#define telldir fsx_telldir
#define seekdir fsx_seekdir
#define socket_fn_write fsx_socket_fn_write
#define socket_fn_read fsx_socket_fn_read
#define socket_fn_close fsx_socket_fn_close
#define socket_fn_ioctl fsx_socket_fn_ioctl
#define socket_fn_fcntl fsx_socket_fn_fcntl
#define stdin_fn_read fsx_stdin_fn_read
#define stdin_fn_ioctl fsx_stdin_fn_ioctl
#define stdin_fn_fcntl fsx_stdin_fn_fcntl
