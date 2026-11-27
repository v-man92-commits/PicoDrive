#pragma once
#include <nds/disc_io.h>
bool fatMount(const char*, const DISC_INTERFACE*, sec_t, uint32_t, uint32_t);
void fatUnmount(const char*);
