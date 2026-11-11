// Checks the DSi's microphone FIFO and SNDEXCNT with both CPUs halted, through the real registers and events: rates, formats,
// status bits, overrun, clearing, interrupts and NDMA mode 0xC. tests/hardware's microphone ROM settled how a real DSi does them
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
#define NEAR(a, b, t) do { checks++; long long x_ = (a), y_ = (b); if (llabs(x_ - y_) > (t)) { fails++; printf("FAIL line %d [%s]: %s = %lld, want %lld +-%d\n", __LINE__, tag, #a, x_, y_, (int)(t)); } } while (0)
static char tag[64] = "";
static Core *c;
static const uint32_t MIC_CNT = 0x4004600, MIC_DATA = 0x4004604, SNDEXCNT = 0x4004700, DST = 0x2300000;
static const int FRAME = 560190;
static uint32_t r(uint32_t a) { return c->memory.read<uint32_t>(1, a, false); }
static uint16_t rh(uint32_t a) { return c->memory.read<uint16_t>(1, a, false); }
static void w(uint32_t a, uint32_t v) { c->memory.write<uint32_t>(1, a, v, false); }
static void wh(uint32_t a, uint16_t v) { c->memory.write<uint16_t>(1, a, v, false); }
static int16_t level = 0x1234;
static void feed() { static int16_t s[4096]; for (auto &x : s) x = level; c->spi.sendMicData(s, 4096, 32768); }
static void frame(int n = 1) { while (n--) { feed(); do c->runCore(); while (!c->frameEnded.exchange(false)); } }
static bool irq() { return c->interpreter[1].irf & BITL(32 + 14); }
static void noIrq() { c->interpreter[1].irf &= ~BITL(32 + 14); }
static void reset() { wh(MIC_CNT, 0); wh(MIC_CNT, BIT(12)); wh(SNDEXCNT, 0); noIrq(); w(0x400411C, 0); }
static int status() { return (rh(MIC_CNT) >> 8) & 0xF; }
static int drain() { int n = 0; while (!(rh(MIC_CNT) & BIT(8)) && n < 100) { r(MIC_DATA); n++; } return n; }
static void dma(uint32_t extra = 0) {
    // NDMA channel 0 on the ARM7 copies blocks of 8 words from MIC_DATA to RAM, forever
    w(0x400411C, 0); w(0x4004104, MIC_DATA); w(0x4004108, DST); w(0x400410C, 0); w(0x4004110, 8);
    w(0x400411C, BIT(31) | BIT(29) | (0xC << 24) | (2 << 13) | extra);
    for (uint32_t i = 0; i < 0x10000; i += 4) w(DST + i, 0xDEADBEEF);
}
// Moves to a time on the free-running sample clock's frame boundary for every divider, so the first slot comes one period later
static void align(uint32_t period = 12288) { c->globalCycles = (c->globalCycles / period + 1) * period; }
static int copied() { int n = 0; for (uint32_t i = 0; i < 0x10000; i += 4) n += r(DST + i) != 0xDEADBEEF; return n; }

