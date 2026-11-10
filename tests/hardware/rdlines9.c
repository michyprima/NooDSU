// Settles how long the DS's 3D engine takes to render, through what it reports: RDLINES_COUNT, the fewest lines its
// 48-line buffer held during a frame, and DISP3DCNT bit 12, set when the buffer ran dry. Each scene in rdlines.h is
// swapped in after an empty one and watched line by line; a few are captured to show what an overloaded frame looks like
#include <stdio.h>
#include <string.h>
#include <nds.h>
#include "screen.h"
#include "rdlines.h"

#define IO(a) (*(volatile unsigned*)(a))
#define IO16(a) (*(volatile unsigned short*)(a))
#define IO8(a) (*(volatile unsigned char*)(a))
#define LINE IO16(0x04000006)
#define DISP3DCNT IO16(0x04000060)
#define DISPCAPCNT IO(0x04000064)
#define RDLINES IO8(0x04000320)
#define GXSTAT IO(0x04000600)
#define RAM_COUNT IO(0x04000604)
#define MTX_MODE IO(0x04000440)
#define MTX_IDENTITY IO(0x04000454)
#define MTX_LOAD_4X4 IO(0x04000458)
#define COLOR IO(0x04000480)
#define VTX_16 IO(0x0400048C)
#define POLYGON_ATTR IO(0x040004A4)
#define BEGIN_VTXS IO(0x04000500)
#define END_VTXS IO(0x04000504)
#define SWAP_BUFFERS IO(0x04000540)
#define VIEWPORT IO(0x04000580)
#define CLEAR_COLOR IO(0x04000350)
#define CLEAR_DEPTH IO16(0x04000354)

static unsigned block[R_TOTAL] __attribute__((aligned(32)));

static const struct { const char *name; int n, w, y0, y1, cap; } scenes[] = {
#define X(name, n, w, y0, y1, cap) { #name, n, w, y0, y1, cap },
    SCENES(X)
#undef X
};

static void nextFrame(void) { while (LINE == 192); while (LINE != 192); }

static void vertex(int px, int py, int z) {
    // The projection matrix makes W 1.5, which puts pixel edges on whole vertex units: X is (px - 128) * 48 and Y is
    // (96 - py) * 64
    VTX_16 = (unsigned)((96 - py) * 64) << 16 | (unsigned short)((px - 128) * 48);
    VTX_16 = (unsigned short)z;
}

static void sendScene(int n, int w, int y0, int y1) {
    // Quads from the left edge, each a colour that says which it is (index + 1 in red and green, blue full) and nearer
    // than the one before, so the last one drawn shows
    MTX_MODE = 0;
    static const unsigned projection[16] = { 0x1000, 0, 0, 0, 0, 0x1000, 0, 0, 0, 0, 0x1000, 0, 0, 0, 0, 0x1800 };
    for (int i = 0; i < 16; i++) MTX_LOAD_4X4 = projection[i];
    MTX_MODE = 2;
    MTX_IDENTITY = 0;
    VIEWPORT = 0xBFFF0000;
    POLYGON_ATTR = 0x001F00C0;
    BEGIN_VTXS = 1;
    for (int i = 0; i < n; i++) {
        unsigned id = i + 1;
        COLOR = (id & 31) | ((id >> 5) & 31) << 5 | 31 << 10;
        int z = 4000 - i * 2;
        vertex(0, y0, z);
        vertex(w, y0, z);
        vertex(w, y1, z);
        vertex(0, y1, z);
    }
    END_VTXS = 0;
}

static void swapAndWait(void) {
    // The geometry engine stays busy from SWAP_BUFFERS until the swap at the next V-blank
    SWAP_BUFFERS = 0;
    while (GXSTAT & (1 << 27));
}

static void watch(unsigned *out) {
    // Line by line for a few frames after the swap: every change of RDLINES_COUNT, and every time bit 12 is found
    // set, which is then acknowledged. Frames count from the swap, going up at the start of each V-blank
    int changes = 0, flags = 0, frame = 0;
    unsigned line = LINE, value = out[W_BEFORE];
    while (frame < WATCH_FRAMES) {
        unsigned now = LINE;
        if (now != line) {
            line = now;
            if (line == 192) frame++;
        }
        unsigned rd = RDLINES;
        if (rd != value) {
            value = rd;
            if (changes < 4) out[W_CHANGES + changes++] = frame << 24 | line << 8 | rd;
        }
        if (DISP3DCNT & (1 << 12)) {
            DISP3DCNT = 1 << 12;
            if (flags < 4) out[W_FLAGS + flags++] = frame << 16 | line;
        }
    }
}

