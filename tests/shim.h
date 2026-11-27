// force-included into FatMap.cpp for the host test
#pragma once
#include <stdio.h>
#include <sys/stat.h>
extern "C" int test_stat(const char* path, struct stat* st);
extern "C" FILE* test_fopen(const char* path, const char* mode);
extern "C" int test_fsync(int fd);
#define stat(p, s) test_stat(p, s)
#define fopen(p, m) test_fopen(p, m)
#define fsync(fd) test_fsync(fd)
