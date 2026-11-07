// Runs the tests on both CPUs and shows where this DS differs from what NooDS U does
#include <stdio.h>
#include <nds.h>
#include <fat.h>
#include "list.h"
void runNormal(int cpu);
void runRisky(int cpu, int index);

#define T(f, n) n,
static const char *const names[] = { NORMAL_TESTS RISKY_TESTS };
#undef T
#define T(f, n) +1
static const int normalCount = 0 NORMAL_TESTS, totalCount = 0 NORMAL_TESTS RISKY_TESTS;
#undef T

// From running this same ROM in NooDS U, which build.sh puts in expected.c
extern volatile const unsigned expected[2][MAX_TESTS][WORDS];
static struct Shared block;

#define SCREEN ((volatile unsigned short*)0x06800000)
#define KEYS (*(volatile unsigned short*)0x04000130)
#define VCOUNT (*(volatile unsigned short*)0x04000006)

static const unsigned char font[][5] = {
    {0,0,0,0,0},{0,0,0x5F,0,0},{0,7,0,7,0},{0x14,0x7F,0x14,0x7F,0x14},{0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,8,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50},{0,5,3,0,0},{0,0x1C,0x22,0x41,0},{0,0x41,0x22,0x1C,0},{0x14,8,0x3E,8,0x14},{8,8,0x3E,8,8},
    {0,0x50,0x30,0,0},{8,8,8,8,8},{0,0x60,0x60,0,0},{0x20,0x10,8,4,2},{0x3E,0x51,0x49,0x45,0x3E},{0,0x42,0x7F,0x40,0},
    {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30},{1,0x71,9,5,3},{0x36,0x49,0x49,0x49,0x36},{6,0x49,0x49,0x29,0x1E},{0,0x36,0x36,0,0},
    {0,0x56,0x36,0,0},{8,0x14,0x22,0x41,0},{0x14,0x14,0x14,0x14,0x14},{0,0x41,0x22,0x14,8},{2,1,0x51,9,6},
    {0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,9,9,9,1},{0x3E,0x41,0x49,0x49,0x7A},{0x7F,8,8,8,0x7F},
    {0,0x41,0x7F,0x41,0},{0x20,0x40,0x41,0x3F,1},{0x7F,8,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},{0x7F,2,0xC,2,0x7F},
    {0x7F,4,8,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},{0x7F,9,9,9,6},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,9,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{1,1,0x7F,1,1},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,8,0x14,0x63},{7,8,0x70,8,7},{0x61,0x51,0x49,0x45,0x43},
};

#define BIT_KEY(n) (1 << (n))
#define WHITE 0xFFFF
#define GREEN 0xC3E0
#define RED 0x801F
#define YELLOW 0x83FF
#define BLACK 0x8000

static void drawChar(int column, int row, char c, unsigned short color) {
    if (c < 32 || c > 90) c = '?';
    const unsigned char *glyph = font[c - 32];
    for (int x = 0; x < 6; x++) {
        unsigned char bits = x < 5 ? glyph[x] : 0;
        for (int y = 0; y < 8; y++)
            SCREEN[(row * 8 + y) * 256 + column * 6 + x + 1] = (bits >> y) & 1 ? color : BLACK;
    }
}

static int textColumn, textRow;

static void text(const char *s, unsigned short color) {
    for (; *s; s++) {
        if (textColumn >= 42) continue;
        drawChar(textColumn++, textRow, *s, color);
    }
}

static void hex(unsigned value, unsigned short color) {
    static const char digits[] = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4) {
        char s[2] = { digits[(value >> i) & 0xF], 0 };
        text(s, color);
    }
}

static void number(int value, unsigned short color) {
    char s[8];
    int n = 0;
    do { s[n++] = '0' + value % 10; value /= 10; } while (value);
    while (n) { char one[2] = { s[--n], 0 }; text(one, color); }
}

static void newLine(void) {
    while (textColumn < 42) drawChar(textColumn++, textRow, ' ', WHITE);
    textColumn = 0;
    textRow++;
}

static void clearFrom(int row) {
    textRow = row;
    textColumn = 0;
    while (textRow < 24) newLine();
}

void onTest(int cpu, int index) {
    // Say what's running, so a hang shows where it happened
    textRow = 2;
    textColumn = 0;
    text("ARM9 RUNNING: ", YELLOW);
    text(names[index], YELLOW);
    newLine();
}

// What's shown: lines of a kind, built from the results
enum { LINE_NAME, LINE_DIFF, LINE_WORD };
struct Line { unsigned char kind, cpu, test, word; };
static struct Line lines[4000];
static int lineCount;
static int shownCpu, showAll, scroll, ranCount, stuck;
static int same[2], different[2];
static int nextRisky;
static int hasExpected;
static const char *saved = "";

static void add(int kind, int cpu, int test, int word) {
    if (lineCount < 4000) lines[lineCount++] = (struct Line) { kind, cpu, test, word };
}

