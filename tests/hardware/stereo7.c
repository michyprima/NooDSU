// The ARM7's side of the stereo test, which makes the sound: a low tone on the left, then a high one on the right, over
// and over, or both while A is held. It sets the sound registers itself, with nothing between it and the mixer
#include <nds.h>

int main(void) {
    // As devkitPro's default ARM7, without its sound server, the microphone, Wi-Fi or Maxmod
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

    // Channels 8 and 9 make square waves; a pan of 0 is all the way left and 127 all the way right
    REG_POWCNT |= POWCNT_SOUND;
    REG_SOUNDCNT = SOUNDCNT_ENABLE | SOUNDCNT_VOL(127);
    soundChPreparePsg(8, 100, SoundVolDiv_1, 0, soundTimerFromHz(330 * 8), SoundDuty_50);
    soundChPreparePsg(9, 100, SoundVolDiv_1, 127, soundTimerFromHz(880 * 8), SoundDuty_50);

    // A second of each with a short gap, timed by frames
    int playing = -1;
    for (unsigned frame = 0; pmMainLoop(); frame++) {
        int phase = frame % 150, now = (phase < 60) ? 0 : (phase >= 75 && phase < 135) ? 1 : -1;
        if (!(REG_KEYINPUT & KEY_A)) now = 2;
        if (now != playing) {
            (now == 0 || now == 2) ? soundChStart(8) : soundChStop(8);
            (now == 1 || now == 2) ? soundChStart(9) : soundChStop(9);
            playing = now;
        }
        threadWaitForVBlank();
    }
    return 0;
}
