// Runs everything on both CPUs and shows how it compares with what's built in
#include "rom.h"
#include "font.h"

// Filled in after building, from running this same ROM with NooDS U's interpreter
const volatile struct {
    unsigned hashes[2][MAX_CASES];
    unsigned short sets[2][MAX_CASES][SETS];
    unsigned cycles[2][MAX_TIMED];
} expected __attribute__((section(".expected"))) = { { { 0x4E4F4E45 } } };

static unsigned hashes[MAX_CASES], cycles[MAX_TIMED];
static unsigned short sets[MAX_CASES * SETS];
volatile unsigned *hashOut = hashes, *cyclesOut = cycles;
volatile unsigned short *setsOut = sets;
static unsigned char survey[SURVEY_BYTES];
volatile unsigned char *surveyOut = survey;

#define VRAM ((volatile unsigned short*)0x06800000)
#define DISPCNT REG32(0x04000000)
#define KEYS REG16(0x04000130)
#define VCOUNT REG16(0x04000006)
#define WHITE 0xFFFF
#define GREEN 0xC3E0
#define RED 0x801F
#define YELLOW 0x83FF
#define BLACK 0x8000
#define KEY_A 1
#define KEY_B 2
#define KEY_RIGHT 16
#define KEY_LEFT 32
#define KEY_UP 64
#define KEY_DOWN 128
#define KEY_R 256
#define KEY_L 512
#define ROWS 24

static int column, row;

static void clearScreen(void) {
    for (int i = 0; i < 256 * 192; i++) VRAM[i] = BLACK;
    column = row = 0;
}

static void text(const char *s, unsigned short color) {
    for (; *s; s++) {
        char c = (*s >= 'a' && *s <= 'z') ? *s - 32 : *s;
        if (c < 32 || c > 90) c = '?';
        if (column >= 42 || row >= ROWS) continue;
        const unsigned char *glyph = font[c - 32];
        for (int x = 0; x < 6; x++) {
            unsigned char bits = x < 5 ? glyph[x] : 0;
            for (int y = 0; y < 8; y++)
                VRAM[(row * 8 + y) * 256 + column * 6 + x + 1] = (bits >> y) & 1 ? color : BLACK;
        }
        column++;
    }
}

static void number(unsigned value, unsigned short color) {
    char s[11];
    int n = 10;
    s[n] = 0;
    do { s[--n] = '0' + value % 10; value /= 10; } while (value);
    text(s + n, color);
}

static void hex(unsigned value, unsigned short color) {
    static const char digits[] = "0123456789ABCDEF";
    char s[9];
    for (int i = 0; i < 8; i++) s[i] = digits[(value >> (28 - i * 4)) & 0xF];
    s[8] = 0;
    text(s, color);
}

static void newline(void) { column = 0; row++; }

static void wake7(void) { IPCSYNC = (1 << 14) | (1 << 13); }

// What's running goes on screen on a real DS, so a hang shows where it happened
void onProgress(int cpu, int index) {
    column = 0;
    row = 3;
    text(cpu ? "ARM7: " : "ARM9: ", YELLOW);
    text(index < caseCount() ? caseName(index) : timedName(index - caseCount()), WHITE);
    for (int i = column; i < 42; i++) text(" ", WHITE);
}

// What differs
static struct { short cpu, index; } differing[MAX_CASES * 2];
static int differCount, compared, skipped;

static void compare(int cpu) {
    volatile unsigned *got = cpu ? HASHES7 : hashes;
    for (int i = 0; i < caseCount(); i++) {
        if (!expected.hashes[cpu][i] && !got[i]) continue;
        if (!got[i]) {
            skipped++;
            continue;
        }
        compared++;
        if (expected.hashes[cpu][i] != got[i]) {
            differing[differCount].cpu = cpu;
            differing[differCount++].index = i;
        }
    }
}

static unsigned short waitKeys(void) {
    // Wait for a new press
    unsigned short held = ~KEYS & 0x3FF;
    while (1) {
        while (VCOUNT != 192);
        while (VCOUNT == 192);
        unsigned short now = ~KEYS & 0x3FF, pressed = now & ~held;
        held = now;
        if (pressed) return pressed;
    }
}

// What happened when saving, for the first page
static char saveLine[43];

static void title(void) {
    clearScreen();
    number(compared - differCount, differCount ? RED : GREEN);
    text(" OF ", WHITE);
    number(compared, WHITE);
    text(" MATCH", differCount ? RED : GREEN);
    if (skipped) {
        text(", ", WHITE);
        number(skipped, WHITE);
        text(" SKIPPED", WHITE);
    }
    newline();
    text(saveLine, saveLine[0] == 'S' && saveLine[4] == 'D' ? GREEN : RED);
    newline();
}

