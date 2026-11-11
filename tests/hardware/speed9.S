@ The ARM9's side of the speed test: what gets timed, in main RAM where the caches cover it, and the timing itself, in
@ ITCM so emptying or turning off the code cache doesn't slow it
#include "speed.h"

.arch armv5te
.syntax unified
.arm

.section .itcm, "ax"

@ r0: the function, r1: its argument, r2: bit 0 to empty the code cache first, bit 1 to run with it turned off
@ Returns timer 0's count, with timer 1's count of its overflows in the top half. Interrupts must be off
.global speed_time9
speed_time9:
    push {r4-r7, lr}
    mov r5, r0
    mov r6, r2
    mrc p15, 0, r7, c1, c0, 0
    tst r6, #2
    bicne r3, r7, #0x1000
    mcrne p15, 0, r3, c1, c0, 0
    mov r3, #0
    tst r6, #1
    mcrne p15, 0, r3, c7, c5, 0
    ldr r4, =0x04000100
    strh r3, [r4, #2]
    strh r3, [r4, #6]
    strh r3, [r4]
    strh r3, [r4, #4]
    mov r2, #0x84
    strh r2, [r4, #6]
    mov r2, #0x80
    mov r0, r1
    strh r2, [r4, #2]
    blx r5
    ldrh r0, [r4]
    ldrh r1, [r4, #4]
    mov r3, #0
    strh r3, [r4, #2]
    strh r3, [r4, #6]
    orr r0, r0, r1, lsl #16
    mcr p15, 0, r7, c1, c0, 0
    pop {r4-r7, pc}

.ltorg

.section .text.speed, "ax"

.balign 32
.global speed_empty
speed_empty:
    bx lr

.balign 32
.global speed_loop
speed_loop:
    subs r0, r0, #1
    bne speed_loop
    bx lr

.balign 32
.global speed_bios
speed_bios:
    swi 0x030000
    bx lr

@ r0: the first of DLINES lines of data
.balign 32
.global speed_read
speed_read:
    mov r2, #DLINES
1:
    ldr r1, [r0], #32
    subs r2, r2, #1
    bne 1b
    bx lr

.balign 32
.global speed_write
speed_write:
    mov r2, #DLINES
1:
    str r2, [r0], #32
    subs r2, r2, #1
    bne 1b
    bx lr

@ Two instructions in each line, the second a branch to the next line
.balign 32
.global speed_hop
speed_hop:
.rept LINES - 1
    add r0, r0, #1
    b 1f
    .balign 32
1:
.endr
    add r0, r0, #1
    bx lr

@ Every instruction of each line
.balign 32
.global speed_seq
speed_seq:
.rept LINES * 8 - 1
    add r0, r0, #1
.endr
    bx lr
