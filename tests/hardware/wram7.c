// The ARM7's side of the shared WRAM test: libnds's usual services, which give the ARM9 the SD card, and a thread that
// does what the ARM9 asks, one short command per message, as the instruction test's ARM7 does
#include <string.h>
#include <nds.h>
#include "wram.h"
void wram_look7(volatile unsigned *block);

static Thread testThread;
alignas(8) static u8 testStack[2048];

static void run(unsigned command, volatile unsigned *block) {
    switch (command) {
        case COMMAND_INIT:
            block[W_INIT7] = *(volatile unsigned*)0x04004060; // MBK9
            block[W_INIT7 + 1] = *(volatile unsigned*)0x04004008; // SCFG_EXT7
            block[W_INIT7 + 2] = *(volatile unsigned char*)0x04000241; // WRAMSTAT
            for (int i = 0; i < 3; i++) block[W_INIT7 + 3 + i] = ((volatile unsigned*)0x04004054)[i]; // MBK6-8
            break;
        case COMMAND_BACKUP:
            memcpy((void*)block[W_BACKUP], (void*)SHARED_ADDRESS, 0x8000);
            break;
        case COMMAND_MBK9:
            *(volatile unsigned*)0x04004060 = block[W_PARAM];
            block[W_MBK9_BACK] = *(volatile unsigned*)0x04004060;
            break;
        case COMMAND_LOOK: {
            ArmIrqState psr = armIrqLockByPsr();
            unsigned ime = REG_IME;
            REG_IME = 0;
            wram_look7(block);
            REG_IME = ime;
            armIrqUnlockByPsr(psr);
            break;
        }
        case COMMAND_RESTORE:
            memcpy((void*)SHARED_ADDRESS, (void*)block[W_BACKUP], 0x8000);
            break;
    }
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
        run(message >> 22, block);
        block[W_DONE7] = message >> 18;
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
