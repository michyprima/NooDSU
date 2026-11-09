// The NDMA tests, run by each CPU with interrupts off so nothing else moves: each sets up channel 2, lets some V-blanks
// pass and records up to 8 words. Transfers that read VCOUNT show which scanline each one happened on
#include <string.h>
#include <nds.h>
#include "ndma.h"

#define IO(a) (*(volatile unsigned*)(a))
#define IO16(a) (*(volatile unsigned short*)(a))
#define LINE IO16(0x04000006)
#define IF IO(0x04000214)
#define GCNT IO(0x04004100)
#define REG(ch, off) IO(0x04004104 + (ch) * 0x1C + (off))
#define SAD 0x00
#define DAD 0x04
#define TCNT 0x08
#define WCNT 0x0C
#define BCNT 0x10
#define FDATA 0x14
#define CNT 0x18
#define TMCNT_L(t) IO16(0x04000100 + (t) * 4)
#define TMCNT_H(t) IO16(0x04000102 + (t) * 4)
#define VCOUNT_REG 0x04000004 // DISPSTAT, then VCOUNT in the top half of the word

#define ENABLE (1u << 31)
#define IRQ (1u << 30)
#define FOREVER (1u << 29)
#define MODE(m) ((unsigned)(m) << 24)
#define DST_STEP(n) ((unsigned)(n) << 10)
#define SRC_STEP(n) ((unsigned)(n) << 13)
#define RELOAD_D (1u << 12)
#define RELOAD_S (1u << 15)
#define INC 0
#define DEC 1
#define FIXED 2
#define OTHER 3
#define EMPTY 0xFFFFFFFF

static volatile unsigned *buf;

// The ARM7 has no cache; the ARM9 must write out what it prepared and forget what NDMA wrote
#ifdef ARM9
static void writeOut(void) { armDCacheFlush((void*)buf, BUFFER_WORDS * 4); }
static void forget(void) { armDCacheInvalidate((void*)buf, BUFFER_WORDS * 4); }
#else
static void writeOut(void) {}
static void forget(void) {}
#endif

static void nextLine195(void) { while (LINE == 195); while (LINE != 195); }
static void frames(int n) { while (n--) nextLine195(); }

static void clearBuffer(void) {
    for (int i = 0; i < BUFFER_WORDS; i++) buf[i] = EMPTY;
    writeOut();
}

static void setUp(unsigned src, unsigned dst, unsigned total, unsigned block) {
    REG(CHANNEL, CNT) = 0;
    REG(CHANNEL, SAD) = src;
    REG(CHANNEL, DAD) = dst;
    REG(CHANNEL, TCNT) = total;
    REG(CHANNEL, WCNT) = block;
    REG(CHANNEL, BCNT) = 0;
    IF = 1u << (28 + CHANNEL);
}

static unsigned irqSeen(void) {
    unsigned seen = (IF >> (28 + CHANNEL)) & 1;
    IF = 1u << (28 + CHANNEL);
    return seen;
}

static int filled(void) {
    forget();
    int n = 0;
    while (n < BUFFER_WORDS && buf[n] != EMPTY) n++;
    return n;
}

static void immediate(unsigned *out, unsigned mode) {
    // Words written, the word at 3, the CNT after, TCNT after, the IRQ, SAD and DAD after
    clearBuffer();
    setUp((unsigned)buf + 0x1000, (unsigned)buf, 2, 5);
    buf[0x400] = 0x11; buf[0x401] = 0x22; buf[0x402] = 0x33; buf[0x403] = 0x44; buf[0x404] = 0x55; writeOut();
    REG(CHANNEL, CNT) = ENABLE | IRQ | MODE(mode);
    frames(1);
    out[0] = filled();
    out[1] = buf[3];
    out[2] = REG(CHANNEL, CNT);
    out[3] = REG(CHANNEL, TCNT);
    out[4] = irqSeen();
    out[5] = REG(CHANNEL, SAD) - (unsigned)buf;
    out[6] = REG(CHANNEL, DAD) - (unsigned)buf;
}

static void steps(unsigned *out, unsigned dstStep, unsigned srcStep) {
    // The six words around where the destination starts (at 2), then CNT after, for 3 words starting from pattern word 4
    clearBuffer();
    for (int i = 0; i < 8; i++) buf[0x400 + i] = 0x1000 + i;
    writeOut();
    REG(CHANNEL, FDATA) = 0x77777777;
    setUp((unsigned)&buf[0x404], (unsigned)&buf[2], 1, 3);
    REG(CHANNEL, CNT) = ENABLE | MODE(0x10) | DST_STEP(dstStep) | SRC_STEP(srcStep);
    frames(1);
    forget();
    for (int i = 0; i < 6; i++) out[i] = buf[i];
    out[6] = REG(CHANNEL, CNT);
    out[7] = REG(CHANNEL, SAD) - (unsigned)buf;
    REG(CHANNEL, CNT) = 0;
}

