// Replays one frame's polygons with their exact clip coordinates, captures what the 3D engine really draws, and shows and saves it
#include "../rom/rom.h"
#include "../rom/font.h"
#include "scene.h"

#define DISPCNT REG32(0x04000000)
#define DISPCAPCNT REG32(0x04000064)
#define DISP3DCNT REG16(0x04000060)
#define VCOUNT REG16(0x04000006)
#define KEYS REG16(0x04000130)
#define SCREEN ((volatile unsigned short*)0x06800000)
#define CAPTURE ((volatile unsigned short*)0x06820000)
#define IMAGES ((unsigned short*)0x02200000) // Kept in main RAM, where a run in NooDS U can be read back too
#define STATUS REG32(0x021FFFF0)

#define WHITE 0xFFFF
#define BLACK 0x8000
#define YELLOW 0x83FF
#define RED 0x801F
#define GREEN 0x83E0

#define BIT4 (1 << 4)
#define SCENES 4
static const struct { const char *name; unsigned short disp; int lining; } scenes[SCENES] = {
    { "AS IN THE GAME", SCENE_DISP3DCNT, 0 },
    { "ANTI-ALIASING OFF", SCENE_DISP3DCNT & ~BIT4, 0 },
    { "ONLY ANTI-ALIASING", (SCENE_DISP3DCNT & 0xF) | BIT4, 0 },
    { "BLACK OVER WHITE", (SCENE_DISP3DCNT & 0xF) | BIT4, 1 },
};
static int streaks[SCENES], streakX[SCENES], streakY[SCENES];

static int textX, textY;
static void text(const char *s, unsigned short color) {
    for (; *s; s++, textX += 6) {
        char c = *s;
        if (c < 32 || c > 90) c = '?';
        for (int x = 0; x < 6; x++) {
            int bits = x < 5 ? font[c - 32][x] : 0;
            for (int y = 0; y < 8; y++)
                SCREEN[(textY + y) * 256 + textX + x] = (bits >> y) & 1 ? color : BLACK;
        }
    }
}
static void number(int value, unsigned short color) {
    char s[12];
    int n = sizeof(s) - 1;
    s[n] = 0;
    do { s[--n] = '0' + value % 10; value /= 10; } while (value);
    text(&s[n], color);
}
static void line(int y) { textX = 0; textY = y * 8; }

static void waitVblank(void) {
    while (VCOUNT == 192);
    while (VCOUNT != 192);
}

// A vertex at the origin takes the matrix's last row as its clip coordinates, with no rounding
static void vertex(unsigned x, unsigned y, unsigned z, unsigned w, unsigned color) {
    static const int identity[12] = { 0x1000, 0, 0, 0, 0, 0x1000, 0, 0, 0, 0, 0x1000, 0 };
    REG32(0x04000480) = color;
    for (int i = 0; i < 12; i++) REG32(0x04000458) = identity[i];
    REG32(0x04000458) = x;
    REG32(0x04000458) = y;
    REG32(0x04000458) = z;
    REG32(0x04000458) = w;
    REG32(0x0400048C) = 0;
    REG32(0x0400048C) = 0;
}

static unsigned toRgb5(unsigned c) { return ((c & 0x3F) >> 1) | (((c >> 6) & 0x3F) >> 1) << 5 | (((c >> 12) & 0x3F) >> 1) << 10; }

static int isBlack(int i) {
    for (int j = 0; j < (int)scene[i].size; j++)
        if (scene[i].v[j * 5 + 4] & 0x3FFFF) return 0;
    return 1;
}

static void polygon(unsigned attr, int size) {
    REG32(0x040004A4) = attr;
    REG32(0x040004A8) = 0; // No texture
    REG32(0x04000500) = size == 3 ? 0 : 1;
}

static void drawScene(int which) {
    // The attributes keep the fog flag, polygon ID and far plane and 1-dot bits; lights and modes aren't replayed
    DISP3DCNT = scenes[which].disp;
    REG32(0x04000350) = SCENE_CLEAR_COLOR;
    REG32(0x04000354) = SCENE_CLEAR_DEPTH;
    REG32(0x04000358) = SCENE_FOG_COLOR;
    REG16(0x0400035C) = SCENE_FOG_OFFSET;
    for (int i = 0; i < 32; i++) *(volatile unsigned char*)(0x04000360 + i) = sceneFog[i];
    for (int i = 0; i < 8; i++) REG16(0x04000330 + i * 2) = sceneEdge[i];

    REG32(0x04000580) = (191 << 24) | (255 << 16); // Viewport
    REG32(0x04000440) = 0;
    REG32(0x04000454) = 0;
    REG32(0x04000440) = 1;

    if (scenes[which].lining) {
        // A plain white wall where the game's world is, behind all of the black polygons
        const unsigned w = 0x4A000, z = 0x40000;
        polygon(0x001F00C0, 4);
        vertex(-w, -w, z, w, 0x7FFF);
        vertex(w, -w, z, w, 0x7FFF);
        vertex(w, w, z, w, 0x7FFF);
        vertex(-w, w, z, w, 0x7FFF);
        REG32(0x04000504) = 0;
    }
    for (int i = 0; i < SCENE_POLYGONS; i++) {
        if (scenes[which].lining && !isBlack(i)) continue;
        polygon((scene[i].attr & ~0x3F) | 0xC0, scene[i].size);
        for (int j = 0; j < (int)scene[i].size; j++) {
            const unsigned *v = &scene[i].v[j * 5];
            vertex(v[0], v[1], v[2], v[3], toRgb5(v[4]));
        }
        REG32(0x04000504) = 0;
    }
    REG32(0x04000540) = 2; // Swap buffers, with W-buffering as the game has it
}

