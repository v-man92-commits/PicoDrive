#pragma once
#include <nds/ndstypes.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef void (*FifoAddressHandlerFunc)(void *address, void *userdata);
typedef void (*FifoValue32HandlerFunc)(u32 value32, void *userdata);
typedef void (*FifoDatamsgHandlerFunc)(int num_bytes, void *userdata);
bool fifoInit(void);
bool fifoSendAddress(u32 channel, void *address);
bool fifoSendValue32(u32 channel, u32 value32);
bool fifoSendDatamsg(u32 channel, u32 num_bytes, u8 *data_array);
bool fifoSendSpecialCommand(u32 cmd);
bool fifoSetAddressHandler(u32 channel, FifoAddressHandlerFunc newhandler, void *userdata);
bool fifoSetValue32Handler(u32 channel, FifoValue32HandlerFunc newhandler, void *userdata);
bool fifoSetDatamsgHandler(u32 channel, FifoDatamsgHandlerFunc newhandler, void *userdata);
bool fifoCheckAddress(u32 channel);
bool fifoCheckValue32(u32 channel);
bool fifoCheckDatamsg(u32 channel);
int fifoCheckDatamsgLength(u32 channel);
void *fifoGetAddress(u32 channel);
u32 fifoGetValue32(u32 channel);
int fifoGetDatamsg(u32 channel, int buffersize, u8 *destbuffer);
void fifoMutexAcquire(u32 channel);
bool fifoMutexTryAcquire(u32 channel);
void fifoMutexRelease(u32 channel);
#ifdef __cplusplus
}
#endif
