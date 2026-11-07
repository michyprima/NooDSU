// Settles what the top byte of MBK9 does to shared WRAM, which the DSi menu needs to launch titles: it sets WRAMCNT to 3
// with 0xFC there, but keeps a heap at 0x3004000 on the ARM9 while its ARM7 reads a table from the start of shared WRAM
#include <stdio.h>
#include <nds.h>
#include "screen.h"
#include "wram.h"

static unsigned block[W_TOTAL] __attribute__((aligned(32)));
static unsigned backup[0x8000 / 4] __attribute__((aligned(32)));

// MBK9's top byte for each phase, or -1 to leave it as the system set it, and WRAMCNT when it's written
static const int phases[PHASES][2] = { { -1, 0 }, { 0x00, 0 }, { 0xFC, 0 }, { 0xFC, 3 }, { 0x03, 3 }, { 0x41, 0 } };

#define WRAMCNT (*(volatile unsigned char*)0x04000247)
#define DMA3 ((volatile unsigned*)0x040000D4)

static void showStep(int step) {
    // Where the ARM9 is and the last command the ARM7 finished, on the third line so a hang says where it happened
    block[W_STEP] = step;
    textRow = 2;
    textColumn = 0;
    text("STEP ", YELLOW);
    number(step, YELLOW);
    text(" ARM7 DID ", YELLOW);
    hex(block[W_DONE7], YELLOW);
    newLine();
}

static int asked;

static int ask(unsigned command) {
    // Send the ARM7 a command and wait about 2 seconds for it, as the instruction test does; the block is cached on
    // this CPU, so it's written out first, then forgotten while the ARM7 writes it
    unsigned done = command << 4 | (++asked & 0xF);
    showStep(block[W_STEP] + 1);
    armDCacheFlush(block, sizeof(block));
    pxiSend(PxiChannel_User0, command << 22 | (asked & 0xF) << 18 | ((unsigned)block - 0x02000000) >> 2);
    for (int frames = 0; frames < 120; frames++) {
        while (VCOUNT != 192) {
            armDCacheInvalidate(&block[W_DONE7], 4);
            if (block[W_DONE7] == done) {
                armDCacheInvalidate(block, sizeof(block));
                return 1;
            }
        }
        while (VCOUNT == 192);
    }
    armDCacheInvalidate(block, sizeof(block));
    return 0;
}

// The ARM9 reaches shared WRAM by DMA, which the protection unit doesn't check, since libnds gives it no region there
static unsigned dmaWord[8] __attribute__((aligned(32)));

static void dma(volatile void *to, volatile void *from) {
    ArmIrqState psr = armIrqLockByPsr();
    armDCacheFlush(dmaWord, sizeof(dmaWord));
    DMA3[0] = (unsigned)from;
    DMA3[1] = (unsigned)to;
    DMA3[2] = 0x84000001; // One word, now
    while (DMA3[2] & 0x80000000);
    armIrqUnlockByPsr(psr);
}

static unsigned readWram(unsigned address) {
    dma(dmaWord, (void*)address);
    return *(volatile unsigned*)dmaWord;
}

static void writeWram(unsigned address, unsigned value) {
    *(volatile unsigned*)dmaWord = value;
    dma((void*)address, dmaWord);
}

static void look9(unsigned *out, unsigned mark) {
    for (int n = 0; n < 8; n++) {
        unsigned at = LOOK_ADDRESS + n * 0x1000;
        out[n] = readWram(at);
        writeWram(at, mark | n);
        out[8 + n] = readWram(at);
    }
}

static int covers(unsigned start, unsigned end, unsigned from, unsigned to) { return start < to && end > from; }

