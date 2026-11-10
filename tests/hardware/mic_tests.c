// The DSi microphone tests, run by the ARM7 with interrupts off so nothing else moves. Timers 0 and 1 count cycles from
// just before the microphone is turned on, and a timeline records every change of MIC_CNT's status bits and IF2 bit 14
// as one word: the time in cycles shifted left 5, IF2 bit 14 in bit 4, and MIC_CNT bits 8-11 (empty, half, full, overrun)
// in bits 0-3. The microphone is unmuted in the touchscreen controller for every test but MUTED
#include <nds.h>
#include <calico/nds/arm7/codec.h>
#include "mic.h"

#define IO(a) (*(volatile unsigned*)(a))
#define IO16(a) (*(volatile unsigned short*)(a))
#define IO8(a) (*(volatile unsigned char*)(a))
#define MICCNT IO16(0x04004600)
#define MICDATA_ADDR 0x04004604
#define MICDATA IO(MICDATA_ADDR)
#define SNDEX IO16(0x04004700)
#define IF2 IO(0x0400021C)
#define REG(ch, off) IO(0x04004104 + (ch) * 0x1C + (off))
#define SAD 0x00
#define DAD 0x04
#define TCNT 0x08
#define WCNT 0x0C
#define BCNT 0x10
#define CNT 0x18
#define TMCNT_L(t) IO16(0x04000100 + (t) * 4)
#define TMCNT_H(t) IO16(0x04000102 + (t) * 4)

#define ENABLE (1u << 31)
#define FOREVER (1u << 29)
#define MODE(m) ((unsigned)(m) << 24)
#define DST_STEP(n) ((unsigned)(n) << 10)
#define SRC_STEP(n) ((unsigned)(n) << 13)
#define FIXED 2
#define EMPTY 0x5EB7A1C3 // Not 0 or FFFFFFFF, which muted samples read as

#define S_EMPTY 0x1
#define S_HALF 0x2
#define S_FULL 0x4
#define S_OVERRUN 0x8
#define S_IRQ 0x10
#define MIC_ON 0x8000
#define CLEAR 0x1000
#define IRQ_MODE(n) ((n) << 13)

static volatile unsigned *buf;
static unsigned short sndexOrig;
static unsigned char codecOrig[4];

static void startTimer(void) {
    // Timer 0 at the full 33MHz, counted by timer 1, so now() gives 32-bit cycles
    TMCNT_H(0) = 0; TMCNT_H(1) = 0;
    TMCNT_L(0) = 0; TMCNT_L(1) = 0;
    TMCNT_H(1) = 0x84;
    TMCNT_H(0) = 0x80;
}

static unsigned now(void) {
    unsigned hi, lo;
    do { hi = TMCNT_L(1); lo = TMCNT_L(0); } while (hi != TMCNT_L(1));
    return hi << 16 | lo;
}

static void wait(unsigned cycles) { unsigned t = now(); while (now() - t < cycles); }
static unsigned status(void) { return ((MICCNT >> 8) & 0xF) | (((IF2 >> 14) & 1) << 4); }

static int waitFor(unsigned bits) {
    // Wait up to about 30ms for a status bit
    unsigned t = now();
    while (!(status() & bits)) if (now() - t > 1000000) return 0;
    return 1;
}

static void stopMic(void) {
    // Turn the microphone off, then clear the FIFO and its overrun, which needs it off first
    MICCNT = 0;
    MICCNT = CLEAR;
    IF2 = 1u << 14;
}

static void setSndex(unsigned short value) {
    // The frequency bit can only change while bit 15 is clear
    SNDEX = SNDEX & 0x7FFF;
    SNDEX = value & 0x7FFF;
    SNDEX = value;
}

static void timeline(unsigned *out, unsigned cnt, int hz47, int sndexOff) {
    // 0: events, 1: status before, 2-13: events, 14-17: the first 4 words read afterwards, 18: how many of the 16 words
    // read have equal halves, 19: status after 16 reads, 20-21: the next 2 reads, 22: status after, 23: MIC_CNT at the end
    stopMic();
    if (hz47) setSndex(sndexOrig | 0x2000);
    if (sndexOff) SNDEX = sndexOrig & 0x7FFF;
    unsigned last = status(), n = 0, overrunAt = 0;
    out[1] = last;
    startTimer();
    MICCNT = cnt;
    while (n < 12) {
        unsigned t = now(), s = status();
        if (s != last) {
            out[2 + n++] = t << 5 | s;
            last = s;
            if ((s & S_OVERRUN) && !overrunAt) overrunAt = t;
        }
        if (t > 400000 || (overrunAt && t - overrunAt > 20000)) break;
    }
    out[0] = n;
    unsigned same = 0;
    for (int i = 0; i < 16; i++) {
        unsigned v = MICDATA;
        if (i < 4) out[14 + i] = v;
        same += (v & 0xFFFF) == (v >> 16);
    }
    out[18] = same;
    out[19] = status();
    out[20] = MICDATA;
    out[21] = MICDATA;
    out[22] = status();
    out[23] = MICCNT;
    stopMic();
    if (hz47 || sndexOff) setSndex(sndexOrig);
}

