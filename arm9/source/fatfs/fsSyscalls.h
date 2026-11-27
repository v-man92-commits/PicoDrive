#pragma once
// Shared bits of the BlocksDS file syscalls (fsSyscalls.cpp / fsDirent.cpp).
#include <stddef.h>

#define FS_FD_FILE      0x10000000
#define FS_FD_KIND_MASK 0x70000000
#define FS_MAX_FILES    8
#define FS_MAX_DIRS     4

#ifdef __cplusplus
extern "C" {
#endif

int fs_errnoFromFresult(int fresult);
const char* fs_ffPath(const char* path);

// 64-bit size of a file (stat()'s st_size is a 32-bit off_t in BlocksDS).
// Returns 0 on success, -1 (errno set) on error. *isDir set if non-NULL.
int fs_fileSize64(const char* path, unsigned long long* size, int* isDir);

// Directory access without exposing FatFs's DIR type.
int ffdir_open(const char* path);                                       // handle or -1
int ffdir_next(int handle, char* name, size_t nameSize, int* isDir);   // 1 entry, 0 end, -1 error
int ffdir_rewind(int handle);
int ffdir_close(int handle);

#ifdef __cplusplus
}
#endif