static int nwramOver(const unsigned *mbk, unsigned from, unsigned to) {
    // Whether a CPU's MBK6-8 map new WRAM anywhere in a range
    return covers(0x03000000 + ((mbk[0] >> 4) & 0xFF) * 0x10000, 0x03000000 + ((mbk[0] >> 20) & 0x1FF) * 0x10000, from, to) ||
        covers(0x03000000 + ((mbk[1] >> 3) & 0x1FF) * 0x8000, 0x03000000 + ((mbk[1] >> 19) & 0x3FF) * 0x8000, from, to) ||
        covers(0x03000000 + ((mbk[2] >> 3) & 0x1FF) * 0x8000, 0x03000000 + ((mbk[2] >> 19) & 0x3FF) * 0x8000, from, to);
}

static unsigned runTest(void) {
    unsigned *init9 = &block[W_INIT9], *init7 = &block[W_INIT7];
    showStep(1);
    init9[0] = WRAMCNT;
    init9[1] = *(volatile unsigned*)0x04004008;
    for (int i = 0; i < 3; i++) init9[2 + i] = ((volatile unsigned*)0x04004054)[i];
    if (nwramOver(&init9[2], SHARED_ADDRESS, SHARED_ADDRESS + 0x8000)) return STATUS_NWRAM;
    if (!ask(COMMAND_INIT)) return STATUS_NO_ARM7;
    if ((init7[2] & 3) != 3) return STATUS_NOT_ARM7S;
    if (nwramOver(&init7[3], SHARED_ADDRESS, SHARED_ADDRESS + 0x8000)) return STATUS_NWRAM;

    // The ARM7 copies shared WRAM to main RAM this CPU may have cached, so that's written out and forgotten first
    block[W_BACKUP] = (unsigned)backup;
    armDCacheFlush(backup, sizeof(backup));
    if (!ask(COMMAND_BACKUP)) return STATUS_NO_ARM7;
    armDCacheInvalidate(backup, sizeof(backup));
    for (int i = 0; i < 0x8000 / 4; i++) block[W_USED] += backup[i] != 0;
    unsigned base = init7[0] & 0x00FFFF0F;
    for (int i = 0; i < PHASES; i++) {
        unsigned *phase = &block[W_PHASES + i * PHASE_WORDS];
        int lock = phases[i][0], cnt = phases[i][1];
        unsigned mark = 0xB9000000 | i << 16;
        phase[P_LOCK] = lock;
        phase[P_CNT] = cnt;
        showStep(100 + i * 100);
        WRAMCNT = cnt;
        if (lock >= 0) {
            block[W_PARAM] = base | lock << 24;
            if (!ask(COMMAND_MBK9)) return STATUS_NO_ARM7;
            phase[P_MBK9] = block[W_MBK9_BACK];
        }
        showStep(block[W_STEP] + 10);
        look9(&phase[P_LOOK9_BEFORE], mark | 0x100);
        showStep(block[W_STEP] + 10);
        WRAMCNT = 3 - cnt;
        look9(&phase[P_LOOK9_AFTER], mark | 0x200);
        block[W_OUT7] = (unsigned)&phase[P_LOOK7];
        block[W_MARK7] = 0xC7000000 | i << 16 | 0x300;
        if (!ask(COMMAND_LOOK)) return STATUS_NO_ARM7;
        showStep(block[W_STEP] + 10);
        look9(&phase[P_LOOK9_LAST], mark | 0x400);
    }

    // Give shared WRAM back to the ARM7 as it was, with nothing locked, then MBK9 as the system set it
    showStep(1000);
    WRAMCNT = 3;
    block[W_PARAM] = base;
    if (!ask(COMMAND_MBK9) || !ask(COMMAND_RESTORE)) return STATUS_NO_ARM7;
    block[W_PARAM] = init7[0];
    if (!ask(COMMAND_MBK9)) return STATUS_NO_ARM7;
    return STATUS_DONE;
}

static char blockChar(unsigned value, unsigned back, unsigned mark, int i, int n) {
    // A digit when a block shows a marker the ARM9 wrote in this phase, which says which block it was written to; W
    // when it can be written but had none, and - when it can't be written
    if ((value & 0xFFFF0000) == (0xB9000000 | i << 16) && (value & 0xFF) < 8) return '0' + (value & 7);
    return back == (mark | n) ? 'W' : '-';
}