int main(int argc, char **argv) {
    std::string files = argv[1];
    Settings::load(".");
    Settings::ndsBios9Path = files + "/bios9.bin"; Settings::ndsBios7Path = files + "/bios7.bin"; Settings::ndsFirmPath = files + "/firmware.bin";
    Settings::dsiBios9Path = files + "/bios9i.bin"; Settings::dsiBios7Path = files + "/bios7i.bin"; Settings::dsiFirmPath = files + "/firmwarei.bin";
    Settings::dsiNandPath = "nand.bin"; Settings::jit = 0; Settings::dsiMode = 1; Settings::directBoot = 1; Settings::savesFolder = 1;
    Settings::threaded2D = 0; Settings::threaded3D = 0; Settings::emulateAudio = 0; Settings::fast2D = 0; Settings::highRes3D = 0; Settings::hardware3D = 0; Settings::fpsLimiter = 0;
    Rtc::fixedTime = 1767225600;
    try { c = new Core(argv[2]); } catch (CoreError e) { printf("core error %d\n", int(e)); return 2; }
    for (int cpu = 0; cpu < 2; cpu++) { c->interpreter[cpu].halt(0); c->memory.write<uint32_t>(cpu, 0x4000208, 0, false); c->memory.write<uint32_t>(cpu, 0x4000210, 0, false); }
    frame(2);

    // Register bits; SNDEXCNT's frequency bit is locked while bit 15 is set
    strcpy(tag, "regs");
    reset();
    wh(SNDEXCNT, 0xFFFF); EQ(rh(SNDEXCNT), 0xE00F);
    wh(SNDEXCNT, 0x8000); EQ(rh(SNDEXCNT), 0xA000);
    wh(SNDEXCNT, 0x0000); wh(SNDEXCNT, 0x0000); EQ(rh(SNDEXCNT), 0x0000);
    wh(MIC_CNT, 0x6F0F); EQ(rh(MIC_CNT), 0x610F); EQ(rh(MIC_CNT + 2), 0);
    wh(MIC_CNT, 0); EQ(rh(MIC_CNT), 0x0100);
    EQ(r(MIC_DATA), 0);
    EQ(c->memory.read<uint32_t>(0, MIC_CNT, false), 0); // The ARM9 doesn't have it

    // Nothing arrives unless both enable bits are set and the format isn't 3
    struct { uint16_t snd, mic; } off[] = { {0x0000, 0x8002}, {0x8000, 0x0002}, {0x8000, 0x8003} };
    for (auto &o : off) {
        snprintf(tag, sizeof tag, "off %04X %04X", o.snd, o.mic);
        reset(); wh(SNDEXCNT, o.snd); wh(MIC_CNT, o.mic | (3 << 13)); frame(1);
        EQ(status(), 0x1); EQ(irq(), 0);
    }

    // Rates through NDMA: F/1 to F/4 of 32.73kHz and 47.61kHz, mono and stereo, in words per frame
    for (int hz = 0; hz < 2; hz++) for (int div = 0; div < 4; div++) for (int fmt = 0; fmt < 3; fmt++) {
        snprintf(tag, sizeof tag, "rate %s F/%d fmt %d", hz ? "47k" : "32k", div + 1, fmt);
        reset(); wh(SNDEXCNT, 0x8000 | (hz << 13)); dma();
        wh(MIC_CNT, 0x8000 | (div << 2) | fmt);
        frame(2);
        int samples = 2 * FRAME / ((hz ? 704 : 1024) * (div + 1));
        int words = fmt == 0 ? samples : samples / 2;
        NEAR(copied(), words, 8);
        EQ(status() & 0x8, 0); // No overrun while NDMA keeps up
        EQ(r(DST), 0x12341234);
        EQ(r(0x400411C) >> 31, 1);
    }

    // Each word holds two samples, the older one in the low half; in stereo, the right slot repeats the left one's sample
    for (int fmt : {0, 2}) {
        snprintf(tag, sizeof tag, "order fmt %d", fmt);
        reset(); wh(SNDEXCNT, 0x8000); frame(1);
        static int16_t ramp[4096]; for (int i = 0; i < 4096; i++) ramp[i] = i * 4;
        c->spi.micRead = c->spi.micWrite; c->spi.micCycles = c->globalCycles; // Skip the input left from earlier
        c->spi.sendMicData(ramp, 4096, 131072); // A new value every 256 cycles, so slots half a frame apart differ
        wh(MIC_CNT, 0x8000 | fmt); c->globalCycles += 1024 * 40;
        int bad = 0; int16_t last = -1;
        for (int i = 0; i < 16; i++) {
            uint32_t v = r(MIC_DATA); int16_t lo = v, hi = v >> 16;
            if (fmt == 0 ? hi != lo : hi <= lo) bad++;
            if (lo <= last) bad++;
            last = hi;
        }
        EQ(bad, 0);
        frame(1);
    }

    // A sample per halfword at known times: start, read, and the FIFO fills exactly at 16 words
    strcpy(tag, "fill");
    reset(); wh(SNDEXCNT, 0x8000); align(); wh(MIC_CNT, 0x8002);
    EQ(status(), 0x1);
    { uint32_t start = c->globalCycles;
      c->globalCycles = start + 1024 * 2 - 1; EQ(status(), 0x1);
      c->globalCycles = start + 1024 * 2; EQ(status(), 0x0);
      c->globalCycles = start + 1024 * 16; EQ(status(), 0x2);
      c->globalCycles = start + 1024 * 32; EQ(status(), 0x6);
      c->globalCycles = start + 1024 * 33; EQ(status(), 0xE);
      // Overrun stops the FIFO; all 16 words can still be read, then it's empty for good, and reads give the last word again,
      // even after clearing
      c->globalCycles = start + 1024 * 200; EQ(drain(), 16); EQ(status(), 0x9);
      uint32_t last = c->dsiMic.lastData; EQ(last, 0x12341234); EQ(r(MIC_DATA), last); EQ(r(MIC_DATA), last);
      c->globalCycles = start + 1024 * 300; EQ(status(), 0x9);
      // Clearing only works while disabled
      wh(MIC_CNT, 0x9002); EQ(status(), 0x9);
      wh(MIC_CNT, 0x0002); wh(MIC_CNT, 0x1002); EQ(status(), 0x1); EQ(r(MIC_DATA), last); }
    frame(1);

    // Stereo fills twice as fast, with the right slot half a frame after the left: 8 words at the 8th frame's right slot
    strcpy(tag, "stereo fill");
    reset(); wh(SNDEXCNT, 0x8000); align(); wh(MIC_CNT, 0x8000);
    { uint32_t start = c->globalCycles;
      c->globalCycles = start + 1024 + 511; EQ(status(), 0x1);
      c->globalCycles = start + 1024 + 512; EQ(status(), 0x0);
      c->globalCycles = start + 1024 * 8 + 511; EQ(status(), 0x0);
      c->globalCycles = start + 1024 * 8 + 512; EQ(status(), 0x2);
      c->globalCycles = start + 1024 * 16 + 512; EQ(status(), 0x6);
      c->globalCycles = start + 1024 * 17; EQ(status(), 0xE);
      EQ(r(MIC_DATA), 0x12341234); }
    frame(1);

    // At a divided rate the right slot still comes half a base frame after the left: 47.61kHz F/2 has frames of 1408 cycles
    // with the right slot 352 cycles in, as the real DSi showed
    strcpy(tag, "stereo F/2 47k");
    reset(); wh(SNDEXCNT, 0x2000); wh(SNDEXCNT, 0xA000); align(1408); wh(MIC_CNT, 0x8004);
    { uint32_t start = c->globalCycles;
      c->globalCycles = start + 1408 + 351; EQ(status(), 0x1);
      c->globalCycles = start + 1408 + 352; EQ(status(), 0x0);
      c->globalCycles = start + 1408 * 16 + 351; EQ(status(), 0x2);
      c->globalCycles = start + 1408 * 16 + 352; EQ(status(), 0x6);
      c->globalCycles = start + 1408 * 17 - 1; EQ(status(), 0x6);
      c->globalCycles = start + 1408 * 17; EQ(status(), 0xE); }
    frame(1);

    // Interrupts: modes 1 and 3 at half full, once until the FIFO drops below the level; 2 when it overruns
    for (int mode = 0; mode < 4; mode++) {
        snprintf(tag, sizeof tag, "irq mode %d", mode);
        reset(); wh(SNDEXCNT, 0x8000); align(); wh(MIC_CNT, 0x8002 | (mode << 13));
        uint32_t start = c->globalCycles;
        c->globalCycles = start + 1024 * 15; c->dsiMic.update(); EQ(irq(), 0);
        c->globalCycles = start + 1024 * 16; c->dsiMic.update(); EQ(irq(), mode == 1 || mode == 3);
        c->globalCycles = start + 1024 * 32; c->dsiMic.update(); EQ(irq(), mode == 1 || mode == 3);
        c->globalCycles = start + 1024 * 33; c->dsiMic.update(); EQ(irq(), mode != 0);
        noIrq(); c->dsiMic.update(); EQ(irq(), 0);
        r(MIC_DATA); c->dsiMic.update(); EQ(irq(), 0); // Still at or over the level after a read is no new edge
        frame(1);
        // Read below the level and let it refill: another interrupt
        reset(); wh(SNDEXCNT, 0x8000); wh(MIC_CNT, 0x8002 | (mode << 13)); noIrq();
        frame(1); EQ(irq(), mode != 0); noIrq();
        if (mode) { wh(MIC_CNT, 0x0002 | (mode << 13)); wh(MIC_CNT, 0x1002 | (mode << 13)); wh(MIC_CNT, 0x8002 | (mode << 13)); frame(1); EQ(irq(), 1); }
    }

    // NDMA stops getting blocks once the microphone is off, and the rest stays in the FIFO
    strcpy(tag, "dma off");
    reset(); wh(SNDEXCNT, 0x8000); dma(); wh(MIC_CNT, 0x8002); frame(1);
    wh(MIC_CNT, 0x0002); int before = copied(); frame(2); EQ(copied(), before);
    EQ(status() & 0x2, 0);

    // NDMA is only asked for a block when the FIFO becomes half full, and not again until it has been empty: a channel
    // enabled after that never starts, and blocks smaller than 8 words stop after the first, as on a real DSi
    strcpy(tag, "dma late");
    reset(); wh(SNDEXCNT, 0x8000); wh(MIC_CNT, 0x8002);
    c->globalCycles += 1024 * 20; c->dsiMic.update();
    dma(BIT(30)); c->interpreter[1].irf &= ~BITL(28);
    frame(1); EQ(copied(), 0); EQ((c->interpreter[1].irf >> 28) & 1, 0); EQ(status(), 0xE);
    for (int block : {1, 4, 7}) {
        snprintf(tag, sizeof tag, "dma block %d", block);
        reset(); wh(SNDEXCNT, 0x8000); dma(); w(0x400411C, 0); w(0x4004110, block); w(0x400411C, BIT(31) | BIT(29) | (0xC << 24) | (2 << 13));
        wh(MIC_CNT, 0x8002); frame(2); EQ(copied(), block); EQ(status(), 0xE);
    }
    // Blocks of 16 words take the 8 there are and 8 reads of an empty FIFO, which empties it, so they carry on
    strcpy(tag, "dma block 16");
    reset(); wh(SNDEXCNT, 0x8000); dma(); w(0x400411C, 0); w(0x4004110, 16); w(0x400411C, BIT(31) | BIT(29) | (0xC << 24) | (2 << 13));
    wh(MIC_CNT, 0x8002); frame(2); NEAR(copied(), 2 * FRAME / 1024, 40); EQ(status() & 0x8, 0);

    // The sample clock runs freely: turned on partway through a frame, the first sample comes at the next frame's start
    strcpy(tag, "free clock");
    reset(); wh(SNDEXCNT, 0x8000); align();
    { uint32_t frameAt = c->globalCycles; c->globalCycles += 300; wh(MIC_CNT, 0x8002);
      c->globalCycles = frameAt + 2047; EQ(status(), 0x1);
      c->globalCycles = frameAt + 2048; EQ(status(), 0x0); }
    frame(1);

    // Holding the blow hotkey gives noise in DSi mode too: varied samples while held, the real input again after
    for (int fmt : {0, 2}) {
        snprintf(tag, sizeof tag, "blow fmt %d", fmt);
        reset(); wh(SNDEXCNT, 0x8000); frame(1);
        c->spi.setMicBlow(true); wh(MIC_CNT, 0x8000 | fmt); c->globalCycles += 1024 * 40;
        std::set<uint32_t> seen; int same = 0;
        for (int i = 0; i < 16; i++) { uint32_t v = r(MIC_DATA); seen.insert(v); same += (v & 0xFFFF) == (v >> 16); }
        NEAR(seen.size(), 16, 1); EQ(same, fmt == 0 ? 16 : 0);
        c->spi.setMicBlow(false); reset(); wh(SNDEXCNT, 0x8000); frame(1); wh(MIC_CNT, 0x8002); c->globalCycles += 1024 * 40;
        EQ(r(MIC_DATA), 0x12341234);
        frame(1);
    }

    // Save states keep the FIFO
    strcpy(tag, "state");
    reset(); wh(SNDEXCNT, 0x8000); align(); wh(MIC_CNT, 0x8002);
    c->globalCycles += 1024 * 10; uint16_t st = rh(MIC_CNT);
    { FILE *f = tmpfile(); c->dsiMic.saveState(f); reset(); rewind(f); c->dsiMic.loadState(f); fclose(f); }
    EQ(rh(MIC_CNT), st); EQ(drain(), 5);

    reset();
    printf("%d of %d checks passed\n", checks - fails, checks);
    return fails != 0;
}
