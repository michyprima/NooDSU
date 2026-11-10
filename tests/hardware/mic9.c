// Settles how the DSi's microphone FIFO really behaves where GBATEK leaves it open: the ARM9 asks the ARM7, which has the
// microphone, for each test, and saves everything for mic.py, which compares it with NooDS U's results
#include <stdio.h>
#include <nds.h>
#include "screen.h"
#include "mic.h"

static unsigned block[M_TOTAL] __attribute__((aligned(32)));
static unsigned buffer7[BUFFER_WORDS] __attribute__((aligned(32)));
static int asked;

static void showStep(int test) {
    // The third line says which test is running, so a hang says where it happened
    block[M_STEP] = test;
    textRow = 2;
    textColumn = 0;
    text("TEST ", YELLOW);
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
            armDCacheInvalidate(&block[M_DONE7], 4);
            if (block[M_DONE7] == done) {
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
    // The ARM7's SCFG_EXT bit 20 gives it the microphone, and SCFG_EXT reads 0 outside DSi mode
    block[M_BUFFER7] = (unsigned)buffer7;
    if (!ask(0)) return STATUS_NO_ARM7;
    if (!(block[M_SCFG7] & (1 << 20))) return STATUS_NOT_DSI;
    for (int test = 0; test < TEST_COUNT; test++) {
        showStep(test);
        if (!ask(test + 1)) return STATUS_NO_ARM7;
    }
    return STATUS_DONE;
}

int main(void) {
    screenInit();
    block[M_MAGIC0] = MIC_MAGIC0;
    block[M_MAGIC1] = MIC_MAGIC1;
    pxiWaitRemote(PxiChannel_User0);
    textRow = textColumn = 0;
    text("NOODS U MIC TEST", WHITE);
    newLine();
    text("RUNNING", YELLOW);
    newLine();

    unsigned status = runAll();
    block[M_STATUS] = status;
    textRow = 1;
    textColumn = 0;
    static const char *const statuses[] = { "", "DONE", "ARM7 DIDN'T ANSWER", "NOT IN DSI MODE" };
    text(statuses[status], status == STATUS_DONE ? GREEN : RED);
    newLine();
    textRow = 3;
    text("SCFG7 ", WHITE);
    hex(block[M_SCFG7], WHITE);
    newLine();

    // Save the whole block, which tests/hardware/mic.py explains
    text("SAVING", YELLOW);
    newLine();
    const char *where = "";
    FILE *file = openSave("noodsu-mictest.bin", &where);
    int good = file && fwrite(block, 4, M_TOTAL, file) == M_TOTAL;
    if (file && fclose(file) != 0) good = 0;
    textRow--;
    textColumn = 0;
    text(!file ? "NOT SAVED" : !good ? "SAVE FAILED" : "SAVED ", good ? GREEN : RED);
    if (good) text(where, GREEN);
    newLine();
    while (1) threadWaitForVBlank();
}
