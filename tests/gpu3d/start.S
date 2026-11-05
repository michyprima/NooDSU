@ Start of each CPU: stacks, caches off so captures can be read straight from memory, and the BSS cleared
.syntax unified
.arm
.section .start, "ax"
.global _start
_start:
    msr cpsr_c, #0xD2
    ldr sp, =STACKS
    msr cpsr_c, #0xD3
    ldr sp, =STACKS - 0x200
    msr cpsr_c, #0xDF
    ldr sp, =STACKS - 0x400
#ifdef ARM9
    mrc p15, 0, r0, c1, c0, 0
    bic r0, r0, #0x1000
    bic r0, r0, #0x5
    mcr p15, 0, r0, c1, c0, 0
    mov r0, #0
    mcr p15, 0, r0, c7, c5, 0
    mcr p15, 0, r0, c7, c6, 0
#endif
    ldr r0, =__bss_start
    ldr r1, =__bss_end
    mov r2, #0
1:  cmp r0, r1
    strlo r2, [r0], #4
    blo 1b
    mov r0, #0x04000000
    str r2, [r0, #0x208]
    str r2, [r0, #0x210]
    bl cpuMain
    b .
.ltorg
