#include <calico.h>
#include "list.h"
extern void run_test(void (*test)(void), volatile unsigned *results);
extern void irq_escape(void), exception_caught(void);
void onTest(int cpu, int index);
#define T(f, n) extern void f(void);
NORMAL_TESTS RISKY_TESTS
#undef T
#define T(f, n) f,
static void (*const normal[])(void) = { NORMAL_TESTS };
static void (*const risky[])(void) = { RISKY_TESTS };
#undef T

struct Shared *shared;

// Data the tests read and write, the same every time: 00 01 02 ... from an aligned address
unsigned test_data[64];

#ifdef ARM9
// The BIOS takes interrupts through the end of DTCM, and exceptions through a pointer near the end of main RAM; that's
// 0x27FFD9C on a DS, which mirrors this, and this on a DSi in DSi mode
#define IRQ_VECTOR ((volatile unsigned*)((dtcmBase() & ~0xFFF) + 0x3FFC))
#define EXCEPTION_VECTOR ((volatile unsigned*)0x02FFFD9C)
static unsigned dtcmBase(void) { unsigned r; __asm__ volatile("mrc p15, 0, %0, c9, c1, 0" : "=r"(r)); return r; }
static unsigned getControl(void) { unsigned r; __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(r)); return r; }
static void setControl(unsigned r) { __asm__ volatile("mcr p15, 0, %0, c1, c0, 0" :: "r"(r)); }
static unsigned getProcessId(void) { unsigned r; __asm__ volatile("mrc p15, 0, %0, c13, c0, 1" : "=r"(r)); return r; }
static void setProcessId(unsigned r) { __asm__ volatile("mcr p15, 0, %0, c13, c0, 1" :: "r"(r)); }
#else
#define IRQ_VECTOR ((volatile unsigned*)0x0380FFFC)
#define EXCEPTION_VECTOR ((volatile unsigned*)0x0380FFDC)
#endif

static void runGuarded(void (*test)(void), volatile unsigned *out) {
    // Lend a test the interrupts and exceptions, which go through the BIOS to the test's own handlers like they did
    // before the ROM used libnds, then give them back to libnds as they were
    ArmIrqState psr = armIrqLockByPsr();
    unsigned ime = REG_IME;
    REG_IME = 0;
    unsigned ie = REG_IE;
    volatile unsigned *irqVector = IRQ_VECTOR, *exceptionVector = EXCEPTION_VECTOR;
    unsigned oldIrq = *irqVector, oldException = *exceptionVector;
    *irqVector = (unsigned)irq_escape;
    *exceptionVector = (unsigned)exception_caught;
#ifdef ARM9
    unsigned control = getControl(), processId = getProcessId(); // Some tests write the trace process ID, which libnds sets
    setControl(control | (1 << 13)); // Vectors in the BIOS, not libnds's in ITCM
#endif
    run_test(test, out);
#ifdef ARM9
    setControl(control);
    setProcessId(processId);
#endif
    *irqVector = oldIrq;
    *exceptionVector = oldException;
    REG_IE = ie;
    REG_IME = ime;
    armIrqUnlockByPsr(psr);
}

static void runAll(int cpu, void (*const *tests)(void), int count, int start) {
    for (int i = 0; i < count; i++) {
        if (cpu) shared->progress7 = start + i + 1;
        else shared->progress9[0] = start + i + 1;
        onTest(cpu, start + i);
        for (int j = 0; j < 64; j++)
            test_data[j] = (((j * 4 + 3) & 0xFF) << 24) | (((j * 4 + 2) & 0xFF) << 16) | (((j * 4 + 1) & 0xFF) << 8) | ((j * 4) & 0xFF);
        volatile unsigned *out = RESULTS(cpu) + (start + i) * WORDS;
        for (int j = 0; j < WORDS; j++) out[j] = 0;
        runGuarded(tests[i], out);
        for (int j = 0; j < 6; j++) out[6 + j] = test_data[j];
    }
}

void runNormal(int cpu) { runAll(cpu, normal, sizeof(normal) / sizeof(normal[0]), 0); }
void runRisky(int cpu, int index) { runAll(cpu, &risky[index], 1, sizeof(normal) / sizeof(normal[0]) + index); }
