// Settles how fast each CPU really runs, which decides races between them: how long the ARM9 takes over code and data
// that aren't in its caches, against the same in them, and how long the ARM7 takes over the same loop in its WRAM, the
// DSi's new WRAM and main RAM. Everything is timed with a timer, with interrupts off
#include <stdio.h>
#include <nds.h>
#include "screen.h"
#include "speed.h"

unsigned speed_time9(void (*function)(unsigned), unsigned argument, unsigned mode);
void speed_empty(unsigned), speed_loop(unsigned), speed_bios(unsigned), speed_read(unsigned), speed_write(unsigned);
void speed_hop(unsigned), speed_seq(unsigned);

static unsigned block[S_TOTAL] __attribute__((aligned(32)));
static unsigned char lines[DLINES * 32] __attribute__((aligned(32)));

#define MODE_EMPTY 1 // Empty the code cache first
#define MODE_OFF 2 // Run with the code cache off
#define CP15(crn, crm, op2) ({ unsigned value; __asm__ volatile("mrc p15, 0, %0, " #crn ", " #crm ", " #op2 : "=r"(value)); value; })

static void showStep(int step) {
    // Where the ARM9 is, on the third line so a hang says where it happened
    block[S_STEP] = step;
    textRow = 2;
    textColumn = 0;
    text("STEP ", YELLOW);
    number(step, YELLOW);
    newLine();
}

static unsigned timed(void (*function)(unsigned), unsigned argument, unsigned mode) {
    ArmIrqState psr = armIrqLockByPsr();
    unsigned ime = REG_IME;
    REG_IME = 0;
    unsigned ticks = speed_time9(function, argument, mode);
    REG_IME = ime;
    armIrqUnlockByPsr(psr);
    return ticks;
}

static void measure(int which, void (*function)(unsigned), unsigned argument, unsigned mode, int warm, int flush) {
    // Each time starts the same way: run once first to fill the caches, or write the data lines out and forget them
    showStep(which + 1);
    for (int i = 0; i < REPEATS; i++) {
        if (warm) function(argument);
        if (flush) armDCacheFlush(lines, sizeof(lines));
        block[S_RESULTS9 + which * REPEATS + i] = timed(function, argument, mode);
    }
}

static int askArm7(void) {
    // Send the ARM7 its command and wait up to 5 seconds, asleep between looks so this CPU leaves main RAM alone while
    // the ARM7 is timed; the block is cached on this CPU, so it's written out first, then forgotten once the ARM7 wrote it
    unsigned done = COMMAND_RUN << 4 | 1;
    armDCacheFlush(block, sizeof(block));
    pxiSend(PxiChannel_User0, COMMAND_RUN << 22 | 1 << 18 | ((unsigned)block - 0x02000000) >> 2);
    for (int frames = 0; frames < 300; frames++) {
        threadWaitForVBlank();
        armDCacheInvalidate(block, sizeof(block));
        if (block[S_DONE7] == done) return 1;
    }
    return 0;
}

static unsigned best(int at) {
    unsigned least = block[at];
    for (int i = 1; i < REPEATS; i++) if (block[at + i] < least) least = block[at + i];
    return least;
}

static void show(const char *name, int at) {
    text(name, WHITE);
    if (block[at] == NOT_RUN) text("-", WHITE);
    else number(best(at), GREEN);
}

