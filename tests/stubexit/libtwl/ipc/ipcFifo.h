// host stand-in: the ARM7's view of a simulated IPC FIFO
#pragma once
#include "exit7_sim.h"
#define IPCFIFOCNT_RECV_FIFO_EMPTY (1 << 8)
#define IPCFIFOCNT_ERROR (1 << 14)
#define REG_IPCFIFOCNT (gSim7FifoCnt)
#define REG_IPCFIFORECV (gSim7FifoRecv)