static unsigned copied(void) {
    unsigned n = 0;
    for (int i = 0; i < 32; i++) n += buf[i] != EMPTY;
    return n;
}

static void ndma(unsigned *out, unsigned block, int late, int forever) {
    // 0: events, 1-15: events as time << 11 | words copied << 5 | status, 16: NDMA CNT at the end, 17: status at the end,
    // 18: when NDMA was turned on. It copies 32 words in blocks, or keeps copying to one word forever
    if (REG(CHANNEL, CNT) & ENABLE) { out[0] = BUSY; return; }
    stopMic();
    for (int i = 0; i < BUFFER_WORDS; i++) buf[i] = EMPTY;
    REG(CHANNEL, SAD) = MICDATA_ADDR;
    REG(CHANNEL, DAD) = (unsigned)buf;
    REG(CHANNEL, TCNT) = forever ? 0 : 32;
    REG(CHANNEL, WCNT) = block;
    REG(CHANNEL, BCNT) = 0;
    unsigned cnt = ENABLE | MODE(0xC) | SRC_STEP(FIXED) | (forever ? FOREVER | DST_STEP(FIXED) : 0);
    startTimer();
    if (!late) REG(CHANNEL, CNT) = cnt;
    MICCNT = MIC_ON | 2;
    if (late) {
        if (!waitFor(S_HALF)) { out[0] = TIMEOUT; stopMic(); return; }
        out[18] = now();
        REG(CHANNEL, CNT) = cnt;
    }
    unsigned last = status(), n = 0;
    while (n < 15) {
        unsigned t = now(), s = copied() << 5 | status();
        if (s != last) { out[1 + n++] = t << 11 | s; last = s; }
        if (t > (forever ? 300000 : 150000) || (!forever && (s >> 5) == 32)) break;
    }
    out[0] = n;
    out[16] = REG(CHANNEL, CNT);
    out[17] = status();
    REG(CHANNEL, CNT) = 0;
    stopMic();
}

static void fill(void) {
    // Fill the FIFO to 16 words in mono and turn the microphone off
    stopMic();
    MICCNT = MIC_ON | 2;
    waitFor(S_FULL);
    MICCNT = 2;
}

static unsigned countReads(int size, unsigned addr) {
    // Read until the FIFO says it's empty
    unsigned n = 0;
    while (!(MICCNT & 0x100) && n < 70) {
        if (size == 1) (void)IO8(addr);
        else if (size == 2) (void)IO16(addr);
        else (void)IO(addr);
        n++;
    }
    return n;
}

static void words(unsigned *out) {
    // 0-7: the first 8 words of a full FIFO in mono, 8: how many of 16 have equal halves, 9: how many are 0 or FFFFFFFF
    fill();
    unsigned same = 0, flat = 0;
    for (int i = 0; i < 16; i++) {
        unsigned v = MICDATA;
        if (i < 8) out[i] = v;
        same += (v & 0xFFFF) == (v >> 16);
        flat += v == 0 || v == 0xFFFFFFFF;
    }
    out[8] = same;
    out[9] = flat;
    stopMic();
}

