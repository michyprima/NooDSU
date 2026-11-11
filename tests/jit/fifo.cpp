// Checks the main memory display FIFO with both CPUs halted, through the real registers and events: what writes to
// DISP_MMEM_FIFO do, engine A showing it in display mode 3, display capture taking it as source B, DMA timing 4 and DSi
// NDMA mode 9 filling it, and the DMA being stopped at V-blank. The expected values are what tests/hardware's
// noodsu-fifo.nds found on a real DSi, and melonDS for the rest
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#define private public
#define main gameMain
#include "game.cpp"
#undef main
#undef private

static int fails = 0, checks = 0;
#define EQ(a, b) do { checks++; auto x_ = (a); auto y_ = (b); if ((uint64_t)x_ != (uint64_t)y_) { fails++; printf("FAIL line %d [%s]: %s = 0x%llX, want 0x%llX\n", __LINE__, tag, #a, (long long)x_, (long long)y_); } } while (0)
static char tag[64] = "";
static Core *c;
static uint16_t rh(uint32_t a) { return c->memory.read<uint16_t>(0, a, false); }
static uint32_t r(uint32_t a) { return c->memory.read<uint32_t>(0, a, false); }
static void wb(uint32_t a, uint8_t v) { c->memory.write<uint8_t>(0, a, v, false); }
static void wh(uint32_t a, uint16_t v) { c->memory.write<uint16_t>(0, a, v, false); }
static void w(uint32_t a, uint32_t v) { c->memory.write<uint32_t>(0, a, v, false); }
static uint32_t screen[256 * 192 * 8];
static void frame(int n = 1) { while (n--) { do c->runCore(); while (!c->frameEnded.exchange(false)); c->ndma[0].update(); } }
static void drain() { while (c->gpu.getFrame(screen, false)); }

static const uint32_t BITMAP = 0x2200000, FIFO = 0x4000068, DMA3 = 0x40000D4;
static uint16_t pixel(int x, int y) { return ((x * 0x9E37 + y * 0x4F1B) >> 3) & 0xFFFF; }
static void fillBitmap() { for (int i = 0; i < 256 * 192; i++) wh(BITMAP + i * 2, pixel(i & 255, i >> 8)); }

// What the top screen shows for an RGB5 color, through the same conversions a frame takes
static uint32_t shown(uint16_t color) { return Gpu::rgb6ToRgb8(Gpu2D::rgb5ToRgb6(color & 0x7FFF)); }

// Count the top screen's pixels that differ from what a function of the position gives, in the last frame
template <typename F> static int topDiffers(F want) {
    int bad = 0;
    for (int y = 0; y < 192; y++)
        for (int x = 0; x < 256; x++)
            if (screen[y * 256 + x] != shown(want(x, y))) { if (!bad++ || getenv("FIFO_DEBUG")) printf("  first bad at %d,%d: %08X want %08X\n", x, y, screen[y * 256 + x], shown(want(x, y))); }
    return bad;
}

// Start ARM9 DMA channel 3 in display FIFO mode from the bitmap, 32-bit or 16-bit with the destination fixed
static void fifoDma(uint32_t count, bool repeat, bool words = true) {
    w(DMA3 + 8, 0); w(DMA3, BITMAP); w(DMA3 + 4, FIFO);
    w(DMA3 + 8, BIT(31) | (4 << 27) | (words ? BIT(26) : 0) | (repeat ? BIT(25) : 0) | (2 << 21) | count);
}

// Run the scheduled tasks until a scanline starts, the way the core does with both CPUs halted
static void runTo(int line) {
    while (c->gpu.vCount != line) {
        c->globalCycles = c->events[0].cycles;
        while (c->events[0].cycles <= c->globalCycles) { c->tasks[c->events[0].task](); c->events.popFront(); }
    }
    c->frameEnded.store(false);
}

