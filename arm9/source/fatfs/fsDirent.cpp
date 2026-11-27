// POSIX directory functions for the BlocksDS build (see fsSyscalls.cpp).
// Includes <dirent.h> (POSIX DIR) but not ff.h (FatFs DIR).
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "fsSyscalls.h"

extern "C" DIR* opendir(const char* path)
{
    int h = ffdir_open(path);
    if (h < 0)
        return nullptr;
    DIR* d = (DIR*)calloc(1, sizeof(DIR));
    if (!d)
    {
        ffdir_close(h);
        errno = ENOMEM;
        return nullptr;
    }
    d->dp = (void*)(intptr_t)(h + 1);
    d->index = 0;
    return d;
}

extern "C" struct dirent* readdir(DIR* d)
{
    if (!d)
    {
        errno = EBADF;
        return nullptr;
    }
    int isDir = 0;
    int r = ffdir_next((int)(intptr_t)d->dp - 1, d->dirent.d_name, sizeof(d->dirent.d_name), &isDir);
    if (r <= 0)
        return nullptr; // end (errno untouched) or error (errno set)
    d->dirent.d_ino = (ino_t)(d->index + 1);
    d->dirent.d_type = isDir ? DT_DIR : DT_REG;
    d->index++;
    return &d->dirent;
}

extern "C" int closedir(DIR* d)
{
    if (!d)
    {
        errno = EBADF;
        return -1;
    }
    int r = ffdir_close((int)(intptr_t)d->dp - 1);
    free(d);
    return r;
}

extern "C" void rewinddir(DIR* d)
{
    if (!d)
        return;
    ffdir_rewind((int)(intptr_t)d->dp - 1);
    d->index = 0;
}

extern "C" long telldir(DIR* d)
{
    return d ? (long)d->index : -1;
}

extern "C" void seekdir(DIR* d, long loc)
{
    if (!d || loc < 0)
        return;
    rewinddir(d);
    while (d->index < loc && readdir(d))
        ;
}
