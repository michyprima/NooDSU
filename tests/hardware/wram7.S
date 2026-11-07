@ The ARM7's look at shared WRAM, from main RAM: with WRAMCNT at 0 the ARM7 sees its own WRAM there, which holds libnds's
@ interrupt stacks and DLDI, so nothing may run from or use it until each word is put back. The thread's stack is in the
@ DSi's new WRAM, which isn't seen there
#include "wram.h"

.arch armv4t
.syntax unified
.arm
.section .main, "ax" @ Main RAM

@ r0: the shared block. Interrupts must be off
.global wram_look7
wram_look7:
    push {r4-r6}
    ldr r1, [r0, #(W_OUT7 * 4)]
    ldr r2, [r0, #(W_MARK7 * 4)]
    ldr r3, =LOOK_ADDRESS
    mov r12, #0
look_loop:
    @ Read the block, write a marker and read it back, then put back what was there
    ldr r4, [r3]
    orr r5, r2, r12
    str r5, [r3]
    ldr r6, [r3]
    str r4, [r3]
    str r4, [r1, r12, lsl #2]
    add r5, r1, #32
    str r6, [r5, r12, lsl #2]
    add r3, r3, #0x1000
    add r12, r12, #1
    cmp r12, #8
    blt look_loop
    pop {r4-r6}
    bx lr

.ltorg
