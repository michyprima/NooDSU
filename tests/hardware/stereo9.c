// Tells left from right by ear: the ARM7 plays a low tone that should only come from the left speaker, then a high one
// that should only come from the right, over and over, and both while A is held. This side only says so
#include <nds.h>
#include "screen.h"

static void line(int row, const char *s, unsigned short color) {
    textRow = row;
    textColumn = 0;
    text(s, color);
}

int main(void) {
    screenInit();
    line(0, "NOODS U STEREO TEST", WHITE);
    line(2, "LOW TONE:  LEFT SPEAKER ONLY", GREEN);
    line(4, "HIGH TONE: RIGHT SPEAKER ONLY", YELLOW);
    line(6, "THEY TAKE TURNS, A SECOND EACH", WHITE);
    line(8, "HOLD A FOR BOTH TOGETHER", WHITE);
    while (pmMainLoop()) threadWaitForVBlank();
    return 0;
}
