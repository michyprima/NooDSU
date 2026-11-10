// Settles how the main memory display FIFO behaves, which GBATEK only outlines: what the display and display capture
// take from it when DMA fills it in different ways or nothing does, what CPU writes of each size do, and which lines
// DMA timing 4 and NDMA mode 9 are triggered on, whether the FIFO is used or not. Display capture's source B shows
// exactly what the FIFO gave each pixel, so most scenes capture it
#include <stdio.h>
#include <string.h>
#include <nds.h>
#include "screen.h"
#include "fifo.h"

#define IO(a) (*(volatile unsigned*)(a))
#define IO16(a) (*(volatile unsigned short*)(a))
#define IO8(a) (*(volatile unsigned char*)(a))
#define LINE IO16(0x04000006)
#define DISPCNT IO(0x04000000)
#define DISPCAPCNT IO(0x04000064)
#define FIFO 0x04000068
#define VCOUNT_REG 0x04000004 // DISPSTAT, then VCOUNT in the top half of the word
#define DMA_SAD(n) IO(0x040000B0 + (n) * 12)
#define DMA_DAD(n) IO(0x040000B4 + (n) * 12)
#define DMA_CNT(n) IO(0x040000B8 + (n) * 12)
#define NDMA(ch, off) IO(0x04004104 + (ch) * 0x1C + (off))

// DMACNT: enabled, display FIFO timing, and 32-bit or 16-bit transfers with repeat or not
#define FIFO_DMA(count, words, repeat) (1u << 31 | 4 << 27 | (words) << 26 | (repeat) << 25 | (count))
#define DMA_DST_FIXED (2 << 21)
#define DMA_SRC_FIXED (2 << 23)
#define FILL 1 // The channel that fills the FIFO
#define REC 2 // The channel that records VCOUNT

// NDMA: enabled, repeating forever, mode 9, and the address steps
#define NDMA_MODE9 (1u << 31 | 1u << 29 | 9u << 24)
#define NDMA_DST_FIXED (2 << 10)
#define NDMA_SRC_FIXED (2 << 13)

static unsigned block[R_TOTAL] __attribute__((aligned(32)));
static unsigned short bitmap[256 * 192] __attribute__((aligned(32)));
static volatile unsigned rec[REC_WORDS] __attribute__((aligned(32)));

static const struct { const char *name; int cap, dsi; } scenes[] = {
#define X(name, cap, dsi) { #name, cap, dsi },
    SCENES(X)
#undef X
};

static void waitLine(int line) { while (LINE == line); while (LINE != line); }

static void stopAll(void) {
    DMA_CNT(FILL) = 0;
    DMA_CNT(REC) = 0;
    if (block[R_SCFG9] & (1 << 16)) { NDMA(1, 0x18) = 0; NDMA(2, 0x18) = 0; }
}

static void fillDma(unsigned cnt) {
    DMA_SAD(FILL) = (unsigned)bitmap;
    DMA_DAD(FILL) = FIFO;
    DMA_CNT(FILL) = cnt | DMA_DST_FIXED;
}

static void recDma(void) {
    DMA_SAD(REC) = VCOUNT_REG;
    DMA_DAD(REC) = (unsigned)rec;
    DMA_CNT(REC) = FIFO_DMA(1, 1, 1) | DMA_SRC_FIXED;
}

static void ndma(int ch, unsigned src, unsigned dst, unsigned steps) {
    NDMA(ch, 0x18) = 0;
    NDMA(ch, 0x00) = src;
    NDMA(ch, 0x04) = dst;
    NDMA(ch, 0x08) = 0;
    NDMA(ch, 0x0C) = ch == 1 ? 4 : 1;
    NDMA(ch, 0x10) = 0;
    NDMA(ch, 0x18) = NDMA_MODE9 | steps;
}