static void showPhase(int i) {
    unsigned *phase = &block[W_PHASES + i * PHASE_WORDS];
    unsigned mark = 0xB9000000 | i << 16;
    char row9[9] = {}, row7[9] = {};
    for (int n = 0; n < 8; n++) {
        row9[n] = blockChar(phase[P_LOOK9_AFTER + n], phase[P_LOOK9_AFTER + 8 + n], mark | 0x200, i, n);
        row7[n] = blockChar(phase[P_LOOK7 + n], phase[P_LOOK7 + 8 + n], 0xC7000000 | i << 16 | 0x300, i, n);
    }
    static const char digits[] = "0123456789ABCDEF";
    char lock[3] = { digits[(phase[P_LOCK] >> 4) & 0xF], digits[phase[P_LOCK] & 0xF], 0 };
    number(i, YELLOW);
    text(" ", WHITE);
    text((int)phase[P_LOCK] < 0 ? "--" : lock, WHITE);
    text(phase[P_CNT] ? " 3>0 9:" : " 0>3 9:", WHITE);
    text(row9, GREEN);
    text(" 7:", WHITE);
    text(row7, GREEN);
    newLine();
}

int main(void) {
    screenInit();
    block[W_MAGIC0] = WRAM_MAGIC0;
    block[W_MAGIC1] = WRAM_MAGIC1;
    pxiWaitRemote(PxiChannel_User0);
    textRow = textColumn = 0;
    text("NOODS U SHARED WRAM TEST", WHITE);
    newLine();
    text("RUNNING", YELLOW);
    newLine();

    unsigned status = runTest();
    if (status != STATUS_DONE && block[W_STEP] >= 100) {
        // Leave shared WRAM to the ARM7 as best it can, though the ARM7 didn't answer
        WRAMCNT = 3;
    }
    block[W_STATUS] = status;
    textRow = 1;
    textColumn = 0;
    static const char *const statuses[] = { "", "DONE", "ARM7 DIDN'T ANSWER", "WRAM NOT ALL ARM7'S", "NEW WRAM IN THE WAY" };
    text(statuses[status], status == STATUS_DONE ? GREEN : RED);
    newLine();
    textRow = 3;
    text("WRAMCNT ", WHITE);
    hex(block[W_INIT9], WHITE);
    text(" SCFG9 ", WHITE);
    hex(block[W_INIT9 + 1], WHITE);
    newLine();
    text("MBK9 ", WHITE);
    hex(block[W_INIT7], WHITE);
    text(" SCFG7 ", WHITE);
    hex(block[W_INIT7 + 1], WHITE);
    newLine();
    text("WORDS IN USE BEFORE ", WHITE);
    number(block[W_USED], WHITE);
    newLine();
    if (status == STATUS_DONE) {
        text("PHASE MBK9 WRAMCNT, THEN BLOCKS 0-7", WHITE);
        newLine();
        for (int i = 0; i < PHASES; i++) showPhase(i);
        text("MBK9 READ BACK IN PHASE 2 ", WHITE);
        hex(block[W_PHASES + 2 * PHASE_WORDS + P_MBK9], WHITE);
        newLine();
    }

    // Save the whole block, which tests/hardware/wram.py explains
    text("SAVING", YELLOW);
    newLine();
    const char *where = "";
    FILE *file = openSave("noodsu-wramtest.bin", &where);
    int good = file && fwrite(block, 4, W_TOTAL, file) == W_TOTAL;
    if (file && fclose(file) != 0) good = 0;
    textRow--;
    textColumn = 0;
    text(!file ? "NOT SAVED" : !good ? "SAVE FAILED" : "SAVED ", good ? GREEN : RED);
    if (good) text(where, GREEN);
    newLine();
    while (1) threadWaitForVBlank();
}