static volatile unsigned *rerun(int chosen, int *setOut) {
    // Run a case that differs again with the first set of values to differ, on its CPU
    int cpu = differing[chosen].cpu, index = differing[chosen].index, set = 0;
    volatile unsigned short *got = cpu ? SETS7 + index * SETS : sets + index * SETS;
    while (set < SETS - 1 && got[set] == expected.sets[cpu][index][set]) set++;
    *setOut = set;
    static unsigned local[DETAIL_WORDS];
    if (cpu) {
        DETAIL_CASE = index;
        DETAIL_SET = set;
        COMMAND = 2;
        wake7();
        while (COMMAND);
        return DETAIL7;
    }
    caseDetail(index, set, local);
    return local;
}

static void saveText(const char *s, int *at) {
    while (*s && *at < 42) saveLine[(*at)++] = *s++;
    saveLine[*at] = 0;
}

static void saveNumber(unsigned value, int *at) {
    char s[11];
    int n = 10;
    s[n] = 0;
    do { s[--n] = '0' + value % 10; value /= 10; } while (value);
    saveText(s + n, at);
}

static void saveResults(void) {
    // A header, then as many differences as fit: where each is, and what its case leaves
    column = 0;
    row = 3;
    text("SAVING RESULTS, THIS CAN TAKE A WHILE", YELLOW);
    const char *chipName = "";
    int at = 0;
    if (!saveStart(&chipName)) {
        saveText("COULDN'T SAVE: NO DLDI OR SAVE CHIP", &at);
        return;
    }
    int surveySize = saveCapacity() > 0x40000 ? 8 + SURVEY_BYTES * 2 : 0;
    int room = (saveCapacity() - 20 - surveySize) / (8 + DETAIL_WORDS * 4);
    int stored = differCount < room ? differCount : room;
    saveWord(0x5345524E); // NRES
    saveWord(compared);
    saveWord(differCount);
    saveWord(skipped);
    saveWord(stored);
    for (int i = 0; i < stored; i++) {
        int set;
        volatile unsigned *words = rerun(i, &set);
        saveWord(differing[i].cpu | (differing[i].index << 8));
        saveWord(set);
        for (int k = 0; k < DETAIL_WORDS; k++) saveWord(words[k]);
        if (!(i & 15)) {
            column = 0;
            row = 4;
            number(i, WHITE);
            text(" OF ", WHITE);
            number(stored, WHITE);
        }
    }
    // Then the flags after multiplies from both CPUs, if there's room
    if (surveySize) {
        saveWord(0x4C554D4E); // NMUL
        saveWord(SURVEY_BYTES);
        for (int cpu = 0; cpu < 2; cpu++) {
            volatile unsigned char *bytes = cpu ? SURVEY7 : survey;
            for (int i = 0; i + 4 <= SURVEY_BYTES; i += 4)
                saveWord(bytes[i] | (bytes[i + 1] << 8) | (bytes[i + 2] << 16) | ((unsigned)bytes[i + 3] << 24));
        }
    }
    int written = saveEnd();
    if (written <= 0) {
        saveText("SAVE FAILED AFTER ", &at);
        saveNumber(-written, &at);
        saveText(" BYTES", &at);
    }
    else {
        saveText("SAVED ", &at);
        saveNumber(written, &at);
        saveText(" BYTES: ", &at);
        saveText(chipName, &at);
    }
}

static void showDetail(int chosen) {
    int cpu = differing[chosen].cpu, index = differing[chosen].index, set;
    volatile unsigned *out = rerun(chosen, &set);

    clearScreen();
    text(cpu ? "ARM7 " : "ARM9 ", YELLOW);
    number(index, YELLOW);
    newline();
    text(caseName(index), WHITE);
    newline();
    text("INPUTS ", WHITE);
    number(set / 4, GREEN);
    text(" FLAGS ", WHITE);
    number(set % 4, GREEN);
    newline();
    newline();
    for (int i = 0; i < DETAIL_WORDS; i++) {
        if (i == 16) {
            text("MEMORY FROM +0X40", WHITE);
            newline();
        }
        hex(out[i], WHITE);
        text(" ", WHITE);
        if (i % 4 == 3) newline();
    }
    newline();
    text("B: BACK", GREEN);
}

