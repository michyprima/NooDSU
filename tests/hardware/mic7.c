// The ARM7's side of the DSi microphone test: libnds's usual services, which give the ARM9 the SD card, and a thread that
// runs one test per message, as the NDMA test's ARM7 does
#include <nds.h>
#include "mic.h"

static Thread testThread;
alignas(8) static u8 testStack[2048];

static int runTests(void *arg) {
    Mailbox mailbox;
    u32 slots[4];
    mailboxPrepare(&mailbox, slots, sizeof(slots) / 4);
    pxiSetMailbox(PxiChannel_User0, &mailbox);
    while (1) {
        // The test plus one (0 only records SCFG_EXT), a count so each answer is told apart, and where the block is
        u32 message = mailboxRecv(&mailbox);
        volatile unsigned *block = (volatile unsigned*)(0x02000000 + ((message & 0x3FFFF) << 2));
        unsigned command = message >> 20; // Messages are 26 bits
        if (command == 0) block[M_SCFG7] = *(volatile unsigned*)0x04004008;
        else runTest(command - 1, (unsigned*)&block[M_RESULTS + (command - 1) * RESULT_WORDS], (volatile unsigned*)block[M_BUFFER7]);
        block[M_DONE7] = message >> 18;
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

    threadPrepare(&testThread, runTests, NULL, &testStack[sizeof(testStack)], MAIN_THREAD_PRIO);
    threadStart(&testThread);
    while (pmMainLoop()) threadWaitForVBlank();
    return 0;
}