static void buildLines(void) {
    lineCount = 0;
    for (int cpu = 0; cpu < 2; cpu++) same[cpu] = different[cpu] = 0;
    for (int cpu = 0; cpu < 2; cpu++) {
        for (int test = 0; test < ranCount; test++) {
            int differs = 0;
            for (int w = 0; w < WORDS; w++)
                if (RESULTS(cpu)[test * WORDS + w] != expected[cpu][test][w]) differs = 1;
            if (differs) different[cpu]++;
            else same[cpu]++;
            if (cpu != shownCpu) continue;
            if (showAll) {
                add(LINE_NAME, cpu, test, differs);
                for (int w = 0; w < WORDS; w++)
                    if (RESULTS(cpu)[test * WORDS + w]) add(LINE_WORD, cpu, test, w);
            }
            else if (differs) {
                add(LINE_NAME, cpu, test, 1);
                for (int w = 0; w < WORDS; w++)
                    if (RESULTS(cpu)[test * WORDS + w] != expected[cpu][test][w]) add(LINE_DIFF, cpu, test, w);
            }
        }
    }
}

static void showResults(void) {
    textRow = 0;
    textColumn = 0;
    text("NOODS U INSTRUCTION TEST", WHITE);
    newLine();
    text(shownCpu ? "ARM7" : "ARM9", YELLOW);
    text(" SAME ", WHITE);
    number(same[shownCpu], GREEN);
    text(" DIFF ", WHITE);
    number(different[shownCpu], different[shownCpu] ? RED : GREEN);
    if (!hasExpected) text(" NO EXPECTED", RED);
    text(" ", WHITE);
    text(saved, saved[4] == 'D' ? GREEN : saved[4] == 'I' ? YELLOW : RED); // SAVED, SAVING, or a failure
    newLine();
    if (stuck) {
        // Keep this up, since it's the only way to know where the ARM7 stopped
        text("ARM7 STUCK AT ", RED);
        number(shared->progress7, RED);
        text(" ", RED);
        if (shared->progress7 >= 1 && shared->progress7 <= (unsigned)totalCount) text(names[shared->progress7 - 1], RED);
    }
    else {
        text(showAll ? "ALL RESULTS" : "DIFFERENCES", WHITE);
        text("  W=WORD R=DS E=NOODS", WHITE);
    }
    newLine();

    int row = 3;
    for (int i = scroll; i < lineCount && row < 22; i++, row++) {
        struct Line *line = &lines[i];
        volatile unsigned *results = RESULTS(line->cpu) + line->test * WORDS;
        textRow = row;
        textColumn = 0;
        if (line->kind == LINE_NAME) {
            number(line->test + 1, YELLOW);
            text(" ", WHITE);
            text(names[line->test], showAll && !line->word ? GREEN : RED);
        }
        else if (line->kind == LINE_DIFF) {
            text(" ", WHITE);
            number(line->word, WHITE);
            text(" R", WHITE);
            hex(results[line->word], RED);
            text(" E", WHITE);
            hex(expected[line->cpu][line->test][line->word], GREEN);
        }
        else {
            text(" ", WHITE);
            number(line->word, WHITE);
            text(" ", WHITE);
            hex(results[line->word], WHITE);
        }
        newLine();
    }
    clearFrom(row);
    textRow = 22;
    textColumn = 0;
    number(scroll + 1, WHITE);
    text("/", WHITE);
    number(lineCount, WHITE);
    text(" UP/DN LEFT/RIGHT PAGE", WHITE);
    newLine();
    text("L/R CPU  A ALL  START ", WHITE);
    if (nextRisky < totalCount - normalCount) text(names[normalCount + nextRisky], YELLOW);
    else text("NO MORE", WHITE);
    newLine();
}

static FILE *openSave(const char **where) {
    // The folder the ROM started from, then the root of the DSi's SD card or a flashcart's
    static int mounted = -1;
    if (mounted < 0) mounted = fatInitDefault();
    if (!mounted) return NULL;
    static const char *const paths[] = { "noodsu-hwtest.bin", "sd:/noodsu-hwtest.bin", "fat:/noodsu-hwtest.bin" };
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        FILE *file = fopen(paths[i], "wb");
        if (!file) continue;
        *where = paths[i];
        return file;
    }
    return NULL;
}

static void put(FILE *file, unsigned value, unsigned *hash) {
    fwrite(&value, 4, 1, file);
    for (int i = 0; i < 32; i += 8) *hash = (*hash ^ ((value >> i) & 0xFF)) * 16777619u;
}