static void cpuWrites(void) {
    // Pixels 0-3 as words, 4-7 as halfwords, 8-11 as bytes, then halfwords to the bottom half only (twice), the top
    // half, and a word, for 12-15 if only the top half moves on
    IO(FIFO) = 0x10021001; IO(FIFO) = 0x10041003;
    IO16(FIFO) = 0x2005; IO16(FIFO + 2) = 0x2006; IO16(FIFO) = 0x2007; IO16(FIFO + 2) = 0x2008;
    IO8(FIFO) = 0x09; IO8(FIFO + 1) = 0x30; IO8(FIFO + 2) = 0x0A; IO8(FIFO + 3) = 0x30;
    IO8(FIFO) = 0x0B; IO8(FIFO + 1) = 0x30; IO8(FIFO + 2) = 0x0C; IO8(FIFO + 3) = 0x30;
    IO16(FIFO) = 0x400D; IO16(FIFO) = 0x400E; IO16(FIFO + 2) = 0x4011; IO(FIFO) = 0x40134012;
}

static void summarize(unsigned *out) {
    // Go through the VCOUNTs recorded, a new frame starting whenever the line goes back
    armDCacheInvalidate((void*)rec, sizeof(rec));
    int n = 0;
    while (n < REC_WORDS && rec[n] != 0xFFFFFFFF) n++;
    unsigned low = NO_LINE, high = 0, minimum = 0xFFFF, maximum = 0, lines = 0, frames = 0, firstCount = 0;
    unsigned previous = NO_LINE, run = 0;
    for (int i = 0; i <= n; i++) {
        unsigned line = i < n ? rec[i] >> 16 : NO_LINE;
        if (i > 0 && line != previous) {
            // A run of transfers on one line ended
            if (lines == 0) firstCount = run;
            else { if (run < minimum) minimum = run; if (run > maximum) maximum = run; }
            lines++;
            run = 0;
        }
        if (i == n) break;
        if (i == 0 || line < previous) frames++;
        if (line < low) low = line;
        if (line > high) high = line;
        run++;
        previous = line;
    }
    out[W_TOTAL] = n;
    out[W_FIRST] = n ? rec[0] >> 16 : NO_LINE;
    out[W_LAST] = n ? rec[n - 1] >> 16 : NO_LINE;
    out[W_LOW] = low;
    out[W_HIGH] = high;
    out[W_FIRST_COUNT] = firstCount;
    out[W_MIN] = minimum == 0xFFFF ? 0 : minimum;
    out[W_MAX] = maximum;
    out[W_LINES] = lines;
    out[W_FRAMES] = frames;
}

static void run(int s) {
    unsigned *out = &block[R_RESULTS + s * RESULT_WORDS];
    block[R_STEP] = s;
    int ndmaScene = s >= S_NDMA;
    if (scenes[s].dsi && !(block[R_SCFG9] & (1 << 16))) return;
    out[W_RAN] = 1;
    for (int i = 0; i < REC_WORDS; i++) rec[i] = 0xFFFFFFFF;
    armDCacheFlush((void*)rec, sizeof(rec));

    // Everything happens with interrupts off, from the start of a V-blank, so the frame after it is the first
    ArmIrqState irqs = armIrqLockByPsr();
    waitLine(193);
    stopAll();
    int mode3 = s != S_CAPONLY && s != S_BLEND && s != S_LINES1 && s != S_MID1 && s != S_NDMAUNUSED;
    DISPCNT = mode3 ? 0x30000 : 0x10000;
    IO16(0x05000000) = 0x03E0; // Green backdrop
    if (s == S_HALF) cpuWrites(); // A known ring first, since this DMA may not move it on

    switch (s) {
        case S_REPEAT4: case S_CAPONLY: case S_BLEND: case S_LINES3: fillDma(FIFO_DMA(4, 1, 1)); break;
        case S_BIG: fillDma(FIFO_DMA(256 * 192 / 2, 1, 0)); break;
        case S_COUNT8: fillDma(FIFO_DMA(8, 1, 1)); break;
        case S_COUNT2: fillDma(FIFO_DMA(2, 1, 1)); break;
        case S_HALF: fillDma(FIFO_DMA(8, 0, 1)); break;
        case S_CPU: cpuWrites(); break;
        case S_NDMA: case S_NDMALINES: ndma(1, (unsigned)bitmap, FIFO, NDMA_DST_FIXED); break;
    }
    if (s == S_LINES3 || s == S_LINES1) recDma();
    if (s == S_NDMALINES || s == S_NDMAUNUSED) ndma(2, VCOUNT_REG, (unsigned)rec, NDMA_SRC_FIXED);

    // Capture source B from the FIFO, 256x192 into VRAM D, blended 8/16 with engine A for BLEND
    if (scenes[s].cap)
        DISPCAPCNT = 1u << 31 | (s == S_BLEND ? 2 : 1) << 29 | 1 << 25 | 3 << 20 | 3 << 16 | (s == S_BLEND ? 8 << 8 | 8 : 0);

    // The channel whose control register is recorded
    volatile unsigned *cnt = ndmaScene ? &NDMA((s == S_NDMA ? 1 : 2), 0x18) :
        &DMA_CNT((s == S_LINES1 || s == S_MID1) ? REC : FILL);

    if (s == S_MID3 || s == S_MID1) {
        waitLine(100);
        if (s == S_MID3) fillDma(FIFO_DMA(4, 1, 1));
        recDma();
    }
    waitLine(191);
    out[W_CNT191] = *cnt;
    waitLine(193);
    out[W_CNT193] = *cnt;
    out[W_CAPCNT] = DISPCAPCNT;
    waitLine(193);
    out[W_CNT_END] = *cnt;
    stopAll();
    DISPCAPCNT = 0;
    armIrqUnlockByPsr(irqs);

    summarize(out);
    // Each capturing scene has its own place, whether or not the ones before it ran
    int slot = 0;
    for (int i = 0; i < s; i++) slot += scenes[i].cap;
    if (scenes[s].cap)
        memcpy(&block[R_CAPTURES + slot * CAPTURE_WORDS], (void*)0x06860000, CAPTURE_WORDS * 4);
}