static void lines(unsigned *out, int mode) {
    // Over 3 frames: transfers, first, last, lowest and highest line, lines with two or more, how many frames saw any
    clearBuffer();
    nextLine195();
    setUp(VCOUNT_REG, (unsigned)buf, BUFFER_WORDS, 1);
    REG(CHANNEL, CNT) = ENABLE | MODE(mode) | SRC_STEP(FIXED);
    frames(3);
    REG(CHANNEL, CNT) = 0;
    int n = filled();
    unsigned low = 0xFFFF, high = 0, doubles = 0, seen = 0, previous = 0xFFFF;
    for (int i = 0; i < n; i++) {
        unsigned line = buf[i] >> 16;
        if (line < low) low = line;
        if (line > high) high = line;
        if (line == previous) doubles++;
        if (i == 0 || line <= previous) seen++;
        previous = line;
    }
    out[0] = n;
    out[1] = n ? buf[0] >> 16 : 0xFFFF;
    out[2] = n ? buf[n - 1] >> 16 : 0xFFFF;
    out[3] = low;
    out[4] = high;
    out[5] = doubles;
    out[6] = seen;
}

static void timer(unsigned *out, int t, int mode) {
    // Transfers in 3 frames of a timer overflowing every 256 ticks at F/64, CNT after, and lines between the first two
    if (TMCNT_H(t) & 0x80) { out[0] = BUSY; return; }
    clearBuffer();
    nextLine195();
    setUp(VCOUNT_REG, (unsigned)buf, BUFFER_WORDS, 1);
    REG(CHANNEL, CNT) = ENABLE | MODE(mode) | SRC_STEP(FIXED);
    TMCNT_L(t) = 0xFF00;
    TMCNT_H(t) = 0x81;
    frames(3);
    TMCNT_H(t) = 0;
    unsigned cnt = REG(CHANNEL, CNT);
    REG(CHANNEL, CNT) = 0;
    int n = filled();
    out[0] = n;
    out[1] = cnt;
    out[2] = n >= 2 ? (buf[1] >> 16) - (buf[0] >> 16) : 0xFFFF;
}

#ifdef ARM9
#define GXSTAT IO(0x04000600)
#define POWERCNT IO16(0x04000304)
#define GXFIFO 0x04000400
#define MTX_MODE IO(0x04000440)
#define MTX_IDENTITY IO(0x04000454)
#define CLIP_X IO(0x04000670)

static void gx(unsigned *out, int fill) {
    // Whether it finished, IRQ, SAD and DAD after, TCNT after, and the X translation the 3D engine ended up with, which
    // needs every word to have arrived in order: 75 translates by 1.0, in blocks of 200 words
    POWERCNT |= (1 << 2) | (1 << 3);
    while (GXSTAT & (1u << 27));
    MTX_MODE = 0;
    MTX_IDENTITY = 0;
    MTX_MODE = 2;
    MTX_IDENTITY = 0;
    for (int i = 0; i < 75; i++) {
        buf[i * 4] = 0x1C;
        buf[i * 4 + 1] = 0x1000;
        buf[i * 4 + 2] = 0;
        buf[i * 4 + 3] = 0;
    }
    writeOut();
    for (int i = 0; i < fill; i++) IO(GXFIFO) = 0x15151515; // 240 entries, under the 256 where a CPU would stop
    setUp((unsigned)buf, GXFIFO, 300, 200);
    REG(CHANNEL, CNT) = ENABLE | IRQ | MODE(0xA) | DST_STEP(FIXED);
    int waited = 0;
    while ((REG(CHANNEL, CNT) & ENABLE) && waited < 30) { frames(1); waited++; }
    out[0] = waited < 30;
    out[1] = irqSeen();
    out[2] = REG(CHANNEL, SAD) - (unsigned)buf;
    out[3] = REG(CHANNEL, DAD);
    out[4] = REG(CHANNEL, TCNT);
    REG(CHANNEL, CNT) = 0;
    while (GXSTAT & (1u << 27));
    out[5] = CLIP_X;
}
#else
static void gx(unsigned *out, int fill) { out[0] = SKIPPED; }
#endif