static void capture(unsigned *out) {
    // Capture only the 3D, 256x192, into VRAM D, starting at the next frame
    nextFrame();
    DISPCAPCNT = 1u << 31 | 1 << 24 | 3 << 20 | 3 << 16 | 16;
    while (DISPCAPCNT & (1u << 31));
    memcpy(out, (void*)0x06860000, CAPTURE_WORDS * 4);
}

static void measure(int s, int *captures) {
    unsigned *out = &block[R_RESULTS + s * RESULT_WORDS];
    for (int i = 0; i < RESULT_WORDS; i++) out[i] = NO_EVENT;
    block[R_STEP] = s;

    // Show nothing for a few frames, so every scene starts from the same state
    sendScene(0, 0, 0, 0);
    swapAndWait();
    nextFrame();
    nextFrame();
    nextFrame();
    DISP3DCNT = 1 << 12;
    out[W_BEFORE] = RDLINES;

    // Send the scene and watch from the swap on, with interrupts off so no line is missed
    ArmIrqState irqs = armIrqLockByPsr();
    sendScene(scenes[s].n, scenes[s].w, scenes[s].y0, scenes[s].y1);
    while (GXSTAT & (1 << 27));
    out[W_RAMCOUNT] = RAM_COUNT;
    swapAndWait();
    watch(out);
    armIrqUnlockByPsr(irqs);
    out[W_STEADY] = RDLINES;
    out[W_DISP3DCNT] = DISP3DCNT;
    if (scenes[s].cap && *captures < CAPTURES)
        capture(&block[R_CAPTURES + (*captures)++ * CAPTURE_WORDS]);
}

static void show(int s) {
    // Three columns of the steady RDLINES_COUNT, with F if bit 12 was ever set
    unsigned *out = &block[R_RESULTS + s * RESULT_WORDS];
    textRow = 5 + s % 19;
    textColumn = (s / 19) * 14;
    text(scenes[s].name, WHITE);
    while (textColumn % 14 < 8) text(" ", WHITE);
    number(out[W_STEADY], out[W_STEADY] < 46 ? YELLOW : GREEN);
    text(out[W_FLAGS] != NO_EVENT ? " F" : "  ", RED);
}

int main(void) {
    // 3D on the main engine, shown on the top screen, and text on the sub engine at the bottom
    IO(0x04000304) = 0x820F;
    screenInitSub();
    IO8(0x04000243) = 0x80; // VRAM D for captures
    IO(0x04000000) = 0x00010108; // Graphics, BG0 on and 3D
    DISP3DCNT = 1 << 12 | 1 << 13;
    CLEAR_COLOR = 0;
    CLEAR_DEPTH = 0x7FFF;
    block[R_MAGIC0] = RDLN_MAGIC0;
    block[R_MAGIC1] = RDLN_MAGIC1;
    block[R_SCFG9] = IO(0x04004008);

    textRow = textColumn = 0;
    text("NOODS U 3D TIMING TEST", WHITE);
    newLine();
    text("RUNNING", YELLOW);
    newLine();
    newLine();
    textRow = 3;
    text("RDLINES AT THE END, F IF IT RAN DRY", WHITE);
    newLine();

    int captures = 0;
    for (int s = 0; s < SCENE_COUNT; s++) {
        textRow = 2;
        textColumn = 0;
        text("SCENE ", YELLOW);
        number(s, YELLOW);
        newLine();
        measure(s, &captures);
        show(s);
    }
    sendScene(0, 0, 0, 0);
    swapAndWait();
    block[R_STATUS] = STATUS_DONE;

    // Save the whole block, which tests/hardware/rdlines.py explains
    textRow = 1;
    textColumn = 0;
    text("DONE, SAVING", YELLOW);
    newLine();
    const char *where = "";
    FILE *file = openSave("noodsu-rdlinestest.bin", &where);
    int good = file && fwrite(block, 4, R_TOTAL, file) == R_TOTAL;
    if (file && fclose(file) != 0) good = 0;
    textRow = 1;
    textColumn = 0;
    text(!file ? "DONE, NOT SAVED" : !good ? "DONE, SAVE FAILED" : "DONE, SAVED ", good ? GREEN : RED);
    if (good) text(where, GREEN);
    newLine();
    while (1) threadWaitForVBlank();
}
