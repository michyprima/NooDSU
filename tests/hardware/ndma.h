// What the NDMA test's CPUs share: a block of words in main RAM with each test's results, and the list of tests
// Both CPUs run the same tests from ndma_tests.c, one per message for the ARM7, and ndma.py explains the results

#define N_MAGIC0 0 // "NDMA"
#define N_MAGIC1 1 // "TEST"
#define N_STATUS 2 // What happened, from the STATUS_ values
#define N_STEP 3 // The test the ARM9 is on
#define N_DONE7 4 // The last command the ARM7 finished, with its count in the low bits
#define N_BUFFER7 5 // Where the ARM7's tests put data
#define N_SCFG9 6 // SCFG_EXT as the ARM9 found it
#define N_SCFG7 7 // SCFG_EXT as the ARM7 found it
#define N_RESULTS 8
#define RESULT_WORDS 8

#define NDMA_MAGIC0 0x414D444E // NDMA
#define NDMA_MAGIC1 0x54534554 // TEST

#define STATUS_DONE 1
#define STATUS_NO_ARM7 2 // The ARM7 didn't answer
#define STATUS_NOT_DSI 3 // Not running in DSi mode with NDMA on

#define SKIPPED 0xDEAD0001 // First result word of a test that doesn't apply to this CPU
#define BUSY 0xDEAD0002 // First result word of a test whose channel or timer was already in use
#define BUFFER_WORDS 2048
#define CHANNEL 2 // Channel used, since libnds's SD card access starts from the lowest

// Each test's result words are described in ndma_tests.c
#define TESTS(X) \
    X(REGS) X(REGS_ENABLED) X(REGS_ENABLED2) X(IMMEDIATE) X(IMMEDIATE_1F) X(CHANNELS) \
    X(DST_DEC) X(DST_FIXED) X(DST_RESERVED) X(SRC_DEC) X(SRC_FIXED) X(FILL) \
    X(UNEVEN_TOTAL) X(RERUN) X(BLOCK_0) X(TOTAL_0) X(INFINITE) X(RESTART) X(NO_IRQ) \
    X(RELOAD_DST) X(RELOAD_SRC) \
    X(LINES_VBLANK) X(LINES_7) X(LINES_8) X(LINES_5) X(LINES_9) X(LINES_B) X(LINES_C) X(LINES_D) X(LINES_E) X(LINES_F) \
    X(TIMER0) X(TIMER1) X(TIMER2) X(TIMER3) X(OTHER_TIMER) X(CASCADE) \
    X(GX) X(GX_FULL)

enum {
#define X(name) T_##name,
    TESTS(X)
#undef X
    TEST_COUNT
};

#define N_TOTAL (N_RESULTS + 2 * TEST_COUNT * RESULT_WORDS)

void runTest(int test, unsigned *out, volatile unsigned *buffer);
