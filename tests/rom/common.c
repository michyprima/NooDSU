// Runs every case and timed unit on either CPU, reporting them to the emulator
#include "rom.h"
#ifdef ARM9
#define NAME "ARM9"
#define CPU 0
#else
#define NAME "ARM7"
#define CPU 1
#endif

struct Case { const char *code; int mode, region, risky; };
struct Timed { void (*code)(int); };
#include "cases.h"

extern void run_case(const char *code, const unsigned *registers, unsigned *results, unsigned flags);

static const unsigned inputs[][13] = {
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0x02000000 },
    { 0xFFFFFFFF, 1, 0xFFFFFFFF, 31, 0x80000000, 0x7FFFFFFF, 32, 33, 0xFFFF, 0x8000, 0x7FFF, 0x10000, 0x02000000 },
    { 0x7FFFFFFF, 0x7FFFFFFF, 1, 1, 0x7FFF8000, 0x80007FFF, 64, 255, 256, 0xAAAAAAAA, 0x55555555, 0xF0F0F0F0, 0x02000004 },
    { 0x80000000, 0x80000000, 0x80000000, 0, 0xFF00FF00, 0x00FF00FF, 1, 2, 3, 4, 5, 6, 0x02000008 },
    { 0x12345678, 0x9ABCDEF0, 0x0F1E2D3C, 0x4B5A6978, 0xC3D2E1F0, 0x76543210, 0xFEDCBA98, 0x01234567, 0x89ABCDEF, 13, 17, 19, 0x0200000C },
    { 0xFFFF8000, 0x00018000, 0xFFFF0001, 16, 0x40000000, 0xC0000000, 0xBFFFFFFF, 0x3FFFFFFF, 7, 15, 63, 0, 0x02000010 },
};
static const unsigned flagSets[] = { 0x00000000, 0x60000000, 0x90000000, 0xF0000000 };

// Where each kind of memory is, the same as in gen's symbols: not there, main RAM, slow, DTCM, ITCM, the ARM7's own
static volatile unsigned *const regions[] = { 0,
#ifdef ARM9
    (volatile unsigned*)DATA_MAIN, (volatile unsigned*)DATA_SLOW, (volatile unsigned*)DATA_DTCM, (volatile unsigned*)DATA_ITCM, 0
#else
    (volatile unsigned*)DATA_MAIN, 0, 0, 0, (volatile unsigned*)DATA_WRAM
#endif
};

static char text[0x2000];
static int textLength;
static int host;

static void flush(void) {
    if (host && textLength) {
        HOST_TEXT = (unsigned)text;
        HOST_LENGTH = textLength;
    }
    textLength = 0;
}

static void put(const char *s) {
    while (*s) text[textLength++] = *s++;
    if (textLength > (int)sizeof(text) - 256) flush();
}

static void putHex(unsigned value) {
    static const char digits[] = "0123456789ABCDEF";
    char s[9];
    for (int i = 0; i < 8; i++) s[i] = digits[(value >> (28 - i * 4)) & 0xF];
    s[8] = 0;
    put(s);
}

static void putNumber(unsigned value) {
    char s[11];
    int n = 10;
    s[n] = 0;
    do { s[--n] = '0' + (value - (value / 10) * 10); value = value / 10; } while (value);
    put(s + n);
}

static unsigned hashWord(unsigned hash, unsigned word) {
    return (hash ^ word) * 16777619u;
}

static void fillRegion(volatile unsigned *memory, unsigned seed, int count) {
    for (int i = 0; i < count; i++) memory[i] = (seed + i * 4) * 0x01010101 ^ 0x5A3C0F96;
}

static unsigned hashRegion(unsigned hash, volatile unsigned *memory, int count) {
    for (int i = 0; i < count; i++) hash = hashWord(hash, memory[i]);
    return hash;
}

static unsigned runCase(const struct Case *c, volatile unsigned short *sets) {
    volatile unsigned *memory = regions[c->region];
    unsigned hash = 2166136261u, results[16];
    for (unsigned i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
        for (unsigned j = 0; j < sizeof(flagSets) / sizeof(flagSets[0]); j++) {
            if (memory) {
                fillRegion(memory, i * 16 + j, 64);
                fillRegion(memory + 0x3F0, i * 16 + j, 32);
            }
            run_case(c->code, inputs[i], results, flagSets[j] | c->mode);
            for (int k = 0; k < 16; k++) hash = hashWord(hash, results[k]);
            if (memory) {
                hash = hashRegion(hash, memory, 64);
                hash = hashRegion(hash, memory + 0x3F0, 32);
            }

            // The hash so far, so the first set to differ can be found, since a difference carries on into the later ones
            sets[i * 4 + j] = hash ^ (hash >> 16);
        }
    }
    return hash;
}