static void showDiffering(int first, int chosen) {
    title();
    if (!differCount) {
        newline();
        text("L/R: CYCLES", GREEN);
        return;
    }
    text("A: DETAILS  L/R: CYCLES", GREEN);
    newline();
    for (int i = first; i < differCount && row < ROWS; i++) {
        text(i == chosen ? ">" : " ", YELLOW);
        text(differing[i].cpu ? "7 " : "9 ", differing[i].cpu ? GREEN : YELLOW);
        text(caseName(differing[i].index), WHITE);
        newline();
    }
}

static void showCycles(int cpu, int first) {
    // Cycles per instruction, in tenths, for this CPU and what NooDS U says
    volatile unsigned *got = cpu ? CYCLES7 : cycles;
    clearScreen();
    text(cpu ? "ARM7" : "ARM9", YELLOW);
    text(" CYCLES X10: REAL, NOODS U, NAME", WHITE);
    newline();
    int shown = 0;
    for (int i = 0; i < timedCount() && row < ROWS; i++) {
        if (!got[i]) continue;
        if (shown++ < first) continue;
        unsigned real = got[i] / 3200, emulated = expected.cycles[cpu][i] / 3200;
        number(real, real == emulated ? GREEN : RED);
        text(" ", WHITE);
        number(emulated, WHITE);
        text(" ", WHITE);
        text(timedName(i), WHITE);
        newline();
    }
}

void cpuMain(void) {
    REG32(0x04000304) = 0x8203; // Both 2D engines, since one that's off reads its palette as 0 on a real DS
    REG32(0x04000240) = 0x80;
    clearScreen();
    DISPCNT = 0x20000;
    text("NOODS U INSTRUCTION ROM", WHITE);
    newline();
    int host = hostHere();
    if (host) {
        // Blank screens and a sleeping ARM7 keep times to the CPU being measured
        text("RUNNING, THE SCREEN GOES BLANK", WHITE);
        while (VCOUNT != 192);
        DISPCNT = 0x80;
    }
    else {
        text("RUNNING, THIS TAKES A FEW MINUTES", WHITE);
    }
    runAll();

    // The ARM7's turn; the ARM9 only wakes with IME set, though interrupts stay off in the CPSR
    // Without the emulator's clock, nothing is being timed, so the ARM9 can show how the ARM7 is doing
    IPCSYNC = 1 << 14;
    IF = IF;
    COMMAND = 1;
    wake7();
    if (host) {
        IE = BIT16;
        IME = 1;
        while (!FINISHED) {
            __asm__ volatile("mov r0, #0\nmcr p15, 0, r0, c7, c0, 4" : : : "r0");
            IF = IF;
        }
        IME = 0;
        IE = 0;
    }
    else {
        int last = 0;
        while (!FINISHED) {
            int now = PROGRESS7;
            if (now != last && now) onProgress(1, now - 1);
            last = now;
        }
    }
    DISPCNT = 0x20000;
    if (host) HOST_DONE = 1;

    // Results
    if (expected.hashes[0][0] == 0x4E4F4E45) {
        clearScreen();
        text("NOTHING BUILT IN TO COMPARE WITH", WHITE);
        while (1);
    }
    compare(0);
    compare(1);
    if (!host) saveResults();

    int page = 0, first = 0, chosen = 0, cycleFirst = 0;
    while (1) {
        if (page == 0) showDiffering(first, chosen);
        else showCycles(page - 1, cycleFirst);
        unsigned short keys = waitKeys();
        if (keys & (KEY_L | KEY_R)) {
            page = (page + ((keys & KEY_R) ? 1 : 2)) % 3;
            cycleFirst = 0;
        }
        else if (page) {
            if (keys & KEY_DOWN) cycleFirst++;
            if ((keys & KEY_UP) && cycleFirst) cycleFirst--;
            if (keys & KEY_RIGHT) cycleFirst += 20;
            if (keys & KEY_LEFT) cycleFirst = cycleFirst > 20 ? cycleFirst - 20 : 0;
        }
        else if (differCount) {
            if (keys & KEY_A) {
                showDetail(chosen);
                while (!(waitKeys() & KEY_B));
            }
            if ((keys & KEY_DOWN) && chosen < differCount - 1) chosen++;
            if ((keys & KEY_UP) && chosen) chosen--;
            if (keys & KEY_RIGHT) chosen = chosen + 20 < differCount ? chosen + 20 : differCount - 1;
            if (keys & KEY_LEFT) chosen = chosen > 20 ? chosen - 20 : 0;
            if (chosen < first) first = chosen;
            if (chosen >= first + ROWS - 2) first = chosen - ROWS + 3;
        }
    }
}
