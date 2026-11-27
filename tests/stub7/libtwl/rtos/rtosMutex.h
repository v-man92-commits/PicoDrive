#pragma once
typedef struct { int locked; } rtos_mutex_t;
static inline void rtos_createMutex(rtos_mutex_t* m) { m->locked = 0; }
void rtos_lockMutex(rtos_mutex_t* m);
void rtos_unlockMutex(rtos_mutex_t* m);
