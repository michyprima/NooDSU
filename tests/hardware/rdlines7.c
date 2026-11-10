// The ARM7's side of the 3D timing test: only libnds's usual services, which give the ARM9 the SD card
#include <nds.h>

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
    while (pmMainLoop()) threadWaitForVBlank();
    return 0;
}
