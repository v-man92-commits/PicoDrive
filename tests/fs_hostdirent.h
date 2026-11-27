// Host stand-in for BlocksDS's struct __dirstream (glibc keeps DIR opaque).
#pragma once
#include <stdint.h>
#include <sys/types.h>
#include <dirent.h>
struct __dirstream
{
    struct dirent dirent;
    void* dp;
    off_t index;
    uint8_t dptype;
};
