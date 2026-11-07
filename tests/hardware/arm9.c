// Runs the tests on both CPUs and shows where this DS differs from what NooDS U does
#include <stdio.h>
#include <nds.h>
#include <fat.h>
#include "list.h"
#include "screen.h"
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
    FILE *file = openSave("noodsu-hwtest.bin", &where);
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
    screenInit();
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