static void boot(const char *rom, bool dsi, bool threads) {
    delete c;
    Settings::dsiMode = dsi; Settings::threaded2D = threads; Settings::fast2D = threads;
    try { c = new Core(rom); } catch (CoreError e) { printf("core error %d\n", int(e)); exit(2); }
    for (int cpu = 0; cpu < 2; cpu++) { c->interpreter[cpu].halt(0); c->memory.write<uint32_t>(cpu, 0x4000208, 0, false); }
    c->gpu.frontendUpscale = 0;
    wh(0x4000304, 0x820F); // Engine A on the top screen
    w(0x4000000, 0x10000); // Display mode 1 with nothing on
    wh(0x400006C, 0); // No master brightness
    frame(2); drain();
}

static void displayChecks(const char *rom, bool dsi, bool threads) {
    const char *mode = threads ? (dsi ? "dsi threads" : "threads") : (dsi ? "dsi" : "ds");
    boot(rom, dsi, threads);
    fillBitmap();

    // DMA every 8 pixels, 4 words at a time with repeat, shows the bitmap from main RAM as it is
    snprintf(tag, sizeof tag, "%s dma repeat", mode);
    w(0x4000000, 0x30000);
    fifoDma(4, true);
    frame(); EQ(c->gpu.getFrame(screen, false), true);
    EQ(topDiffers(pixel), 0);
    EQ(r(DMA3 + 8) >> 31, 0); // Stopped at V-blank
    EQ(c->dma[0].srcAddrs[3], BITMAP + 256 * 192 * 2);

    // Without starting it again, the next frame repeats whatever the FIFO last held, every 16 pixels
    snprintf(tag, sizeof tag, "%s stale", mode);
    frame(); EQ(c->gpu.getFrame(screen, false), true);
    EQ(topDiffers([](int x, int) { return pixel(240 + (x & 15), 191); }), 0);

    // The DMA has to be started again each frame; started again, the same picture shows
    snprintf(tag, sizeof tag, "%s restart", mode);
    fifoDma(4, true); frame(); EQ(c->gpu.getFrame(screen, false), true);
    EQ(topDiffers(pixel), 0);

    // Repeating 8 words at a time still sends 4 at each request, carrying on with the rest at the next, and so does
    // repeating 8 half-words, each of which is a pixel
    snprintf(tag, sizeof tag, "%s count 8", mode);
    fifoDma(8, true); frame(); EQ(c->gpu.getFrame(screen, false), true);
    EQ(topDiffers(pixel), 0);
    snprintf(tag, sizeof tag, "%s half-words", mode);
    fifoDma(8, true, false); frame(); EQ(c->gpu.getFrame(screen, false), true);
    EQ(topDiffers(pixel), 0);
    EQ(c->dma[0].srcAddrs[3], BITMAP + 256 * 192 * 2);

    // Repeating 2 words at a time sends 4 pixels every 8, which land between reading pixels 8n + 2 and 8n + 3, as a real
    // DSi showed: a line shows the 4 sent for it where they arrive in time, and otherwise the ring's older pixels
    snprintf(tag, sizeof tag, "%s count 2", mode);
    fifoDma(2, true); frame(); EQ(c->gpu.getFrame(screen, false), true);
    EQ(c->dma[0].srcAddrs[3], BITMAP + 192 * 32 * 8);
    for (int x = 0; x < 8; x++) EQ(screen[256 + x], shown(pixel(128 + x, 0)));
    for (int x = 8; x < 11; x++) EQ(screen[256 + x], shown(pixel(112 + x, 0)));
    EQ(screen[256 + 11], shown(pixel(139, 0)));

    // A DMA started after the frame started waits for the next frame, which it doesn't reach since V-blank stops it
    snprintf(tag, sizeof tag, "%s mid-frame", mode);
    fifoDma(4, true); frame(); drain();
    runTo(100); fifoDma(4, true); frame(); EQ(c->gpu.getFrame(screen, false), true);
    EQ(c->dma[0].srcAddrs[3], BITMAP);
    EQ(topDiffers([](int x, int) { return pixel(240 + (x & 15), 191); }), 0);

    // One transfer of the whole bitmap at the first request overruns the 16-pixel ring, leaving its last 16 pixels
    // A real DSi sends it as fast as it can while the display reads, which shows a different jumble
    snprintf(tag, sizeof tag, "%s one big transfer", mode);
    fifoDma(256 * 192 / 2, false); frame(); EQ(c->gpu.getFrame(screen, false), true);
    EQ(topDiffers([](int x, int) { return pixel(240 + (x & 15), 191); }), 0);

    // Display mode 3 with only engine B is ignored, since only engine A has it
    snprintf(tag, sizeof tag, "%s engine B", mode);
    w(0x4001000, 0x30000); EQ(r(0x4001000) & 0x30000, 0x10000);

    // Master brightness still applies
    snprintf(tag, sizeof tag, "%s brightness", mode);
    wh(0x400006C, BIT(15) | 16); fifoDma(4, true); frame(); EQ(c->gpu.getFrame(screen, false), true);
    EQ(topDiffers([](int, int) { return 0; }), 0);
    wh(0x400006C, 0);

    // Display capture takes the FIFO as source B, raw, with display mode 1 showing something else
    snprintf(tag, sizeof tag, "%s capture", mode);
    w(0x4000000, 0x10000);
    wb(0x4000240, 0x80); // VRAM A to LCDC
    for (int i = 0; i < 256 * 192; i++) wh(0x6800000 + i * 2, 0);
    w(0x4000064, BIT(31) | (1 << 29) | BIT(25) | (3 << 20));
    fifoDma(4, true); frame(); drain();
    int bad = 0;
    for (int i = 0; i < 256 * 192; i++) bad += rh(0x6800000 + i * 2) != pixel(i & 255, i >> 8);
    EQ(bad, 0);
    EQ(r(0x4000064) >> 31, 0);

    // Blended with engine A's backdrop at full source B weight, it's the FIFO's colors with bit 15 set, or black where
    // their bit 15 is clear
    snprintf(tag, sizeof tag, "%s capture blended", mode);
    wh(0x5000000, 0);
    w(0x4000064, BIT(31) | (2 << 29) | BIT(25) | (3 << 20) | (16 << 8));
    fifoDma(4, true); frame(); drain();
    bad = 0;
    for (int i = 0; i < 256 * 192; i++) {
        uint16_t p = pixel(i & 255, i >> 8);
        bad += rh(0x6800000 + i * 2) != ((p & BIT(15)) ? p : BIT(15));
    }
    EQ(bad, 0);

    // Blending rounds to the nearest
    snprintf(tag, sizeof tag, "%s capture rounding", mode);
    wh(0x5000000, 0x0003);
    w(0x4000064, BIT(31) | (2 << 29) | BIT(25) | (3 << 20) | (8 << 8) | 8);
    for (int i = 0; i < 8; i++) w(FIFO, 0x80008000);
    frame(); drain();
    EQ(rh(0x6800000), BIT(15) | 2);
    w(0x4000064, 0);

    // With the FIFO neither shown nor captured, a DMA waiting for it never transfers, and V-blank still stops it
    snprintf(tag, sizeof tag, "%s unused", mode);
    fifoDma(4, true); frame(); drain();
    EQ(c->dma[0].srcAddrs[3], BITMAP);
    EQ(r(DMA3 + 8) >> 31, 0);

    // With nothing using it and no DMA waiting at the start of the frame, the FIFO doesn't run that frame
    snprintf(tag, sizeof tag, "%s not running", mode);
    w(0x4000000, 0x30000); frame(); drain();
    w(0x4000000, 0x10000); frame(); drain();
    EQ(c->gpu.runFifo, false);
}

