// host stand-in for devkitARM's <sys/iosupport.h> (exFAT build tests)
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
typedef struct devoptab_t devoptab_t;
int AddDevice(const devoptab_t* device);
int FindDevice(const char* name);
int RemoveDevice(const char* name);
#ifdef __cplusplus
}
#endif
