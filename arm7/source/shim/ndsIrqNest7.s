@ Runs a libnds-style (DSWiFi) interrupt handler with interrupts enabled,
@ like libnds's own dispatcher does.
@
@ libtwl calls handlers in IRQ mode with interrupts off and never nests.
@ DSWiFi expects libnds behaviour: its handlers can wait for something that
@ another interrupt delivers (e.g. an SDIO transfer finishing). Under libtwl
@ that wait never ends and the ARM7 locks up. So: mask this handler's own
@ bit, switch to system mode on a private stack, enable interrupts, call it,
@ then undo all of that.
@
@ void ndsIrqShim7_callNested(VoidFn f, u32 bit, vu32* regIE)

    .arm
    .text
    .global ndsIrqShim7_callNested
    .type ndsIrqShim7_callNested, %function
ndsIrqShim7_callNested:
    push    {r4-r7, lr}             @ IRQ stack
    mrs     r4, spsr
    mov     r5, r1                  @ bit
    mov     r6, r2                  @ &REG_IE or &REG_IE2
    ldr     r7, [r6]                @ old IE
    bic     r3, r7, r5
    str     r3, [r6]                @ no re-entry for this source

    msr     cpsr_c, #0x9F           @ system mode, IRQs still off
    ldr     r2, =sNestDepth
    ldr     r3, [r2]
    add     r3, r3, #1
    str     r3, [r2]
    mov     r12, sp                 @ interrupted thread's stack
    cmp     r3, #1
    ldreq   sp, =sNestStackTop      @ outermost: private stack
    push    {r12, lr}               @ thread sp and its lr (8 bytes)
    msr     cpsr_c, #0x1F           @ IRQs on
    mov     lr, pc
    bx      r0
    msr     cpsr_c, #0x9F           @ IRQs off
    pop     {r12, lr}
    mov     sp, r12
    ldr     r2, =sNestDepth
    ldr     r3, [r2]
    sub     r3, r3, #1
    str     r3, [r2]
    msr     cpsr_c, #0x92           @ back to IRQ mode, IRQs off

    ldr     r3, [r6]                @ the handler may have changed IE
    and     r2, r7, r5
    orr     r3, r3, r2              @ re-enable our bit if it was on
    str     r3, [r6]
    msr     spsr_cxsf, r4
    pop     {r4-r7, lr}
    bx      lr

    .bss
    .align 3
sNestDepth:
    .space 4
    .align 3
sNestStack:
    .space 2048
sNestStackTop:
