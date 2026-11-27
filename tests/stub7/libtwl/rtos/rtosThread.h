#pragma once
typedef struct { int x; } rtos_thread_t;
static inline void rtos_createThread(rtos_thread_t*, int, void (*)(void*), void*, u32*, u32) {}
static inline void rtos_wakeupThread(rtos_thread_t*) {}
