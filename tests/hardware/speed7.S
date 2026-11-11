@ The ARM7's side of the speed test: the same loops in each place its code can be, and the timing
#include "speed.h"

.arch armv4t
.syntax unified
.arm

.section .text.speed, "ax"

@ r0: the function, r1: its argument
@ Returns timer 0's count, with timer 1's count of its overflows in the top half. Interrupts must be off
.global speed_time7
speed_time7:
    push {r4-r5, lr}
    mov r5, r0
    mov r3, #0
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
    mov lr, pc
    bx r5
    ldrh r0, [r4]
    ldrh r1, [r4, #4]
    mov r3, #0
    strh r3, [r4, #2]
    strh r3, [r4, #6]
    orr r0, r0, r1, lsl #16
    pop {r4-r5, lr}
    bx lr

.global speed_bios7
speed_bios7:
    swi 0x030000
    bx lr

.ltorg

@ The four kinds in one place, r0: the turns; the word LOAD and STORE use is next to them
.macro PLACE name, section
.section \section, "ax"
.balign 4
.global speed_empty_\name
speed_empty_\name:
    bx lr
.global speed_loop_\name
speed_loop_\name:
    subs r0, r0, #1
    bne speed_loop_\name
    bx lr
.global speed_load_\name
speed_load_\name:
    adr r1, speed_word_\name
1:
    ldr r3, [r1]
    subs r0, r0, #1
    bne 1b
    bx lr
.global speed_store_\name
speed_store_\name:
    adr r1, speed_word_\name
1:
    str r0, [r1]
    subs r0, r0, #1
    bne 1b
    bx lr
speed_word_\name:
    .word 0
.endm

PLACE wram, .text.speed_wram
PLACE twl, .twl.speed
PLACE main, .main.speed
