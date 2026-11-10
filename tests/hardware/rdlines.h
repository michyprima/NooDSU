// What the 3D timing test saves: a block of words in main RAM with each scene's results, then the 3D captures
// rdlines.py explains the results; the scenes are the same for the ROM and rdlines.py

#define R_MAGIC0 0 // "RDLN"
#define R_MAGIC1 1 // "TEST"
#define R_STATUS 2 // STATUS_DONE once every scene ran
#define R_STEP 3 // The scene being measured
#define R_SCFG9 4 // SCFG_EXT as the ARM9 found it, 0 in DS mode
#define R_RESULTS 8
#define RESULT_WORDS 12

#define RDLN_MAGIC0 0x4E4C4452 // RDLN
#define RDLN_MAGIC1 0x54534554 // TEST
#define STATUS_DONE 1

// Each scene draws N opaque quads, each a different colour and nearer than the one before, W pixels wide from the left
// edge and covering lines Y0 to Y1 (Y1 not included). CAP is 1 for scenes whose 3D output is captured and saved
#define SCENES(X) \
    X(EMPTY, 0, 4, 0, 192, 0) \
    X(N1, 1, 4, 0, 192, 0) X(N50, 50, 4, 0, 192, 0) X(N100, 100, 4, 0, 192, 0) X(N150, 150, 4, 0, 192, 0) \
    X(N170, 170, 4, 0, 192, 0) X(N180, 180, 4, 0, 192, 0) X(N190, 190, 4, 0, 192, 0) X(N200, 200, 4, 0, 192, 0) \
    X(N220, 220, 4, 0, 192, 0) X(N250, 250, 4, 0, 192, 0) X(N300, 300, 4, 0, 192, 0) X(N400, 400, 4, 0, 192, 0) \
    X(N500, 500, 4, 0, 192, 0) X(N700, 700, 4, 0, 192, 0) X(N1000, 1000, 4, 0, 192, 0) X(N1500, 1500, 4, 0, 192, 0) \
    X(N200W1, 200, 1, 0, 192, 0) X(N200W2, 200, 2, 0, 192, 0) X(N200W3, 200, 3, 0, 192, 0) X(N200W5, 200, 5, 0, 192, 0) \
    X(N200W6, 200, 6, 0, 192, 0) X(N200W8, 200, 8, 0, 192, 0) \
    X(N16W64, 16, 64, 0, 192, 0) X(N16W96, 16, 96, 0, 192, 0) X(N16W128, 16, 128, 0, 192, 0) \
    X(N16W144, 16, 144, 0, 192, 0) X(N16W160, 16, 160, 0, 192, 0) X(N16W192, 16, 192, 0, 192, 0) \
    X(N16W256, 16, 256, 0, 192, 0) \
    X(N32W32, 32, 32, 0, 192, 0) X(N32W48, 32, 48, 0, 192, 0) X(N32W56, 32, 56, 0, 192, 0) \
    X(N32W64, 32, 64, 0, 192, 0) X(N32W72, 32, 72, 0, 192, 0) X(N32W96, 32, 96, 0, 192, 0) \
    X(N32W128, 32, 128, 0, 192, 0) \
    X(TOP16, 300, 4, 0, 16, 0) X(TOP32, 300, 4, 0, 32, 0) X(TOP48, 300, 4, 0, 48, 0) X(TOP64, 300, 4, 0, 64, 0) \
    X(TOP96, 300, 4, 0, 96, 0) X(TOP128, 300, 4, 0, 128, 0) \
    X(BOT32, 300, 4, 160, 192, 0) X(BOT64, 300, 4, 128, 192, 0) X(BOT96, 300, 4, 96, 192, 0) \
    X(BOT128, 300, 4, 64, 192, 0) X(MID64, 300, 4, 64, 128, 0) \
    X(H1, 1000, 4, 96, 97, 0) X(H2, 1000, 4, 96, 98, 0) X(H3, 1000, 4, 96, 99, 0) X(H4, 1000, 4, 96, 100, 0) \
    X(CAP8, 8, 256, 0, 192, 1) X(CAP24, 24, 256, 0, 192, 1) X(CAP64, 64, 256, 0, 192, 1) X(CAP300, 300, 4, 0, 192, 1)

enum {
#define X(name, n, w, y0, y1, cap) S_##name,
    SCENES(X)
#undef X
    SCENE_COUNT
};

#define CAPTURES 4
#define CAPTURE_WORDS (256 * 192 / 2)
#define R_CAPTURES (R_RESULTS + SCENE_COUNT * RESULT_WORDS)
#define R_TOTAL (R_CAPTURES + CAPTURES * CAPTURE_WORDS)

// Result words of each scene
#define W_STEADY 0 // RDLINES_COUNT at the end, after the scene has been shown for a few frames
#define W_DISP3DCNT 1 // DISP3DCNT at the end
#define W_RAMCOUNT 2 // RAM_COUNT once the scene was sent, to check every polygon made it
#define W_BEFORE 3 // RDLINES_COUNT before swapping to the scene, with the empty scene shown
#define W_CHANGES 4 // Up to 4 changes of RDLINES_COUNT after the swap: frame << 24 | line << 8 | value
#define W_FLAGS 8 // Up to 4 times DISP3DCNT bit 12 was found set, each then acknowledged: frame << 16 | line
#define NO_EVENT 0xFFFFFFFF
#define WATCH_FRAMES 6 // How many frames after the swap are watched