static void tests(int test, unsigned *out) {
    switch (test) {
    case T_REGS:
        // 0-3: SNDEXCNT, MIC_CNT and both as words when the test started; 4-6: MIC_CNT after writing 6F0F, 0 and 7FFF
        // while off; 7-12: SNDEXCNT after writing its value without bit 15, 7FFF, FFFF, DFFF, 5FFF, 0; 13: put back
        // (16-20, the touchscreen controller's 0:51, 0:52, 1:2E, 1:2F and DSi mode, are filled in before the test)
        out[0] = sndexOrig;
        out[1] = MICCNT;
        out[2] = IO(0x04004600);
        out[3] = IO(0x04004700);
        MICCNT = 0x6F0F; out[4] = MICCNT;
        MICCNT = 0; out[5] = MICCNT;
        MICCNT = 0x7FFF; out[6] = MICCNT;
        stopMic();
        SNDEX = sndexOrig & 0x7FFF; out[7] = SNDEX;
        SNDEX = 0x7FFF; out[8] = SNDEX;
        SNDEX = 0xFFFF; out[9] = SNDEX;
        SNDEX = 0xDFFF; out[10] = SNDEX;
        SNDEX = 0x5FFF; out[11] = SNDEX;
        SNDEX = 0x0000; out[12] = SNDEX;
        setSndex(sndexOrig);
        out[13] = SNDEX;
        break;

    case T_TL_F2_D0: timeline(out, MIC_ON | 2, 0, 0); break;
    case T_TL_F0_D0: timeline(out, MIC_ON | 0, 0, 0); break;
    case T_TL_F1_D0: timeline(out, MIC_ON | 1, 0, 0); break;
    case T_TL_F3_D0: timeline(out, MIC_ON | 3, 0, 0); break;
    case T_TL_F2_D1: timeline(out, MIC_ON | 2 | 1 << 2, 0, 0); break;
    case T_TL_F2_D2: timeline(out, MIC_ON | 2 | 2 << 2, 0, 0); break;
    case T_TL_F2_D3: timeline(out, MIC_ON | 2 | 3 << 2, 0, 0); break;
    case T_TL_F2_D0_47: timeline(out, MIC_ON | 2, 1, 0); break;
    case T_TL_F0_D1_47: timeline(out, MIC_ON | 0 | 1 << 2, 1, 0); break;
    case T_TL_IRQ1: timeline(out, MIC_ON | 2 | IRQ_MODE(1), 0, 0); break;
    case T_TL_IRQ2: timeline(out, MIC_ON | 2 | IRQ_MODE(2), 0, 0); break;
    case T_TL_IRQ3: timeline(out, MIC_ON | 2 | IRQ_MODE(3), 0, 0); break;
    case T_TL_SNDEX_OFF: timeline(out, MIC_ON | 2 | IRQ_MODE(3), 0, 1); break;

    case T_IRQ_EDGE:
        // In IRQ mode 3: 0: IF2 at half full, 1: again 3000 cycles after acknowledging it while still half full,
        // 2: right after reading 4 words, 3: once half full again, 4: status then
        stopMic();
        MICCNT = MIC_ON | 2 | IRQ_MODE(3);
        if (!waitFor(S_HALF)) { out[0] = TIMEOUT; break; }
        out[0] = status() >> 4;
        IF2 = 1u << 14;
        wait(3000);
        out[1] = status() >> 4;
        IF2 = 1u << 14;
        for (int i = 0; i < 4; i++) (void)MICDATA;
        out[2] = status() >> 4;
        waitFor(S_IRQ);
        out[3] = status() >> 4;
        out[4] = status();
        stopMic();
        break;

    case T_READ_SIZES:
        // 0: status of a full FIFO after turning off, 1-4: reads until empty with halfwords at 604, halfwords at 606,
        // bytes at 604, words; 5-8: a word, the halfword at 604, the halfword at 606, then another word; 9-10: a word read
        // from a freshly cleared FIFO, twice; 11: status after
        fill();
        out[0] = status();
        out[1] = countReads(2, MICDATA_ADDR);
        fill(); out[2] = countReads(2, MICDATA_ADDR + 2);
        fill(); out[3] = countReads(1, MICDATA_ADDR);
        fill(); out[4] = countReads(4, MICDATA_ADDR);
        fill();
        out[5] = MICDATA;
        out[6] = IO16(MICDATA_ADDR);
        out[7] = IO16(MICDATA_ADDR + 2);
        out[8] = MICDATA;
        stopMic();
        out[9] = MICDATA;
        out[10] = MICDATA;
        out[11] = status();
        break;

    case T_KEEP_CLEAR:
        // Status: 0: half full then off, 1: on again at once, 2: after an overrun, 3: off and on again, 4: after writing
        // the clear bit while on, 5: clear with the same write turning it off, 6: clear again while off; 7: MIC_CNT then
        stopMic();
        MICCNT = MIC_ON | 2;
        if (!waitFor(S_HALF)) { out[0] = TIMEOUT; break; }
        MICCNT = 2; out[0] = status();
        MICCNT = MIC_ON | 2; out[1] = status();
        waitFor(S_OVERRUN); wait(2000); out[2] = status();
        MICCNT = 2; MICCNT = MIC_ON | 2; out[3] = status();
        MICCNT = MIC_ON | CLEAR | 2; out[4] = status();
        MICCNT = CLEAR | 2; out[5] = status();
        MICCNT = CLEAR | 2; out[6] = status();
        out[7] = MICCNT;
        stopMic();
        break;

    case T_START_DELAY:
        // Cycles from turning on to the first sample, in stereo so one sample empties nothing out, for 12 starts at
        // different times: the same each time if sampling starts when it's turned on, spread if its clock runs freely
        for (int i = 0; i < 12; i++) {
            stopMic();
            wait(100 + i * 173);
            startTimer();
            MICCNT = MIC_ON | 0;
            unsigned t;
            while ((MICCNT & 0x100) && (t = now()) < 100000);
            out[i] = now();
        }
        stopMic();
        break;

    case T_NDMA_8: ndma(out, 8, 0, 0); break;
    case T_NDMA_4: ndma(out, 4, 0, 0); break;
    case T_NDMA_16: ndma(out, 16, 0, 0); break;
    case T_NDMA_1: ndma(out, 1, 0, 0); break;
    case T_NDMA_LATE: ndma(out, 8, 1, 0); break;
    case T_NDMA_OVERRUN: ndma(out, 8, 0, 1); break;
    case T_MUTED: words(out); break;
    case T_UNMUTED: words(out); break;
    }
}