static void tests(int test, unsigned *out) {
#ifdef ARM9
    const int arm9 = 1;
#else
    const int arm9 = 0;
#endif
    unsigned source = (unsigned)&buf[0x400];
    switch (test) {
    case T_REGS: {
        // What each register keeps of all ones
        unsigned gcnt = GCNT;
        GCNT = 0xFFFFFFFF; out[0] = GCNT; GCNT = gcnt;
        REG(CHANNEL, CNT) = 0;
        REG(CHANNEL, BCNT) = 0xFFFFFFFF; out[1] = REG(CHANNEL, BCNT); REG(CHANNEL, BCNT) = 0;
        REG(CHANNEL, TCNT) = 0xFFFFFFFF; out[2] = REG(CHANNEL, TCNT);
        REG(CHANNEL, WCNT) = 0xFFFFFFFF; out[3] = REG(CHANNEL, WCNT);
        REG(CHANNEL, SAD) = 0xFFFFFFFF; out[4] = REG(CHANNEL, SAD);
        REG(CHANNEL, DAD) = 0xFFFFFFFF; out[5] = REG(CHANNEL, DAD);
        REG(CHANNEL, FDATA) = 0xFFFFFFFF; out[6] = REG(CHANNEL, FDATA);
        REG(CHANNEL, CNT) = 0x7FFFFFFF; out[7] = REG(CHANNEL, CNT);
        REG(CHANNEL, CNT) = 0;
        break;
    }
    case T_REGS_ENABLED: {
        // GBATEK says something makes these read-only: what SAD, DAD, TCNT, WCNT and CNT's low bits read after writes
        // while a channel waits, then the destination it really used and how many words a V-blank moved
        clearBuffer();
        nextLine195();
        setUp(source, (unsigned)buf, 100, 1);
        buf[0x400] = 0x1234; writeOut();
        REG(CHANNEL, CNT) = ENABLE | MODE(0x6) | SRC_STEP(FIXED);
        REG(CHANNEL, SAD) = source + 8;
        REG(CHANNEL, DAD) = (unsigned)&buf[8];
        REG(CHANNEL, TCNT) = 50;
        REG(CHANNEL, WCNT) = 2;
        out[0] = REG(CHANNEL, SAD) - source;
        out[1] = REG(CHANNEL, DAD) - (unsigned)buf;
        out[2] = REG(CHANNEL, TCNT);
        out[3] = REG(CHANNEL, WCNT);
        REG(CHANNEL, CNT) = ENABLE | MODE(0x6) | SRC_STEP(FIXED) | DST_STEP(FIXED);
        out[4] = REG(CHANNEL, CNT);
        frames(1);
        REG(CHANNEL, CNT) = 0;
        forget();
        out[5] = buf[0];
        out[6] = buf[8];
        out[7] = buf[1];
        break;
    }
    case T_REGS_ENABLED2: {
        // While a channel waits: FDATA and BCNT after writes, CNT after writing its IRQ bit, then its repeat bit and mode
        // Then CNT after a byte write that clears only the enable bit, and whether SAD can be written again after that
        clearBuffer();
        nextLine195();
        setUp(source, (unsigned)buf, 100, 1);
        REG(CHANNEL, FDATA) = 0x11111111;
        REG(CHANNEL, CNT) = ENABLE | MODE(0x6) | SRC_STEP(FIXED);
        REG(CHANNEL, FDATA) = 0x22222222;
        REG(CHANNEL, BCNT) = 0x12345;
        out[0] = REG(CHANNEL, FDATA);
        out[1] = REG(CHANNEL, BCNT);
        REG(CHANNEL, CNT) = ENABLE | IRQ | MODE(0x6) | SRC_STEP(FIXED);
        out[2] = REG(CHANNEL, CNT);
        REG(CHANNEL, CNT) = ENABLE | FOREVER | MODE(0x7) | SRC_STEP(FIXED);
        out[3] = REG(CHANNEL, CNT);
        *(volatile unsigned char*)(0x04004104 + CHANNEL * 0x1C + CNT + 3) = 0x06;
        out[4] = REG(CHANNEL, CNT);
        REG(CHANNEL, SAD) = source + 4;
        out[5] = REG(CHANNEL, SAD) - source;
        REG(CHANNEL, CNT) = 0;
        break;
    }
    case T_IMMEDIATE: immediate(out, 0x10); break;
    case T_IMMEDIATE_1F: immediate(out, 0x1F); break;
    case T_CHANNELS: {
        // Words moved by each channel, or BUSY for one already running
        for (int ch = 0; ch < 4; ch++) {
            if (REG(ch, CNT) & ENABLE) { out[ch] = BUSY; continue; }
            clearBuffer();
            buf[0x400] = 0x5A; writeOut();
            REG(ch, SAD) = source; REG(ch, DAD) = (unsigned)buf; REG(ch, TCNT) = 1; REG(ch, WCNT) = 4; REG(ch, BCNT) = 0;
            REG(ch, CNT) = ENABLE | MODE(0x10) | SRC_STEP(FIXED);
            frames(1);
            out[ch] = filled();
        }
        break;
    }
    case T_DST_DEC: steps(out, DEC, INC); break;
    case T_DST_FIXED: steps(out, FIXED, INC); break;
    case T_DST_RESERVED: steps(out, OTHER, INC); break;
    case T_SRC_DEC: steps(out, INC, DEC); break;
    case T_SRC_FIXED: steps(out, INC, FIXED); break;
    case T_FILL: steps(out, INC, OTHER); break;
    case T_UNEVEN_TOTAL: {
        // Blocks of 2 with a total of 3: after each of 2 V-blanks the words, CNT's enable and IRQ; then TCNT
        clearBuffer();
        buf[0x400] = 0x3C; writeOut();
        nextLine195();
        setUp(source, (unsigned)buf, 3, 2);
        REG(CHANNEL, CNT) = ENABLE | IRQ | MODE(0x6) | SRC_STEP(FIXED);
        for (int i = 0; i < 2; i++) {
            frames(1);
            out[i * 3] = filled();
            out[i * 3 + 1] = REG(CHANNEL, CNT) >> 31;
            out[i * 3 + 2] = irqSeen();
        }
        out[6] = REG(CHANNEL, TCNT);
        REG(CHANNEL, CNT) = 0;
        break;
    }
    case T_RERUN: {
        // After a total of 3 is done, enable again without writing anything else: TCNT then, words after 2 V-blanks, enable
        clearBuffer();
        buf[0x400] = 0x3C; writeOut();
        nextLine195();
        setUp(source, (unsigned)buf, 3, 2);
        REG(CHANNEL, CNT) = ENABLE | MODE(0x6) | SRC_STEP(FIXED);
        frames(2);
        out[0] = filled();
        out[1] = REG(CHANNEL, TCNT);
        clearBuffer();
        buf[0x400] = 0x3C; writeOut();
        REG(CHANNEL, CNT) = ENABLE | MODE(0x6) | SRC_STEP(FIXED);
        frames(2);
        out[2] = filled();
        out[3] = REG(CHANNEL, CNT) >> 31;
        out[4] = REG(CHANNEL, TCNT);
        REG(CHANNEL, CNT) = 0;
        break;
    }
    case T_BLOCK_0: {
        // A block size of 0 is the largest, so the total of 3 ends it
        clearBuffer();
        buf[0x400] = 0x3C; writeOut();
        nextLine195();
        setUp(source, (unsigned)buf, 3, 0);
        REG(CHANNEL, CNT) = ENABLE | MODE(0x6) | SRC_STEP(FIXED);
        frames(1);
        out[0] = filled();
        out[1] = REG(CHANNEL, CNT) >> 31;
        REG(CHANNEL, CNT) = 0;
        break;
    }
    case T_TOTAL_0: {
        // A total of 0 is the largest, so it keeps going
        clearBuffer();
        buf[0x400] = 0x3C; writeOut();
        nextLine195();
        setUp(source, (unsigned)buf, 0, 2);
        REG(CHANNEL, CNT) = ENABLE | MODE(0x6) | SRC_STEP(FIXED);
        frames(2);
        out[0] = filled();
        out[1] = REG(CHANNEL, CNT) >> 31;
        REG(CHANNEL, CNT) = 0;
        break;
    }
    case T_INFINITE: {
        // After each of 2 V-blanks the words, enable and IRQ
        clearBuffer();
        buf[0x400] = 0x3C; writeOut();
        nextLine195();
        setUp(source, (unsigned)buf, 1, 2);
        REG(CHANNEL, CNT) = ENABLE | IRQ | FOREVER | MODE(0x6) | SRC_STEP(FIXED);
        for (int i = 0; i < 2; i++) {
            frames(1);
            out[i * 3] = filled();
            out[i * 3 + 1] = REG(CHANNEL, CNT) >> 31;
            out[i * 3 + 2] = irqSeen();
        }
        REG(CHANNEL, CNT) = 0;
        break;
    }
    case T_RESTART: {
        // Disabled partway: words after a quiet V-blank; enabled again, where the next words land
        clearBuffer();
        buf[0x400] = 0x3C; writeOut();
        nextLine195();
        setUp(source, (unsigned)buf, 100, 1);
        REG(CHANNEL, CNT) = ENABLE | MODE(0x6) | SRC_STEP(FIXED);
        frames(2);
        REG(CHANNEL, CNT) = 0;
        out[0] = filled();
        frames(1);
        out[1] = filled();
        for (int i = 0; i < BUFFER_WORDS; i++) buf[i] = EMPTY;
        buf[0x400] = 0x3C; writeOut();
        REG(CHANNEL, CNT) = ENABLE | MODE(0x6) | SRC_STEP(FIXED);
        frames(1);
        REG(CHANNEL, CNT) = 0;
        out[2] = filled();
        break;
    }
    case T_NO_IRQ: {
        // No interrupt flag without the enable bit
        clearBuffer();
        setUp(source, (unsigned)buf, 1, 1);
        REG(CHANNEL, CNT) = ENABLE | MODE(0x10);
        frames(1);
        out[0] = irqSeen();
        break;
    }
    case T_RELOAD_DST: {
        // Blocks of 2, a total of 6, reloading the destination: words filled at the end
        clearBuffer();
        buf[0x400] = 0x3C; writeOut();
        nextLine195();
        setUp(source, (unsigned)buf, 6, 2);
        REG(CHANNEL, CNT) = ENABLE | MODE(0x6) | SRC_STEP(FIXED) | RELOAD_D;
        frames(3);
        out[0] = filled();
        out[1] = REG(CHANNEL, CNT) >> 31;
        out[2] = REG(CHANNEL, DAD) - (unsigned)buf;
        REG(CHANNEL, CNT) = 0;
        break;
    }
    case T_RELOAD_SRC: {
        // Blocks of 2, reloading the source: the four words written over 2 V-blanks
        clearBuffer();
        buf[0x400] = 0x61; buf[0x401] = 0x62; buf[0x402] = 0x63; writeOut();
        nextLine195();
        setUp(source, (unsigned)buf, 4, 2);
        REG(CHANNEL, CNT) = ENABLE | MODE(0x6) | RELOAD_S;
        frames(2);
        REG(CHANNEL, CNT) = 0;
        forget();
        for (int i = 0; i < 4; i++) out[i] = buf[i];
        break;
    }
    case T_LINES_VBLANK: lines(out, 0x6); break;
    case T_LINES_7: lines(out, 0x7); break;
    case T_LINES_8: if (arm9) lines(out, 0x8); else out[0] = SKIPPED; break; // On the ARM7 that's the SD card
    case T_LINES_5: lines(out, 0x5); break;
    case T_LINES_9: if (arm9) lines(out, 0x9); else out[0] = SKIPPED; break; // DSi Wi-Fi on the ARM7
    case T_LINES_B: if (arm9) lines(out, 0xB); else out[0] = SKIPPED; break; // AES on the ARM7
    case T_LINES_C: lines(out, 0xC); break;
    case T_LINES_D: lines(out, 0xD); break;
    case T_LINES_E: lines(out, 0xE); break;
    case T_LINES_F: lines(out, 0xF); break;
    case T_TIMER0: timer(out, 0, 0); break;
    case T_TIMER1: timer(out, 1, 1); break;
    case T_TIMER2: timer(out, 2, 2); break;
    case T_TIMER3: timer(out, 3, 3); break;
    case T_OTHER_TIMER: timer(out, 2, 3); break; // Waiting for timer 3 while timer 2 runs
    case T_CASCADE: {
        // Timer 3 counting timer 2's overflows and overflowing every second one: transfers in 3 frames
        if ((TMCNT_H(2) & 0x80) || (TMCNT_H(3) & 0x80)) { out[0] = BUSY; break; }
        clearBuffer();
        nextLine195();
        setUp(VCOUNT_REG, (unsigned)buf, BUFFER_WORDS, 1);
        REG(CHANNEL, CNT) = ENABLE | MODE(3) | SRC_STEP(FIXED);
        TMCNT_L(2) = 0xFF00;
        TMCNT_L(3) = 0xFFFE;
        TMCNT_H(3) = 0x84;
        TMCNT_H(2) = 0x81;
        frames(3);
        TMCNT_H(2) = 0;
        TMCNT_H(3) = 0;
        REG(CHANNEL, CNT) = 0;
        out[0] = filled();
        break;
    }
    case T_GX: gx(out, 0); break;
    case T_GX_FULL: gx(out, 60); break;
    }
}

void runTest(int test, unsigned *out, volatile unsigned *buffer) {
    buf = buffer;
    ArmIrqState psr = armIrqLockByPsr();
    unsigned ime = REG_IME;
    REG_IME = 0;
    if (REG(CHANNEL, CNT) & ENABLE) out[0] = BUSY;
    else tests(test, out);
    IF = 1u << (28 + CHANNEL);
    REG_IME = ime;
    armIrqUnlockByPsr(psr);
}
