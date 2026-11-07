// The ARM7's side: libnds's usual services, which give the ARM9 the SD card, and a thread that runs tests when asked
#include <nds.h>
#include "list.h"
void runNormal(int cpu);
void runRisky(int cpu, int index);
void onTest(int cpu, int index) {}

static Thread testThread;
alignas(8) static u8 testStack[2048];

static int runTests(void *arg) {
    Mailbox mailbox;
    u32 slots[4];
    mailboxPrepare(&mailbox, slots, sizeof(slots) / 4);
    pxiSetMailbox(PxiChannel_User0, &mailbox);
    while (1) {
        u32 message = mailboxRecv(&mailbox);
        unsigned command = message >> 22;
        shared = (struct Shared*)(0x02000000 + ((message & 0x3FFFFF) << 2));
        if (command == 1) runNormal(1);
        else runRisky(1, command - 2);
        shared->done7 = command;
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