static void saveResults(void) {
    // Write everything so far to the card after each run, with what NooDS U gives, so tests/hardware/results.py can
    // list the differences; the results are on screen first, in case saving hangs
    saved = "SAVING";
    showResults();
    const char *where;
    FILE *file = openSave(&where);
    if (!file) {
        saved = "NOT SAVED";
        return;
    }
    armDCacheInvalidate(shared->results[1], sizeof(shared->results[1]));
    unsigned hash = 2166136261u;
    put(file, SHARED_MAGIC0, &hash);
    put(file, ranCount, &hash);
    put(file, normalCount, &hash);
    put(file, stuck ? shared->progress7 : 0, &hash);
    for (int cpu = 0; cpu < 2; cpu++)
        for (int i = 0; i < MAX_TESTS * WORDS; i++) put(file, RESULTS(cpu)[i], &hash);
    for (int cpu = 0; cpu < 2; cpu++)
        for (int test = 0; test < MAX_TESTS; test++)
            for (int w = 0; w < WORDS; w++) put(file, expected[cpu][test][w], &hash);
    int good = fclose(file) == 0;

    // Read it back to be sure it's all there
    file = good ? fopen(where, "rb") : NULL;
    unsigned back = 2166136261u, value;
    if (file) {
        while (fread(&value, 4, 1, file) == 1)
            for (int i = 0; i < 32; i += 8) back = (back ^ ((value >> i) & 0xFF)) * 16777619u;
        fclose(file);
    }
    saved = (!file || back != hash) ? "SAVE FAILED" : where[0] == 's' ? "SAVED SD:" : where[0] == 'f' ? "SAVED FAT:" : "SAVED";
}

static int runArm7(unsigned command) {
    // Ask the ARM7 to run tests, waiting about 10 seconds of frames for it; its part of the block is written without
    // going through this CPU's cache, so the cache has to forget it before looking
    armDCacheFlush(&block, sizeof(block));
    shared->done7 = 0;
    armDCacheFlush(&shared->progress7, 32);
    pxiSend(PxiChannel_User0, COMMAND(command, shared));
    for (int frames = 0; frames < 600; frames++) {
        while (VCOUNT != 100) {
            armDCacheInvalidate(&shared->progress7, 32);
            if (shared->done7 == command) {
                armDCacheInvalidate(shared->results[1], sizeof(shared->results[1]));
                return 1;
            }
        }
        while (VCOUNT == 100);
    }
    armDCacheInvalidate(shared->results[1], sizeof(shared->results[1]));
    return 0;
}

int main(void) {
    *(volatile unsigned*)0x04000304 = 0x8003;
    *(volatile unsigned char*)0x04000240 = 0x80;
    *(volatile unsigned*)0x04000000 = 0x00020000;
    for (int i = 0; i < 256 * 192; i++) SCREEN[i] = BLACK;
    hasExpected = expected[0][0][0] != 0x4E4F4E45;
    shared = &block;
    shared->magic[0] = SHARED_MAGIC0;
    shared->magic[1] = SHARED_MAGIC1;
    pxiWaitRemote(PxiChannel_User0);

    textRow = 0;
    textColumn = 0;
    text("NOODS U INSTRUCTION TEST", WHITE);
    newLine();
    runNormal(0);

    textRow = 2;
    textColumn = 0;
    text("ARM7 RUNNING", YELLOW);
    newLine();
    stuck = !runArm7(1);
    ranCount = normalCount;
    buildLines();
    saveResults();
    showResults();

    unsigned short held = 0xFFFF;
    while (1) {
        while (VCOUNT != 192);
        while (VCOUNT == 192);
        unsigned short now = KEYS & 0x3FF, pressed = held & ~now;
        held = now;
        int redraw = 1;
        if (pressed & BIT_KEY(6)) scroll = scroll > 0 ? scroll - 1 : 0;
        else if (pressed & BIT_KEY(7)) scroll = scroll + 1 < lineCount ? scroll + 1 : scroll;
        else if (pressed & BIT_KEY(5)) scroll = scroll > 19 ? scroll - 19 : 0;
        else if (pressed & BIT_KEY(4)) scroll = scroll + 19 < lineCount ? scroll + 19 : scroll;
        else if (pressed & (BIT_KEY(8) | BIT_KEY(9))) { shownCpu = !shownCpu; scroll = 0; buildLines(); }
        else if (pressed & BIT_KEY(0)) { showAll = !showAll; scroll = 0; buildLines(); }
        else if ((pressed & BIT_KEY(3)) && nextRisky < totalCount - normalCount) {
            // One at a time, so a crash can only be from this test, and the other CPU's results are still safe
            runRisky(0, nextRisky);
            textRow = 2;
            textColumn = 0;
            text("ARM7 RUNNING RISKY", YELLOW);
            newLine();
            stuck = stuck || !runArm7(nextRisky + 2);
            nextRisky++;
            ranCount = normalCount + nextRisky;
            scroll = lineCount;
            buildLines();
            scroll = lineCount > 10 ? lineCount - 10 : 0;
            saveResults();
        }
        else redraw = 0;
        if (redraw) showResults();
    }
}
