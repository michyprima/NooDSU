// What the DSi microphone test's CPUs share: a block of words in main RAM with each test's results, and the list of tests
// Only the ARM7 has the microphone, so the ARM9 asks it for one test per message; mic.py explains the results

#define M_MAGIC0 0 // "MIC "
#define M_MAGIC1 1 // "TEST"
#define M_STATUS 2 // What happened, from the STATUS_ values
#define M_STEP 3 // The test the ARM7 was asked for
#define M_DONE7 4 // The last command the ARM7 finished, with its count in the low bits
#define M_BUFFER7 5 // Where the ARM7's tests put NDMA data
#define M_SCFG7 6 // SCFG_EXT as the ARM7 found it
#define M_RESULTS 8
#define RESULT_WORDS 32

#define MIC_MAGIC0 0x2043494D // MIC
#define MIC_MAGIC1 0x54534554 // TEST

#define STATUS_DONE 1
#define STATUS_NO_ARM7 2 // The ARM7 didn't answer
#define STATUS_NOT_DSI 3 // Not running in DSi mode with the microphone on

#define BUSY 0xDEAD0002 // First result word of a test whose channel or timer was already in use
#define TIMEOUT 0xDEAD0003 // A result the test gave up waiting for
#define BUFFER_WORDS 64
#define CHANNEL 2 // NDMA channel used, since libnds's SD card access starts from the lowest

// Each test's result words are described in mic_tests.c. TL_ tests are timelines: format, divider, 47kHz or not, IRQ mode
#define TESTS(X) \
    X(REGS) \
    X(TL_F2_D0) X(TL_F0_D0) X(TL_F1_D0) X(TL_F3_D0) X(TL_F2_D1) X(TL_F2_D2) X(TL_F2_D3) X(TL_F2_D0_47) X(TL_F0_D1_47) \
    X(TL_IRQ1) X(TL_IRQ2) X(TL_IRQ3) X(TL_SNDEX_OFF) \
    X(IRQ_EDGE) X(READ_SIZES) X(KEEP_CLEAR) X(START_DELAY) \
    X(NDMA_8) X(NDMA_4) X(NDMA_16) X(NDMA_1) X(NDMA_LATE) X(NDMA_OVERRUN) \
    X(MUTED) X(UNMUTED)

enum {
#define X(name) T_##name,
    TESTS(X)
#undef X
    TEST_COUNT
};

#define M_TOTAL (M_RESULTS + TEST_COUNT * RESULT_WORDS)

void runTest(int test, unsigned *out, volatile unsigned *buffer);