void caseDetail(int index, int set, volatile unsigned *out) {
    // Run one case with one set again, for its results
    const struct Case *c = &cases[index];
    volatile unsigned *memory = regions[c->region];
    // Always the same results address, since cases can see it through the stack pointer
    static unsigned results[16];
    unsigned i = set / 4, j = set % 4;
    if (memory) {
        fillRegion(memory, i * 16 + j, 64);
        fillRegion(memory + 0x3F0, i * 16 + j, 32);
    }
    run_case(c->code, inputs[i], results, flagSets[j] | c->mode);
    for (int k = 0; k < 16; k++) out[k] = results[k];
    for (int k = 0; k < 16; k++) out[16 + k] = memory ? memory[0x10 + k] : 0;
}

// Interrupts arriving during a run, which must leave the run's sums as they'd be without any
static volatile unsigned interrupts;


static void onInterrupt(void) {
    interrupts++;
    IF = IF;
}

#ifdef ARM9
#define IRQ_VECTOR REG32(0x027E3FFC)
#else
#define IRQ_VECTOR REG32(0x0380FFFC)
#endif

extern void setInterrupts(int on);

static unsigned sumArm(unsigned count) {
    unsigned sum = 1;
    for (unsigned i = 0; i < count; i++) sum = (sum * 33) ^ (sum >> 7) ^ i;
    return sum;
}

__attribute__((target("thumb"))) static unsigned sumThumb(unsigned count) {
    unsigned sum = 1;
    for (unsigned i = 0; i < count; i++) sum = (sum * 33) ^ (sum >> 7) ^ i;
    return sum;
}

static void wait(void) {
#ifdef ARM9
    __asm__ volatile("mov r0, #0\nmcr p15, 0, r0, c7, c0, 4" : : : "r0");
#else
    __asm__ volatile("swi #0x060000" : : : "r0", "r1", "r2", "r3", "lr");
#endif
}

#ifdef ARM7
__attribute__((unused))
#endif
static void line(const char *kind, unsigned hash, const char *name) {
    put(kind);
    put(" " NAME " ");
    putHex(hash);
    put(" ");
    put(name);
    put("\n");
}

extern unsigned run_mul(int which, unsigned rm, unsigned rs, unsigned low, unsigned high, unsigned flags);

static unsigned surveyMultiplies(void) {
    // tests/rom/multiply.py has the same operands, to work out what a real ARM7 does to the carry
    static const unsigned grid[SURVEY_GRID] = { 0, 1, 2, 3, 0x7F, 0x80, 0xFF, 0x100, 0xFFFF, 0x10000, 0xFFFFFF, 0x1000000, 0x7FFFFFFF,
        0x80000000, 0xFFFFFFFF, 0xFFFFFF00, 0xFFFF0000, 0xFF000000, 0xAAAAAAAA, 0x55555555, 0x12345678, 0x89ABCDEF, 0xFFFFFF80, 0xC0000000 };
    unsigned hash = 2166136261u;
    int at = 0;
    for (int op = 0; op < SURVEY_OPS; op++) {
        unsigned random = 0x13579BDF;
        for (int pair = 0; pair < SURVEY_PAIRS; pair++) {
            unsigned rm, rs, low = 0, high = 0;
            if (pair < SURVEY_GRID * SURVEY_GRID) {
                rm = grid[pair / SURVEY_GRID];
                rs = grid[pair % SURVEY_GRID];
            }
            else {
                unsigned value[4];
                for (int i = 0; i < 4; i++) {
                    random ^= random << 13;
                    random ^= random >> 17;
                    random ^= random << 5;
                    value[i] = random;
                }
                // Smaller multipliers often, since how many steps a multiply takes depends on them
                rm = value[0];
                rs = value[1] >> ((value[3] >> 8) & 0x18);
                low = value[2];
                high = value[3];
            }
            for (int carry = 0; carry < 2; carry++) {
                unsigned flags = run_mul(op, rm, rs, low, high, carry ? 0x20000000 : 0) >> 28;
                surveyOut[at++] = flags;
                hash = hashWord(hash, flags);
            }
        }
    }
    return hash;
}