int main(int argc, char **argv) {
    std::string files = argv[1];
    const char *rom = argv[2];
    Settings::load(".");
    Settings::ndsBios9Path = files + "/bios9.bin"; Settings::ndsBios7Path = files + "/bios7.bin"; Settings::ndsFirmPath = files + "/firmware.bin";
    Settings::dsiBios9Path = files + "/bios9i.bin"; Settings::dsiBios7Path = files + "/bios7i.bin"; Settings::dsiFirmPath = files + "/firmwarei.bin";
    Settings::dsiNandPath = "nand.bin"; Settings::jit = 0; Settings::directBoot = 1; Settings::savesFolder = 1;
    Settings::threaded3D = 0; Settings::emulateAudio = 0; Settings::highRes3D = 0; Settings::hardware3D = 0;
    Settings::fpsLimiter = 0; Settings::screenGhost = 0; Settings::frameskip = 0;
    Rtc::fixedTime = 1767225600;

    // Writes: a word is two pixels and moves on 2, a halfword only moves on when it's the top one, and a byte goes in
    // both halves of its pixel, with only the top byte moving on
    boot(rom, false, false);
    snprintf(tag, sizeof tag, "writes");
    Gpu &g = c->gpu;
    w(FIFO, 0x22221111); EQ(g.dispFifo[0], 0x1111); EQ(g.dispFifo[1], 0x2222); EQ(g.fifoWrite, 2);
    wh(FIFO, 0x3333); EQ(g.dispFifo[2], 0x3333); EQ(g.fifoWrite, 2);
    wh(FIFO + 2, 0x4444); EQ(g.dispFifo[3], 0x4444); EQ(g.fifoWrite, 4);
    wb(FIFO, 0x55); EQ(g.dispFifo[4], 0x5555); wb(FIFO + 1, 0x66); EQ(g.dispFifo[4], 0x5555);
    wb(FIFO + 2, 0x77); EQ(g.dispFifo[5], 0x7777); EQ(g.fifoWrite, 4); wb(FIFO + 3, 0x88); EQ(g.fifoWrite, 6);
    for (int i = 0; i < 6; i++) w(FIFO, 0xAAAA0000 + i); EQ(g.fifoWrite, 2); EQ(g.dispFifo[0], 0x0005);
    EQ(r(FIFO), 0); // Not readable

    // The ring empties at V-blank, and with nothing filling it the display repeats its 16 pixels on every line
    snprintf(tag, sizeof tag, "cpu writes");
    frame(); drain();
    EQ(g.fifoWrite, 0); EQ(g.fifoRead, 0);
    for (int i = 0; i < 8; i++) w(FIFO, ((i * 2 + 1) * 0x421 << 16) | (i * 2 * 0x421));
    w(0x4000000, 0x30000); frame(); EQ(g.getFrame(screen, false), true);
    EQ(topDiffers([](int x, int) { return (x & 15) * 0x421; }), 0);

    displayChecks(rom, false, false);
    displayChecks(rom, false, true);
    displayChecks(rom, true, false);

    // On the DSi, NDMA mode 9 fills the FIFO a block at a time while the display uses it, and moves nothing otherwise,
    // as a real DSi did in tests/hardware's NDMA test
    boot(rom, true, false);
    fillBitmap();
    for (int on = 1; on >= 0; on--) {
        snprintf(tag, sizeof tag, "ndma %s", on ? "shown" : "unused");
        w(0x4000000, on ? 0x30000 : 0x10000);
        frame(); drain();
        uint32_t ch = 0x4004104 + 1 * 0x1C;
        w(ch + 0x18, 0); w(ch, BITMAP); w(ch + 4, FIFO); w(ch + 8, 0); w(ch + 0xC, 4);
        w(ch + 0x18, BIT(31) | BIT(29) | (9 << 24) | (2 << 10));
        frame(); EQ(c->gpu.getFrame(screen, false), true);
        EQ(c->ndma[0].srcAddrs[1], on ? BITMAP + 256 * 192 * 2 : BITMAP);
        if (on) EQ(topDiffers(pixel), 0);
        w(ch + 0x18, 0);
    }

    // A save state keeps the ring and the current scanline's pixels
    snprintf(tag, sizeof tag, "state");
    boot(rom, false, false);
    for (int i = 0; i < 8; i++) w(FIFO, 0x12345678 + i);
    std::vector<uint16_t> ring(c->gpu.dispFifo, c->gpu.dispFifo + 16);
    FILE *f = tmpfile(); c->gpu.saveState(f); rewind(f);
    memset(c->gpu.dispFifo, 0, sizeof(c->gpu.dispFifo)); c->gpu.fifoWrite = 9;
    c->gpu.loadState(f); fclose(f);
    EQ(memcmp(ring.data(), c->gpu.dispFifo, 32), 0); EQ(c->gpu.fifoWrite, 0);

    delete c;
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
