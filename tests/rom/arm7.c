// The ARM7 sleeps until the ARM9 asks it to run something
#include "rom.h"
volatile unsigned *hashOut = HASHES7, *cyclesOut = CYCLES7;
volatile unsigned short *setsOut = SETS7;
volatile unsigned char *surveyOut = SURVEY7;

void onProgress(int cpu, int index) { PROGRESS7 = index + 1; }

static void sleep(void) {
    __asm__ volatile("swi #0x060000" : : : "r0", "r1", "r2", "r3", "lr");
    IF = IF;
}

void cpuMain(void) {
    IPCSYNC = 1 << 14;
    IF = IF;
    IE = BIT16;
    while (COMMAND != 1) sleep();
    IE = 0;
    IF = IF;
    runAll();
    FINISHED = 1;
    IPCSYNC = (1 << 14) | (1 << 13);

    // Then run single cases again when asked, for their details
    IE = BIT16;
    while (1) {
        if (COMMAND == 2) {
            caseDetail(DETAIL_CASE, DETAIL_SET, DETAIL7);
            COMMAND = 0;
        }
        sleep();
    }
}
