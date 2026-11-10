// What the display FIFO test saves: a block of words in main RAM with each scene's results, then the captures
// fifo.py explains the results; the scenes are the same for the ROM and fifo.py

#define R_MAGIC0 0 // "FIFO"
#define R_MAGIC1 1 // "TEST"
#define R_STATUS 2 // STATUS_DONE once every scene ran
#define R_STEP 3 // The scene being run
#define R_SCFG9 4 // SCFG_EXT as the ARM9 found it, 0 in DS mode
#define R_READ32 5 // A 32-bit read of DISP_MMEM_FIFO
#define R_READ16 6 // A 16-bit read of DISP_MMEM_FIFO
#define R_RESULTS 8
#define RESULT_WORDS 16

#define FIFO_MAGIC0 0x4F464946 // FIFO
#define FIFO_MAGIC1 0x54534554 // TEST
#define STATUS_DONE 1

// Each scene runs from the start of a V-blank for 2 frames. CAP is 1 for scenes that capture the FIFO in the first
// frame, DSI is 1 for scenes that need NDMA and only run in DSi mode
//   REPEAT4    display mode 3, DMA filling the FIFO 4 words at a time with repeat, as GBATEK says to
//   STALE      display mode 3 with nothing filling the FIFO, right after REPEAT4
//   CAPONLY    display mode 1 with the FIFO only captured, DMA as REPEAT4
//   BIG        DMA sending the whole bitmap at once, without repeat
//   COUNT8     DMA sending 8 words at a time with repeat
//   COUNT2     DMA sending 2 words at a time with repeat
//   HALF       DMA sending 8 halfwords at a time with repeat, all to the bottom half of the register
//   CPU        the CPU writing 16 pixels in V-blank with words, halfwords, bytes, and halfwords to the bottom only
//   BLEND      the FIFO captured blended 8/16 with engine A's green backdrop, DMA as REPEAT4
//   LINES3     display mode 3, DMA as REPEAT4, and a second DMA recording VCOUNT each time it's triggered
//   LINES1     display mode 1 with nothing captured, only the DMA recording VCOUNT
//   MID3       as LINES3, with both DMAs started at line 100
//   MID1       as LINES1, started at line 100
//   NDMA       display mode 3, NDMA mode 9 filling the FIFO 4 words a block, repeating forever
//   NDMALINES  display mode 3, NDMA as NDMA, and a second NDMA in mode 9 recording VCOUNT
//   NDMAUNUSED display mode 1, only the NDMA recording VCOUNT
#define SCENES(X) \
    X(REPEAT4, 1, 0) X(STALE, 1, 0) X(CAPONLY, 1, 0) X(BIG, 1, 0) X(COUNT8, 1, 0) X(COUNT2, 1, 0) X(HALF, 1, 0) \
    X(CPU, 1, 0) X(BLEND, 1, 0) X(LINES3, 0, 0) X(LINES1, 0, 0) X(MID3, 1, 0) X(MID1, 0, 0) \
    X(NDMA, 1, 1) X(NDMALINES, 0, 1) X(NDMAUNUSED, 0, 1)

enum {
#define X(name, cap, dsi) S_##name,
    SCENES(X)
#undef X
    SCENE_COUNT
};

#define CAPTURES 11
#define CAPTURE_WORDS (256 * 192 / 2)
#define R_CAPTURES (R_RESULTS + SCENE_COUNT * RESULT_WORDS)
#define R_TOTAL (R_CAPTURES + CAPTURES * CAPTURE_WORDS)

// Result words of each scene
#define W_RAN 0 // 1 if the scene ran, 0 if it was skipped
#define W_CNT191 1 // The control register of the channel filling or recording, at line 191 of the first frame
#define W_CNT193 2 // The same at line 193, after V-blank started
#define W_CNT_END 3 // The same after the second frame
#define W_CAPCNT 4 // DISPCAPCNT at line 193 of the first frame
#define W_TOTAL 5 // VCOUNTs recorded over both frames
#define W_FIRST 6 // The first line recorded, and the last
#define W_LAST 7
#define W_LOW 8 // The lowest line recorded, and the highest
#define W_HIGH 9
#define W_FIRST_COUNT 10 // Transfers on the first line recorded
#define W_MIN 11 // The fewest and most transfers on a line after the first, among lines with any
#define W_MAX 12
#define W_LINES 13 // Lines with any transfers, counting each frame's separately
#define W_FRAMES 14 // Frames with any transfers
#define NO_LINE 0xFFFF

#define BITMAP_PIXEL(x, y) ((y) << 8 | (x)) // Each pixel's own position, so bit 15 is set from line 128
#define REC_WORDS 16384