static int brightness(unsigned short c) { return (c & 0x1F) + ((c >> 5) & 0x1F) + ((c >> 10) & 0x1F); }

// Finds lines one pixel high that are bright between dark rows, the same way the Mac harness does
static void findStreaks(int which, const unsigned short *image) {
    for (int y = 2; y < 190; y++) {
        int run = 0;
        for (int x = 0; x <= 256; x++) {
            int bright = x < 256 && brightness(image[y * 256 + x]) >= 5 &&
                brightness(image[(y - 1) * 256 + x]) <= 3 && brightness(image[(y + 1) * 256 + x]) <= 3;
            if (bright) {
                run++;
                continue;
            }
            if (run >= 4) {
                if (!streaks[which]) {
                    streakX[which] = x - run;
                    streakY[which] = y;
                }
                streaks[which]++;
            }
            run = 0;
        }
    }
}

static void runScene(int which) {
    waitVblank();
    drawScene(which);
    waitVblank(); // The swap, then a whole frame to draw it
    waitVblank();
    // Source A only, from the 3D layer, into VRAM B at full size
    DISPCAPCNT = (1u << 31) | (1 << 24) | (3 << 20) | (1 << 16) | 16;
    while (DISPCAPCNT & (1u << 31));
    unsigned short *image = &IMAGES[which * 256 * 192];
    for (int i = 0; i < 256 * 192; i++) image[i] = CAPTURE[i];
    findStreaks(which, image);
}

static void show(int which, int zoom, const char *saved, int savedColor) {
    const unsigned short *image = &IMAGES[which * 256 * 192];
    int fromX = streakX[which] - 28, fromY = streakY[which] - 16;
    if (!streaks[which]) { fromX = 64; fromY = 99; }
    if (fromX < 0) fromX = 0;
    if (fromX > 256 - 64) fromX = 256 - 64;
    if (fromY < 0) fromY = 0;
    if (fromY > 192 - 48) fromY = 192 - 48;
    for (int y = 0; y < 192; y++)
        for (int x = 0; x < 256; x++)
            SCREEN[y * 256 + x] = (zoom ? image[(fromY + y / 4) * 256 + fromX + x / 4] : image[y * 256 + x]) | BLACK;
    line(0);
    number(which + 1, WHITE);
    text(" ", WHITE);
    text(scenes[which].name, WHITE);
    line(1);
    text("STREAKS ", WHITE);
    number(streaks[which], streaks[which] ? RED : GREEN);
    if (streaks[which]) {
        text(" AT ", WHITE);
        number(streakX[which], WHITE);
        text(",", WHITE);
        number(streakY[which], WHITE);
    }
    line(2);
    text(saved, savedColor);
    line(23);
    text("A NEXT  B ZOOM", YELLOW);
}

void cpuMain(void) {
    REG32(0x04000304) = 0x820F; // The LCDs, both 2D engines and the 3D engine, with this engine on the top screen
    REG32(0x04000240) = 0x8080; // VRAM A and B to the CPU
    DISPCNT = 0x20108; // Show VRAM A, with the 3D layer on so it's drawn for capturing
    STATUS = 0;
    for (int i = 0; i < 256 * 192; i++) SCREEN[i] = BLACK;
    line(0);
    text("DRAWING", WHITE);
    REG32(0x04000600) = 1 << 15; // Clear any matrix stack error

    // The first frame after starting comes out as one flat color in NooDS U, so it's drawn and left out
    runScene(0);
    streaks[0] = 0;
    for (int i = 0; i < SCENES; i++) runScene(i);
    STATUS = 0x444F4E45; // DONE

    line(1);
    text("SAVING", WHITE);
    const char *name = "NOT SAVED, TAKE PHOTOS";
    int color = RED;
    const char *where;
    if (saveStart(&where)) {
        saveWord(0x53443352); // R3DS
        saveWord(SCENES);
        for (int i = 0; i < SCENES * 256 * 192; i += 2)
            saveWord(IMAGES[i] | ((unsigned)IMAGES[i + 1] << 16));
        if (saveEnd() > 0) {
            name = where;
            color = GREEN;
        }
    }

    int which = 0, zoom = 0, last = 0;
    show(which, zoom, name, color);
    while (1) {
        waitVblank();
        int keys = ~KEYS & 0x3FF, pressed = keys & ~last;
        last = keys;
        if (pressed & 1) which = (which + 1) % SCENES;
        if (pressed & 2) zoom = !zoom;
        if (pressed & 3) show(which, zoom, name, color);
    }
}
