// The ARM7's side of the speed test: libnds's usual services, which give the ARM9 the SD card, and a thread that takes
// the ARM7's measurements when the ARM9 asks, as the shared WRAM test's ARM7 does
#include <nds.h>
#include "speed.h"

unsigned speed_time7(void (*function)(unsigned), unsigned argument);
void speed_bios7(unsigned count);
#define PLACE(name) \
    void speed_empty_##name(unsigned), speed_loop_##name(unsigned), speed_load_##name(unsigned), speed_store_##name(unsigned);
PLACE(wram) PLACE(twl) PLACE(main)
#undef PLACE

static void (*const functions[PLACES][KINDS])(unsigned) = {
    { speed_empty_wram, speed_loop_wram, speed_load_wram, speed_store_wram },
    { speed_empty_twl, speed_loop_twl, speed_load_twl, speed_store_twl },
    { speed_empty_main, speed_loop_main, speed_load_main, speed_store_main },
};

static Thread testThread;
alignas(8) static u8 testStack[2048];

static void measure(volatile unsigned *out, void (*function)(unsigned)) {
    for (int i = 0; i < REPEATS; i++) {
        ArmIrqState psr = armIrqLockByPsr();
        unsigned ime = REG_IME;
        REG_IME = 0;
        out[i] = speed_time7(function, LOOPS);
        REG_IME = ime;
        armIrqUnlockByPsr(psr);
    }
}

static void run(volatile unsigned *block) {
    block[S_SCFG7] = *(volatile unsigned short*)0x04004004; // SCFG_CLK
    block[S_SCFG7 + 1] = *(volatile unsigned*)0x04004008; // SCFG_EXT
    for (int place = 0; place < PLACES; place++) {
        // New WRAM only holds code in DSi mode
        int there = place != PLACE_TWL || isDSiMode();
        block[S_ADDR7 + place] = there ? (unsigned)functions[place][KIND_LOOP] : NOT_RUN;
        for (int kind = 0; kind < KINDS; kind++) {
            volatile unsigned *out = &block[S_RESULTS7 + (place * KINDS + kind) * REPEATS];
            if (there) measure(out, functions[place][kind]);
            else for (int i = 0; i < REPEATS; i++) out[i] = NOT_RUN;
        }
    }
    measure(&block[S_RESULTS7 + M7_BIOS * REPEATS], speed_bios7);
}

static int runTest(void *arg) {
    Mailbox mailbox;
    u32 slots[4];
    mailboxPrepare(&mailbox, slots, sizeof(slots) / 4);
    pxiSetMailbox(PxiChannel_User0, &mailbox);
    while (1) {
        // The command, a count so each answer is told apart, and where the block is
        u32 message = mailboxRecv(&mailbox);
        volatile unsigned *block = (volatile unsigned*)(0x02000000 + ((message & 0x3FFFF) << 2));
        if (message >> 22 == COMMAND_RUN) run(block);
        block[S_DONE7] = message >> 18;
    }
    return 0;
}

int main(void) {
    // As devkitPro's default ARM7, without sound, the microphone, Wi-Fi or Maxmod
    envReadNvramSettings();
    keypadStartExtServer();
    lcdSetIrqMask(DISPSTAT_IE_ALL, DISPSTAT_IE_VBLANK);
    irqEnable(IRQ_VBLANK);
    rtcInit();
    rtcSyncTime();
    pmInit();
    blkInit();
    touchInit();
    touchStartServer(80, MAIN_THREAD_PRIO);

    threadPrepare(&testThread, runTest, NULL, &testStack[sizeof(testStack)], MAIN_THREAD_PRIO);
    threadStart(&testThread);
    while (pmMainLoop()) threadWaitForVBlank();
    return 0;
}