static void show(int s) {
    // The name, then whether it ran, and a sum of what it recorded
    unsigned *out = &block[R_RESULTS + s * RESULT_WORDS];
    textRow = 4 + s;
    textColumn = 0;
    text(scenes[s].name, WHITE);
    textColumn = 12;
    if (!out[W_RAN]) { text("SKIPPED", YELLOW); return; }
    unsigned sum = 0;
    for (int i = 0; i < RESULT_WORDS; i++) sum = sum * 31 + out[i];
    hex(sum, GREEN);
}

int main(void) {
    // The FIFO on the main engine, shown on the top screen, and text on the sub engine at the bottom
    IO(0x04000304) = 0x820F;
    screenInitSub();
    IO8(0x04000243) = 0x80; // VRAM D for captures
    block[R_MAGIC0] = FIFO_MAGIC0;
    block[R_MAGIC1] = FIFO_MAGIC1;
    block[R_SCFG9] = IO(0x04004008);
    block[R_READ32] = IO(FIFO);
    block[R_READ16] = IO16(FIFO);
    for (int y = 0; y < 192; y++)
        for (int x = 0; x < 256; x++)
            bitmap[y * 256 + x] = BITMAP_PIXEL(x, y);
    armDCacheFlush(bitmap, sizeof(bitmap));

    textRow = textColumn = 0;
    text("NOODS U DISPLAY FIFO TEST", WHITE);
    newLine();
    text("RUNNING", YELLOW);
    newLine();

    for (int s = 0; s < SCENE_COUNT; s++) {
        textRow = 2;
        textColumn = 0;
        text("SCENE ", YELLOW);
        number(s, YELLOW);
        text("  ", YELLOW);
        run(s);
        show(s);
    }
    DISPCNT = 0x10000;
    block[R_STATUS] = STATUS_DONE;

    // Save the whole block, which tests/hardware/fifo.py explains
    textRow = 1;
    textColumn = 0;
    text("DONE, SAVING", YELLOW);
    newLine();
    const char *where = "";
    FILE *file = openSave("noodsu-fifotest.bin", &where);
    int good = file && fwrite(block, 4, R_TOTAL, file) == R_TOTAL;
    if (file && fclose(file) != 0) good = 0;
    textRow = 1;
    textColumn = 0;
    text(!file ? "DONE, NOT SAVED" : !good ? "DONE, SAVE FAILED" : "DONE, SAVED ", good ? GREEN : RED);
    if (good) text(where, GREEN);
    newLine();
    while (1) threadWaitForVBlank();
}
