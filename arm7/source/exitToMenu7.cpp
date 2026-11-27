#include "common.h"
#include <libtwl/ipc/ipcFifo.h>
#include <libtwl/ipc/ipcSync.h>
#include "exitToMenu7.h"

#define EXIT_REG_IME  (*(vu32*)0x04000208)
#define EXIT_REG_IE   (*(vu32*)0x04000210)
#define EXIT_REG_IF   (*(vu32*)0x04000214)
#define EXIT_REG_IE2  (*(vu32*)0x04000218)
#define EXIT_REG_IF2  (*(vu32*)0x0400021C)

#ifndef EXIT7_HOST_TEST
#define EXIT7_ARM7_BOOT_ADDRESS (*(vu32*)0x02FFFE34) // set by the ARM9 before it asks
#endif

/// libnds' "reset the other CPU" FIFO message (see libnds reset.c / fifosystem.c)
static constexpr u32 LIBNDS_RESET_MESSAGE = 0x0c04000c;

#ifndef EXIT7_HOST_TEST
void exit7_quiesce()
{
    EXIT_REG_IME = 0;
    EXIT_REG_IE = 0;
    EXIT_REG_IF = 0xFFFFFFFF;
    if (isDSiMode())
    {
        EXIT_REG_IE2 = 0;
        EXIT_REG_IF2 = 0xFFFFFFFF;
    }
    for (u32 i = 0; i < 4; i++)
    {
        *(vu32*)(0x040000B8 + i * 12) = 0;  // DMAxCNT
        *(vu16*)(0x04000102 + i * 4) = 0;   // TMxCNT_H
    }
    for (u32 i = 0; i < 16; i++)
        *(vu32*)(0x04000400 + i * 16) = 0;  // sound channel control
    *(vu16*)0x04000500 = 0;                 // SOUNDCNT: master off
}

void exit7_jump(u32 entry)
{
    reinterpret_cast<void (*)()>(entry)();
    while (true)
        ;
}
#endif

void exit7_bootstubHandoff()
{
    REG_IPCFIFOCNT |= IPCFIFOCNT_ERROR; // acknowledge any old FIFO error
    for (;;)
    {
        while (REG_IPCFIFOCNT & IPCFIFOCNT_RECV_FIFO_EMPTY)
            ;
        if (REG_IPCFIFORECV == LIBNDS_RESET_MESSAGE)
            break;  // anything else still queued from before is dropped
    }
    // same handshake as libnds' ARM7 fifo system
    REG_IPCSYNC = 0x100;                    // tell the ARM9: 1
    while ((REG_IPCSYNC & 0x0F) != 1)       // wait for the ARM9's 1
        ;
    REG_IPCSYNC = 0;                        // 0: the ARM9 goes on
    exit7_jump(EXIT7_ARM7_BOOT_ADDRESS);
}