int main(void) {
    screenInit();
    block[S_MAGIC0] = SPED_MAGIC0;
    block[S_MAGIC1] = SPED_MAGIC1;
    pxiWaitRemote(PxiChannel_User0);
    textRow = textColumn = 0;
    text("NOODS U SPEED TEST", WHITE);
    newLine();
    text("RUNNING", YELLOW);
    newLine();

    block[S_DSI] = isDSiMode();
    block[S_SCFG9] = *(volatile unsigned short*)0x04004004;
    block[S_SCFG9 + 1] = *(volatile unsigned*)0x04004008;
    unsigned *cp15 = &block[S_CP15];
    cp15[0] = CP15(c1, c0, 0);
    cp15[1] = CP15(c2, c0, 0);
    cp15[2] = CP15(c2, c0, 1);
    cp15[3] = CP15(c3, c0, 0);
    cp15[4] = CP15(c6, c0, 0);
    cp15[5] = CP15(c6, c1, 0);
    cp15[6] = CP15(c6, c2, 0);
    cp15[7] = CP15(c6, c3, 0);
    cp15[8] = CP15(c6, c4, 0);
    cp15[9] = CP15(c6, c5, 0);
    cp15[10] = CP15(c6, c6, 0);
    cp15[11] = CP15(c6, c7, 0);
    block[S_ADDR9] = (unsigned)speed_hop;
    block[S_ADDR9 + 1] = (unsigned)lines;
    block[S_LINES] = LINES;
    block[S_LINES + 1] = DLINES;

    measure(M9_EMPTY, speed_empty, 0, 0, 1, 0);
    measure(M9_LOOP, speed_loop, LOOPS, 0, 1, 0);
    measure(M9_BIOS, speed_bios, LOOPS, 0, 1, 0);
    measure(M9_HOP_HOT, speed_hop, 0, 0, 1, 0);
    measure(M9_HOP_COLD, speed_hop, 0, MODE_EMPTY, 0, 0);
    measure(M9_HOP_OFF, speed_hop, 0, MODE_OFF, 0, 0);
    measure(M9_SEQ_HOT, speed_seq, 0, 0, 1, 0);
    measure(M9_SEQ_COLD, speed_seq, 0, MODE_EMPTY, 0, 0);
    measure(M9_SEQ_OFF, speed_seq, 0, MODE_OFF, 0, 0);
    measure(M9_READ_HOT, speed_read, (unsigned)lines, 0, 1, 0);
    measure(M9_READ_COLD, speed_read, (unsigned)lines, 0, 1, 1);
    measure(M9_WRITE_HOT, speed_write, (unsigned)lines, 0, 1, 0);
    measure(M9_WRITE_COLD, speed_write, (unsigned)lines, 0, 1, 1);

    showStep(100);
    unsigned status = askArm7() ? STATUS_DONE : STATUS_NO_ARM7;
    block[S_STATUS] = status;

    // The least ticks of each measurement; speed.py explains the saved block
    textRow = 1;
    textColumn = 0;
    text(status == STATUS_DONE ? "DONE" : "ARM7 DIDN'T ANSWER", status == STATUS_DONE ? GREEN : RED);
    newLine();
    textRow = 3;
    text(block[S_DSI] ? "DSI MODE, SCFG CLK " : "DS MODE, SCFG CLK ", WHITE);
    hex(block[S_SCFG9], WHITE);
    text(" CP15 ", WHITE);
    hex(block[S_CP15], WHITE);
    newLine();
    text("ARM9 TICKS, LEAST OF ", WHITE);
    number(REPEATS, WHITE);
    newLine();
    show("EMPTY ", S_RESULTS9 + M9_EMPTY * REPEATS);
    show(" LOOP ", S_RESULTS9 + M9_LOOP * REPEATS);
    show(" BIOS ", S_RESULTS9 + M9_BIOS * REPEATS);
    newLine();
    static const char *const rows9[] = { "HOP   ", "SEQ   ", "READ  ", "WRITE " };
    static const int first9[] = { M9_HOP_HOT, M9_SEQ_HOT, M9_READ_HOT, M9_WRITE_HOT };
    for (int i = 0; i < 4; i++) {
        text(rows9[i], WHITE);
        show("HOT ", S_RESULTS9 + first9[i] * REPEATS);
        show(" COLD ", S_RESULTS9 + (first9[i] + 1) * REPEATS);
        if (i < 2) show(" OFF ", S_RESULTS9 + (first9[i] + 2) * REPEATS);
        newLine();
    }
    if (status == STATUS_DONE) {
        text("ARM7 TICKS: EMPTY LOOP LOAD STORE", WHITE);
        newLine();
        static const char *const rows7[] = { "WRAM", "NEW ", "MAIN" };
        for (int place = 0; place < PLACES; place++) {
            text(rows7[place], WHITE);
            for (int kind = 0; kind < KINDS; kind++) show(" ", S_RESULTS7 + (place * KINDS + kind) * REPEATS);
            newLine();
        }
        show("BIOS ", S_RESULTS7 + M7_BIOS * REPEATS);
        newLine();
    }

    // Save the whole block, which tests/hardware/speed.py explains
    const char *where = "";
    FILE *file = openSave("noodsu-speedtest.bin", &where);
    int good = file && fwrite(block, 4, S_TOTAL, file) == S_TOTAL;
    if (file && fclose(file) != 0) good = 0;
    text(!file ? "NOT SAVED" : !good ? "SAVE FAILED" : "SAVED ", good ? GREEN : RED);
    if (good) text(where, GREEN);
    newLine();
    while (1) threadWaitForVBlank();
}
