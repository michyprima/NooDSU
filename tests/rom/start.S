@ Start of each CPU, and what runs the generated cases and returns from them
.syntax unified
.arm
.section .start, "ax"
.global _start
_start:
    @ Every mode gets the same known registers, since cases look at them
    mov r1, #0x1000
    mov r2, #0
    adr r3, modes
1:  ldrb r0, [r3], #1
    cmp r0, #0
    beq 2f
    msr cpsr_c, r0
    ldr sp, =STACKS
    add sp, sp, r2
    mov lr, r0
    msr spsr_fsxc, r0
    add r2, r2, #0x200
    b 1b
2:
#ifdef ARM9
    @ Caches and protection off, so everything's in memory for the other CPU, keeping the BIOS's vectors
    mrc p15, 0, r0, c1, c0, 0
    bic r0, r0, #0x1000
    bic r0, r0, #0x5
    bic r0, r0, #0x4000 @ What the flashcart left this at shouldn't matter
    mcr p15, 0, r0, c1, c0, 0
    mov r0, #0
    mcr p15, 0, r0, c7, c5, 0
    mcr p15, 0, r0, c7, c6, 0
    ldr r0, =0x027E000A @ DTCM where the BIOS looks for the interrupt handler
    mcr p15, 0, r0, c9, c1, 0
    mov r0, #0x20 @ 32 MB of ITCM addresses, for the cases' data
    mcr p15, 0, r0, c9, c1, 1
    mrc p15, 0, r0, c1, c0, 0
    orr r0, r0, #0x50000
    mcr p15, 0, r0, c1, c0, 0
#endif
    ldr r0, =__bss_start
    ldr r1, =__bss_end
    mov r2, #0
1:  cmp r0, r1
    strlo r2, [r0], #4
    blo 1b
    mov r0, #0x04000000
    mov r1, #0
    str r1, [r0, #0x208]
    str r1, [r0, #0x210]
    bl cpuMain
    b .
modes:
    .byte 0xD1, 0xD7, 0xDB, 0xD3, 0xD2, 0xDF, 0
.align 2
.ltorg

.text
.global run_case
.type run_case, %function
run_case: @ r0 = case, r1 = 13 registers, r2 = results, r3 = flags on top of the mode
    push {r4-r11, lr}
    ldr r4, =saved_sp
    str sp, [r4]
    and r5, r3, #0xFF
    orr r5, r5, #0xC0
    msr cpsr_c, r5
    str sp, [r4, #4]
    mov sp, r2
    mov r12, r0
    ldr lr, [r1, #48]
    msr cpsr_f, r3
    ldmia r1, {r0-r11}
    bx r12

.global case_exit
case_exit:
    stmia sp, {r0-r12}
    str lr, [sp, #52]
    mrs r0, cpsr
    str r0, [sp, #56]
    and r1, r0, #0x1F
    cmp r1, #0x1F
    cmpne r1, #0x10
    mrsne r0, spsr @ What's read without one depends on what ran before it on a real DS
    moveq r0, #0
    str r0, [sp, #60]
    ldr r4, =saved_sp
    ldr sp, [r4, #4]
    msr cpsr_c, #0xDF
    ldr sp, [r4]
    pop {r4-r11, lr}
    bx lr
.ltorg

.bss
.align 2
saved_sp: .word 0, 0

.text
.global setInterrupts
.type setInterrupts, %function
setInterrupts: @ r0 = whether interrupts can happen
    mrs r1, cpsr
    bic r1, r1, #0x80
    cmp r0, #0
    orreq r1, r1, #0x80
    msr cpsr_c, r1
    bx lr

.text
.global run_mul
.type run_mul, %function
run_mul: @ r0 = which, r1 = Rm, r2 = Rs, r3 = low accumulator, then high accumulator and flags on the stack; returns the CPSR
    push {r4-r6, lr}
    ldr r5, [sp, #16]
    ldr r6, [sp, #20]
    adr r4, 1f
    ldr pc, [r4, r0, lsl #2]
1:  .word 10f, 11f, 12f, 13f, 14f, 15f, 16f
10: msr cpsr_f, r6
    muls r4, r1, r2
    b 2f
11: msr cpsr_f, r6
    mlas r4, r1, r2, r3
    b 2f
12: msr cpsr_f, r6
    umulls r4, r5, r1, r2
    b 2f
13: msr cpsr_f, r6
    umlals r3, r5, r1, r2
    b 2f
14: msr cpsr_f, r6
    smulls r4, r5, r1, r2
    b 2f
15: msr cpsr_f, r6
    smlals r3, r5, r1, r2
    b 2f
16: mov r0, r2 @ THUMB's MUL Rd,Rs is Rd = Rs * Rd, so Rd is the multiplier
    adr r4, 3f + 1
    msr cpsr_f, r6
    bx r4
.thumb
.p2align 1
3:  muls r0, r1
    .p2align 2
    bx pc
    nop
.arm
2:  mrs r0, cpsr
    mov r1, #0
    msr cpsr_f, r1
    pop {r4-r6, lr}
    bx lr