static unsigned interruptCase(int thumb, int halted) {
    interrupts = 0;
    IRQ_VECTOR = (unsigned)onInterrupt;
    TM0CNT_L = 0xFC00;
    TM0CNT_H = 0xC0;
    IF = IF;
    IE = 1 << 3;
    IME = 1;
    setInterrupts(1);
    unsigned sum = 0;
    if (halted) {
        wait();
        sum = interrupts > 0;
    }
    else {
        sum = thumb ? sumThumb(200000) : sumArm(200000);
    }
    setInterrupts(0);
    TM0CNT_H = 0;
    IME = 0;
    IE = 0;
    IF = IF;
    return sum;
}

void runAll(void) {
    host = HOST_MAGIC == MAGIC;
    int count = sizeof(cases) / sizeof(cases[0]), detail = host ? HOST_DETAIL : 0;
    for (int i = 0; i < count; i++) {
        hashOut[i] = 0;
        if (!cases[i].code || (cases[i].risky && !host)) continue;
        if (host) HOST_PROGRESS = i;
        else onProgress(CPU, i);
        volatile unsigned short *sets = setsOut + i * SETS;
        hashOut[i] = runCase(&cases[i], sets);
        line("C", hashOut[i], caseNames[i]);
        if (detail == 1) {
            put("S " NAME);
            for (int j = 0; j < SETS; j++) {
                put(" ");
                putHex(sets[j]);
            }
            put("\n");
        }
    }
    line("C", interruptCase(0, 0), "interrupts during an ARM run");
    line("C", interruptCase(1, 0), "interrupts during a THUMB run");
    line("C", interruptCase(0, 1), "waking from a halt");
    line("C", surveyMultiplies(), "multiply flags survey");
    flush();

    // Times, the shortest of a few runs, to leave out whatever else the console was doing
    TM0CNT_L = 0;
    TM1CNT_L = 0;
    TM1CNT_H = 0x84;
    TM0CNT_H = 0x80;
    int timedTotal = sizeof(timed) / sizeof(timed[0]);
    for (int i = 0; i < timedTotal; i++) {
        cyclesOut[i] = 0;
        if (!timed[i].code) continue;
        if (host) HOST_PROGRESS = count + i;
        else onProgress(CPU, count + i);
        const int loops = 4000;
        unsigned best = 0xFFFFFFFF, guest = 0;
        for (int run = 0; run < 5; run++) {
            unsigned g0 = TM0CNT_L | (TM1CNT_L << 16), t0 = host ? HOST_MICROSECONDS : 0;
            timed[i].code(loops);
            unsigned t1 = host ? HOST_MICROSECONDS : 0, g1 = TM0CNT_L | (TM1CNT_L << 16);
            if (t1 - t0 < best) best = t1 - t0;
            guest = g1 - g0;
        }
        cyclesOut[i] = guest;
        put("T " NAME " ");
        putNumber(best);
        put(" ");
        putNumber(guest);
        put(" ");
        putNumber(loops * 8);
        put(" ");
        put(timedNames[i]);
        put("\n");
    }
    TM0CNT_H = 0;
    TM1CNT_H = 0;

    // Every case's results, for comparing with what a real DS saves
    if (detail == 2) {
        static unsigned words[DETAIL_WORDS];
        for (int i = 0; i < count; i++) {
            if (!cases[i].code || cases[i].risky) continue;
            HOST_PROGRESS = count + timedTotal + i;
            for (int set = 0; set < SETS; set++) {
                caseDetail(i, set, words);
                put("D ");
                putNumber(CPU);
                put(" ");
                putNumber(i);
                put(" ");
                putNumber(set);
                for (int k = 0; k < DETAIL_WORDS; k++) {
                    put(" ");
                    putHex(words[k]);
                }
                put("\n");
            }
        }
    }
    flush();
}

int caseCount(void) { return sizeof(cases) / sizeof(cases[0]); }
int timedCount(void) { return sizeof(timed) / sizeof(timed[0]); }
int caseRisky(int index) { return cases[index].risky; }
int hostHere(void) { return HOST_MAGIC == MAGIC; }
const char *caseName(int index) { return caseNames[index]; }
const char *timedName(int index) { return timedNames[index]; }