static void codecMute(int mute) {
    // The touchscreen controller's microphone settings from GBATEK's flowcharts; calico's codec calls only work for
    // the thread holding the SPI lock, and read FF otherwise
    spiLock();
    if (mute) {
        cdcWriteReg(CdcPage_Control, CdcTscCtrlReg_AdcDigitalVolFine, 0x80);
        cdcWriteReg(CdcPage_Control, CdcTscCtrlReg_AdcDigitalMic, 0x00);
        cdcWriteReg(CdcPage_Sound, CdcTscSndReg_MicBias, 0x00);
    }
    else {
        cdcWriteReg(CdcPage_Sound, CdcTscSndReg_MicBias, 0x03);
        cdcWriteReg(CdcPage_Control, CdcTscCtrlReg_AdcDigitalMic, 0x80);
        cdcWriteReg(CdcPage_Control, CdcTscCtrlReg_AdcDigitalVolFine, 0x00);
        cdcWriteReg(CdcPage_Sound, CdcTscSndReg_MicPga, 0x37);
    }
    spiUnlock();
}

void runTest(int test, unsigned *out, volatile unsigned *buffer) {
    // The touchscreen controller is used with interrupts on, since libnds shares it with the touch screen
    buf = buffer;
    if (test == T_REGS) {
        sndexOrig = SNDEX;
        spiLock();
        codecOrig[0] = cdcReadReg(CdcPage_Control, CdcTscCtrlReg_AdcDigitalMic);
        codecOrig[1] = cdcReadReg(CdcPage_Control, CdcTscCtrlReg_AdcDigitalVolFine);
        codecOrig[2] = cdcReadReg(CdcPage_Sound, CdcTscSndReg_MicBias);
        codecOrig[3] = cdcReadReg(CdcPage_Sound, CdcTscSndReg_MicPga);
        spiUnlock();
        for (int i = 0; i < 4; i++) out[16 + i] = codecOrig[i];
        out[20] = cdcIsTwlMode();
        codecMute(0);
    }
    if (test == T_MUTED) codecMute(1);

    ArmIrqState psr = armIrqLockByPsr();
    unsigned ime = REG_IME;
    REG_IME = 0;
    if ((TMCNT_H(0) & 0x80) || (TMCNT_H(1) & 0x80)) out[0] = BUSY;
    else startTimer(), tests(test, out);
    TMCNT_H(0) = 0;
    TMCNT_H(1) = 0;
    IF2 = 1u << 14;
    REG_IME = ime;
    armIrqUnlockByPsr(psr);

    if (test == T_MUTED) codecMute(0);
    if (test == T_UNMUTED) {
        spiLock();
        cdcWriteReg(CdcPage_Control, CdcTscCtrlReg_AdcDigitalVolFine, codecOrig[1]);
        cdcWriteReg(CdcPage_Control, CdcTscCtrlReg_AdcDigitalMic, codecOrig[0]);
        cdcWriteReg(CdcPage_Sound, CdcTscSndReg_MicBias, codecOrig[2]);
        cdcWriteReg(CdcPage_Sound, CdcTscSndReg_MicPga, codecOrig[3]);
        spiUnlock();
    }
}
