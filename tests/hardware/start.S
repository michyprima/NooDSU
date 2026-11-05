.syntax unified
.arm
.section .start, "ax"
.global _start
_start:
    msr cpsr_c, #0xD2 @ IRQ mode's stack, for the BIOS
    ldr sp, =IRQ_STACK
    msr cpsr_c, #0xD3
    ldr sp, =SVC_STACK
    msr cpsr_c, #0xDF
    ldr sp, =SYS_STACK
#ifdef ARM9
    @ Caches and protection off, so everything's in memory for the other CPU, keeping the BIOS's vectors
    mrc p15, 0, r0, c1, c0, 0
    bic r0, r0, #0x1000
    bic r0, r0, #0x5
    mcr p15, 0, r0, c1, c0, 0
    mov r0, #0
    mcr p15, 0, r0, c7, c5, 0
    mcr p15, 0, r0, c7, c6, 0
    ldr r0, =0x027E000A @ DTCM where the BIOS looks for the interrupt handler
    mcr p15, 0, r0, c9, c1, 0
    mrc p15, 0, r0, c1, c0, 0
    orr r0, r0, #0x10000
    mcr p15, 0, r0, c1, c0, 0
#endif
    ldr r0, =__bss_start
    ldr r1, =__bss_end
    mov r2, #0
1:  cmp r0, r1
    strlo r2, [r0], #4
    blo 1b
#ifdef ARM9
    ldr r0, =0x027E3FFC
#else
    ldr r0, =0x0380FFFC
#endif
    ldr r1, =irq_escape
    str r1, [r0]
#ifdef ARM9
    ldr r0, =0x027FFD9C
#else
    ldr r0, =0x0380FFDC
#endif
    ldr r1, =exception_caught
    str r1, [r0]
    mov r0, #0x04000000
    mov r1, #0
    str r1, [r0, #0x208]
    str r1, [r0, #0x210]
#ifdef ARM9
    bl arm9Main
#else
    bl arm7Main
#endif
    b .
