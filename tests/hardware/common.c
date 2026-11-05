#include "list.h"
extern void run_test(void (*test)(void), volatile unsigned *results);
void onTest(int cpu, int index);
#define T(f, n) extern void f(void);
NORMAL_TESTS RISKY_TESTS
#undef T
#define T(f, n) f,
static void (*const normal[])(void) = { NORMAL_TESTS };
static void (*const risky[])(void) = { RISKY_TESTS };
#undef T

// Data the tests read and write, the same every time: 00 01 02 ... from an aligned address
unsigned test_data[64];

static void runAll(int cpu, void (*const *tests)(void), int count, int start) {
    for (int i = 0; i < count; i++) {
        PROGRESS(cpu) = start + i + 1;
        onTest(cpu, start + i);
        for (int j = 0; j < 64; j++)
            test_data[j] = (((j * 4 + 3) & 0xFF) << 24) | (((j * 4 + 2) & 0xFF) << 16) | (((j * 4 + 1) & 0xFF) << 8) | ((j * 4) & 0xFF);
        volatile unsigned *out = RESULTS(cpu) + (start + i) * WORDS;
        for (int j = 0; j < WORDS; j++) out[j] = 0;
        run_test(tests[i], out);
        for (int j = 0; j < 6; j++) out[6 + j] = test_data[j];
    }
}

void runNormal(int cpu) { runAll(cpu, normal, sizeof(normal) / sizeof(normal[0]), 0); }
void runRisky(int cpu, int index) { runAll(cpu, &risky[index], 1, sizeof(normal) / sizeof(normal[0]) + index); }
