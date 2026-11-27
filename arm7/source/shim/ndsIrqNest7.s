@ Runs a libnds-style (DSWiFi) interrupt handler the way libnds's ARM7
@ dispatcher does: in system mode, on a roomy stack, with interrupts off.
@
@ libtwl calls handlers in IRQ mode, on the 1 KB IRQ stack. DSWiFi's DSi
@ handlers need far more than that (running them there crashed the ARM7
@ while joining), so they get a private 4 KB stack in system mode.
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
    @ IRQs stay OFF while the handler runs, exactly like libnds's ARM7
    @ dispatcher (it clears IME around handlers). Letting DSWiFi's timer and
    @ SDIO handlers interrupt each other mixed up SDIO transfers and stalled
    @ the ARM7 for seconds at a time.
    mov     lr, pc
    bx      r0
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
    @ in main RAM (IWRAM is kept for USB): 4 KB for DSWiFi handlers
    .section .ewram_bss, "aw", %nobits
    .align 3
sNestStack:
    .space 4096
sNestStackTop:
