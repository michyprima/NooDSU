// Settles how the DSi's NDMA channels really behave where GBATEK leaves it open or nothing else tests it. Each test runs on
// the ARM9, then the ARM7, and everything is saved for ndma.py, which compares it with NooDS U's results
#include <stdio.h>
#include <nds.h>
#include "screen.h"
#include "ndma.h"

static unsigned block[N_TOTAL] __attribute__((aligned(32)));
static unsigned buffer9[BUFFER_WORDS] __attribute__((aligned(32)));
static unsigned buffer7[BUFFER_WORDS] __attribute__((aligned(32)));
static int asked;

static void showStep(int test, int cpu) {
    // The third line says which test is running, so a hang says where it happened
    block[N_STEP] = test << 1 | cpu;
    textRow = 2;
    textColumn = 0;
    text(cpu ? "ARM7 TEST " : "ARM9 TEST ", YELLOW);
    number(test, YELLOW);
    text("     ", YELLOW);
    newLine();
}

static int ask(unsigned command) {
    // Send the ARM7 a command and wait about 2 seconds for it; the block is cached on this CPU, so it's written out first
    unsigned done = command << 2 | (++asked & 3);
    armDCacheFlush(block, sizeof(block));
    pxiSend(PxiChannel_User0, command << 20 | (asked & 3) << 18 | ((unsigned)block - 0x02000000) >> 2);
    for (int frames = 0; frames < 120; frames++) {
        while (VCOUNT != 192) {
            armDCacheInvalidate(&block[N_DONE7], 4);
            if (block[N_DONE7] == done) {
                armDCacheInvalidate(block, sizeof(block));
                return 1;
            }
        }
        while (VCOUNT == 192);
    }
    armDCacheInvalidate(block, sizeof(block));
    return 0;
}

static unsigned runAll(void) {
    // SCFG_EXT's bit 16 is what turns NDMA on, and it reads 0 outside DSi mode
    block[N_SCFG9] = *(volatile unsigned*)0x04004008;
    if (!(block[N_SCFG9] & (1 << 16))) return STATUS_NOT_DSI;
    block[N_BUFFER7] = (unsigned)buffer7;
    armDCacheFlush(buffer7, sizeof(buffer7));
    if (!ask(0)) return STATUS_NO_ARM7;
    for (int test = 0; test < TEST_COUNT; test++) {
        showStep(test, 0);
        runTest(test, &block[N_RESULTS + test * RESULT_WORDS], buffer9);
    }
    for (int test = 0; test < TEST_COUNT; test++) {
        showStep(test, 1);
        armDCacheFlush(buffer7, sizeof(buffer7));
        if (!ask(test + 1)) return STATUS_NO_ARM7;
    }
    return STATUS_DONE;
}

int main(void) {
    screenInit();
    block[N_MAGIC0] = NDMA_MAGIC0;
    block[N_MAGIC1] = NDMA_MAGIC1;
    pxiWaitRemote(PxiChannel_User0);
    textRow = textColumn = 0;
    text("NOODS U NDMA TEST", WHITE);
    newLine();
    text("RUNNING", YELLOW);
    newLine();

    unsigned status = runAll();
    block[N_STATUS] = status;
    textRow = 1;
    textColumn = 0;
    static const char *const statuses[] = { "", "DONE", "ARM7 DIDN'T ANSWER", "NOT IN DSI MODE" };
    text(statuses[status], status == STATUS_DONE ? GREEN : RED);
    newLine();
    textRow = 3;
    text("SCFG9 ", WHITE);
    hex(block[N_SCFG9], WHITE);
    text(" SCFG7 ", WHITE);
    hex(block[N_SCFG7], WHITE);
    newLine();

    // Save the whole block, which tests/hardware/ndma.py explains
    text("SAVING", YELLOW);
    newLine();
    const char *where = "";
    FILE *file = openSave("noodsu-ndmatest.bin", &where);
    int good = file && fwrite(block, 4, N_TOTAL, file) == N_TOTAL;
    if (file && fclose(file) != 0) good = 0;
    textRow--;
    textColumn = 0;
    text(!file ? "NOT SAVED" : !good ? "SAVE FAILED" : "SAVED ", good ? GREEN : RED);
    if (good) text(where, GREEN);
    newLine();
    while (1) threadWaitForVBlank();
}
