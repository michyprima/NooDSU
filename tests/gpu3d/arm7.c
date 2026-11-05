// The ARM7 has nothing to do here
void cpuMain(void) {
    while (1) __asm__ volatile("swi #0x060000" : : : "r0", "r1", "r2", "r3", "lr");
}
