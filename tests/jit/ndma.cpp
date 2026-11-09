// Checks the DSi's NDMA channels with both CPUs halted, so only the emulated hardware moves, through the real registers and
// events: tests/hardware/ndma_tests.c settled how a real DSi behaves, and this also covers what that can't, like the DS cart
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
static uint32_t r(int cpu, uint32_t a) { return c->memory.read<uint32_t>(cpu, a, false); }
static void w(int cpu, uint32_t a, uint32_t v) { c->memory.write<uint32_t>(cpu, a, v, false); }
static void wh(int cpu, uint32_t a, uint16_t v) { c->memory.write<uint16_t>(cpu, a, v, false); }
static void frame(int n = 1) { while (n--) { do c->runCore(); while (!c->frameEnded.exchange(false)); c->ndma[0].update(); c->ndma[1].update(); } }
enum { SAD = 0, DAD = 4, TCNT = 8, WCNT = 0xC, BCNT = 0x10, FDATA = 0x14, CNT = 0x18 };
static uint32_t R(int ch, int off) { return 0x4004104 + ch * 0x1C + off; }
static const uint32_t SRC = 0x2300000, DST = 0x2310000, MARK = 0xABCD1234;
static void clear() { for (uint32_t i = 0; i < 0x4000; i += 4) { w(0, DST + i, 0); } w(0, SRC, MARK); }
static int count() { int n = 0; for (uint32_t i = 0; i < 0x4000; i += 4) n += r(0, DST + i) == MARK; return n; }
static bool irq(int cpu, int ch) { return c->interpreter[cpu].irf & BITL(28 + ch); }
static void noIrq(int cpu, int ch) { c->interpreter[cpu].irf &= ~BITL(28 + ch); }
static void arm(int cpu, int ch, uint32_t cnt, uint32_t tcnt, uint32_t wcnt, uint32_t src = SRC, uint32_t dst = DST) {
    w(cpu, R(ch, CNT), 0); w(cpu, R(ch, SAD), src); w(cpu, R(ch, DAD), dst); w(cpu, R(ch, TCNT), tcnt); w(cpu, R(ch, WCNT), wcnt); noIrq(cpu, ch);
    w(cpu, R(ch, CNT), cnt);
}
static uint32_t C(int mode, int srcStep = 2, int dstStep = 0, uint32_t extra = 0) { return BIT(31) | (mode << 24) | (srcStep << 13) | (dstStep << 10) | extra; }
static void off(int cpu, int ch) { w(cpu, R(ch, CNT), 0); }
static void stopTimers() { for (int cpu = 0; cpu < 2; cpu++) for (int t = 0; t < 4; t++) wh(cpu, 0x4000102 + t * 4, 0); }
static void gxDrain() { for (int i = 0; i < 60 && !(c->gpu3D.gxStat & BIT(26)); i++) frame(); }

int main(int argc, char **argv) {
    std::string files = argv[1];
    Settings::load(".");
    Settings::ndsBios9Path = files + "/bios9.bin"; Settings::ndsBios7Path = files + "/bios7.bin"; Settings::ndsFirmPath = files + "/firmware.bin";
    Settings::dsiBios9Path = files + "/bios9i.bin"; Settings::dsiBios7Path = files + "/bios7i.bin"; Settings::dsiFirmPath = files + "/firmwarei.bin";
    Settings::dsiNandPath = "nand.bin"; Settings::jit = 0; Settings::dsiMode = 1; Settings::directBoot = 1; Settings::savesFolder = 1;
    Settings::threaded2D = 0; Settings::threaded3D = 0; Settings::arm7Hle = 0; Settings::emulateAudio = 0; Settings::fast2D = 0; Settings::highRes3D = 0; Settings::hardware3D = 0; Settings::fpsLimiter = 0;
    Rtc::fixedTime = 1767225600;
    try { c = new Core(argv[2]); } catch (CoreError e) { printf("core error %d\n", int(e)); return 2; }
    for (int cpu = 0; cpu < 2; cpu++) { c->interpreter[cpu].halt(0); w(cpu, 0x4000208, 0); w(cpu, 0x4000210, 0); }
    frame(2);

    for (int cpu = 0; cpu < 2; cpu++) {
        snprintf(tag, sizeof tag, "regs arm%d", cpu ? 7 : 9);
        w(cpu, 0x4004100, 0xFFFFFFFF); EQ(r(cpu, 0x4004100), 0x800F0000);
        EQ(r(!cpu, 0x4004100), 0); w(cpu, 0x4004100, 0); 
        for (int ch = 0; ch < 4; ch++) {
            w(cpu, R(ch, BCNT), 0xFFFFFFFF); EQ(r(cpu, R(ch, BCNT)), 0x3FFFF); w(cpu, R(ch, BCNT), 0); EQ(r(cpu, R(ch, BCNT)), 0);
            w(cpu, R(ch, TCNT), 0xFFFFFFFF); EQ(r(cpu, R(ch, TCNT)), 0xFFFFFFF); w(cpu, R(ch, WCNT), 0xFFFFFFFF); EQ(r(cpu, R(ch, WCNT)), 0xFFFFFF);
            w(cpu, R(ch, SAD), 0xFFFFFFFF); EQ(r(cpu, R(ch, SAD)), 0xFFFFFFFC);
        }
    }

    // Every event trigger, on each CPU and channel; a channel stays quiet for other CPUs' events and for modes with no source
    for (int cpu = 0; cpu < 2; cpu++) for (int ch = 0; ch < 4; ch++) {
        snprintf(tag, sizeof tag, "events arm%d ch%d", cpu ? 7 : 9, ch);
        struct { int mode; int want; int want7; } ev[] = {
            {0x6, 3, 3}, {0x7, 3 * 192, 0}, {0x8, 3 * 192, -1}, {0x5, 0, 0}, {0x9, 0, -1}, {0xC, 0, 0}, {0xD, 0, 0}, {0xF, 0, 0}, {0xB, 0, -1} };
        for (auto &e : ev) {
            int want = cpu ? e.want7 : e.want;
            if (want < 0) continue;
            clear(); arm(cpu, ch, C(e.mode), 0xFFFFF, 1);
            frame(3);
            off(cpu, ch);
            char t2[64]; snprintf(t2, sizeof t2, "%s mode %X", tag, e.mode); char saved[64]; strcpy(saved, tag); strcpy(tag, t2);
            NEAR(count(), want, e.mode == 0x6 ? 0 : 0);
            strcpy(tag, saved);
        }
        for (int t = 0; t < 4; t++) {
            snprintf(tag, sizeof tag, "timer arm%d ch%d t%d", cpu ? 7 : 9, ch, t);
            clear(); arm(cpu, ch, C(t), 0xFFFFF, 1);
            int other = cpu ^ 1; arm(other, ch, C(t), 0xFFFFF, 1, SRC, DST + 0x2000);
            wh(cpu, 0x4000100 + t * 4, 0xFF00); wh(cpu, 0x4000102 + t * 4, 0x80 | 1); // f/64, 256 ticks = 16384 clocks
            frame(3);
            stopTimers(); off(cpu, ch); off(other, ch);
            int n = 0; for (uint32_t i = 0; i < 0x2000; i += 4) n += r(0, DST + i) == MARK;
            NEAR(n, 3 * 560190 / 16384, 2);
            n = 0; for (uint32_t i = 0x2000; i < 0x4000; i += 4) n += r(0, DST + i) == MARK;
            EQ(n, 0);
            // A different timer doesn't start it
            clear(); arm(cpu, ch, C((t + 1) & 3), 0xFFFFF, 1); wh(cpu, 0x4000100 + t * 4, 0xFF00); wh(cpu, 0x4000102 + t * 4, 0x80 | 1);
            frame(1); stopTimers(); off(cpu, ch); EQ(count(), 0);
        }
        // Timers cascading: the count-up timer's own overflow triggers it
        if (1) {
            snprintf(tag, sizeof tag, "cascade arm%d ch%d", cpu ? 7 : 9, ch);
            clear(); arm(cpu, ch, C(1), 0xFFFFF, 1);
            wh(cpu, 0x4000100, 0xFF00); wh(cpu, 0x4000104, 0xFFFE); wh(cpu, 0x4000106, 0x84); wh(cpu, 0x4000102, 0x81);
            frame(3); stopTimers(); off(cpu, ch);
            NEAR(count(), 3 * 560190 / 16384 / 2, 2);
        }

        // Stops at the total, keeps TCNT, runs the same again when re-enabled, one IRQ at the very end
        snprintf(tag, sizeof tag, "total arm%d ch%d", cpu ? 7 : 9, ch);
        clear(); arm(cpu, ch, C(6, 2, 0, BIT(30)), 3, 2);
        frame(1); EQ(count(), 2); EQ(irq(cpu, ch), 0); EQ(r(cpu, R(ch, CNT)) >> 31, 1);
        frame(1); EQ(count(), 3); EQ(irq(cpu, ch), 1); EQ(r(cpu, R(ch, CNT)) >> 31, 0); EQ(r(cpu, R(ch, TCNT)), 3);
        frame(2); EQ(count(), 3);
        clear(); noIrq(cpu, ch); w(cpu, R(ch, CNT), C(6, 2, 0, BIT(30))); frame(2); EQ(count(), 3); EQ(irq(cpu, ch), 1);

        // Repeats forever with an IRQ per block
        snprintf(tag, sizeof tag, "infinite arm%d ch%d", cpu ? 7 : 9, ch);
        clear(); arm(cpu, ch, C(6, 2, 0, BIT(29) | BIT(30)), 1, 2);
        frame(1); EQ(count(), 2); EQ(irq(cpu, ch), 1); noIrq(cpu, ch);
        frame(1); EQ(count(), 4); EQ(irq(cpu, ch), 1); EQ(r(cpu, R(ch, CNT)) >> 31, 1); off(cpu, ch);
        frame(1); EQ(count(), 4);

        // While enabled, only the enable bit can be written; CNT's low bits don't exist
        snprintf(tag, sizeof tag, "locked arm%d ch%d", cpu ? 7 : 9, ch);
        w(cpu, R(ch, CNT), 0x7FFFFFFF); EQ(r(cpu, R(ch, CNT)), 0x7F0FFC00); off(cpu, ch);
        clear(); arm(cpu, ch, C(6), 100, 1); uint32_t cc = r(cpu, R(ch, CNT));
        w(cpu, R(ch, SAD), SRC + 8); w(cpu, R(ch, DAD), DST + 32); w(cpu, R(ch, TCNT), 50); w(cpu, R(ch, WCNT), 2);
        EQ(r(cpu, R(ch, SAD)), SRC); EQ(r(cpu, R(ch, DAD)), DST); EQ(r(cpu, R(ch, TCNT)), 100); EQ(r(cpu, R(ch, WCNT)), 1);
        { uint32_t fd = r(cpu, R(ch, FDATA)); w(cpu, R(ch, FDATA), ~fd); EQ(r(cpu, R(ch, FDATA)), fd); w(cpu, R(ch, BCNT), 0x12345); EQ(r(cpu, R(ch, BCNT)), 0); }
        w(cpu, R(ch, CNT), C(7, 2, 2, BIT(30))); EQ(r(cpu, R(ch, CNT)), cc);
        c->memory.write<uint8_t>(cpu, R(ch, CNT) + 3, 0x06, false); EQ(r(cpu, R(ch, CNT)) >> 31, 0);
        w(cpu, R(ch, SAD), SRC + 8); EQ(r(cpu, R(ch, SAD)), SRC + 8); off(cpu, ch);

        // Immediate: one block, TCNT ignored, one IRQ
        snprintf(tag, sizeof tag, "immediate arm%d ch%d", cpu ? 7 : 9, ch);
        clear(); arm(cpu, ch, C(0x10, 2, 0, BIT(30)), 1, 5); frame(1); EQ(count(), 5); EQ(irq(cpu, ch), 1); EQ(r(cpu, R(ch, CNT)) >> 31, 0);
        clear(); arm(cpu, ch, C(0x1F), 1, 5); frame(1); EQ(count(), 5);

        // Address steps
        snprintf(tag, sizeof tag, "steps arm%d ch%d", cpu ? 7 : 9, ch);
        clear(); for (int i = 0; i < 4; i++) w(0, SRC + i * 4, 0x100 + i);
        arm(cpu, ch, C(0x10, 0, 0), 1, 4); frame(1); for (int i = 0; i < 4; i++) EQ(r(0, DST + i * 4), 0x100 + i);
        clear(); for (int i = 0; i < 4; i++) w(0, SRC + i * 4, 0x100 + i);
        arm(cpu, ch, C(0x10, 1, 1), 1, 4, SRC + 12, DST + 12); frame(1); for (int i = 0; i < 4; i++) EQ(r(0, DST + 12 - i * 4), 0x103 - i);
        clear(); arm(cpu, ch, C(0x10, 2, 3), 1, 3); frame(1); EQ(count(), 1); // Reserved destination step is fixed on a real DSi
        clear(); w(cpu, R(ch, FDATA), 0x55AA1234); arm(cpu, ch, C(0x10, 3, 2), 1, 3); frame(1); EQ(r(0, DST), 0x55AA1234); EQ(r(0, DST + 4), 0);
        EQ(r(cpu, R(ch, CNT)) >> 31, 0);
        clear(); w(cpu, R(ch, FDATA), MARK); arm(cpu, ch, C(0x10, 3, 0), 1, 3); frame(1); EQ(count(), 3);

        // Reload at the end of each block
        snprintf(tag, sizeof tag, "reload arm%d ch%d", cpu ? 7 : 9, ch);
        clear(); arm(cpu, ch, C(6, 2, 0, BIT(12)), 0xFF, 2); frame(3); off(cpu, ch); EQ(count(), 2);
        clear(); for (int i = 0; i < 2; i++) w(0, SRC + i * 4, 0x200 + i);
        arm(cpu, ch, C(6, 0, 0, BIT(15)), 4, 2); frame(2); EQ(r(0, DST + 8), 0x200); EQ(r(0, DST + 12), 0x201); w(0, SRC, MARK);
        snprintf(tag, sizeof tag, "defaults arm%d ch%d", cpu ? 7 : 9, ch);
        arm(cpu, ch, C(6), 0, 1); EQ(c->ndma[cpu].totalLeft[ch], 0x10000000); off(cpu, ch);
    }

    // GXFIFO
    strcpy(tag, "gxfifo");
    for (uint32_t i = 0; i < 1000; i++) w(0, SRC + i * 4, 0x15151515);
    w(0, 0x4000400, 0x15151515); gxDrain(); frame(); EQ(c->gpu3D.fifoHalfEmpty(), 1);
    for (int ch = 0; ch < 4; ch++) {
        arm(0, ch, C(0xA, 0, 2, BIT(30)), 1000, 400, SRC, 0x4000400);
        EQ(c->ndma[0].runMask, BIT(ch)); // Starts by itself with room in the FIFO
        c->ndma[0].update();
        EQ(c->ndma[0].srcAddrs[ch], SRC + 112 * 4); EQ(c->ndma[0].blockLeft[ch], 400 - 112); EQ(c->ndma[0].totalLeft[ch], 1000 - 112); EQ(irq(0, ch), 0);
        FILE *f = tmpfile(); c->ndma[0].saveState(f); uint32_t tl = c->ndma[0].totalLeft[ch], bl = c->ndma[0].blockLeft[ch]; c->ndma[0].totalLeft[ch] = 7; c->ndma[0].blockLeft[ch] = 7;
        rewind(f); c->ndma[0].loadState(f); fclose(f); EQ(c->ndma[0].totalLeft[ch], tl); EQ(c->ndma[0].blockLeft[ch], bl);
        for (int i = 0; i < 40 && (r(0, R(ch, CNT)) >> 31); i++) frame();
        EQ(r(0, R(ch, CNT)) >> 31, 0); EQ(c->ndma[0].srcAddrs[ch], SRC + 4000); EQ(irq(0, ch), 1); EQ(r(0, R(ch, TCNT)), 1000);
        gxDrain(); EQ(c->gpu3D.gxStat >> 26 & 1, 1);
    }
    // Words that leave the FIFO under half full: nothing will trigger the channel again, so it has to carry on by itself
    for (uint32_t i = 0; i < 1000; i++) w(0, SRC + i * 4, 0x00000015);
    arm(0, 1, C(0xA, 0, 2), 900, 400, SRC, 0x4000400); c->ndma[0].update();
    EQ(c->ndma[0].blockLeft[1], 400 - 112); EQ(c->gpu3D.fifoHalfEmpty(), 1); EQ(c->ndma[0].runMask, BIT(1)); // Partway through a block
    for (int i = 0; i < 40 && (r(0, R(1, CNT)) >> 31); i++) frame();
    EQ(r(0, R(1, CNT)) >> 31, 0); EQ(c->ndma[0].srcAddrs[1], SRC + 3600); gxDrain();
    arm(0, 3, C(0xA, 0, 2), 30, 10, SRC, 0x4000400); c->ndma[0].update();
    EQ(c->ndma[0].blockLeft[3], 0); EQ(c->ndma[0].totalLeft[3], 20); EQ(c->ndma[0].runMask, BIT(3)); // Between blocks
    for (int i = 0; i < 40 && (r(0, R(3, CNT)) >> 31); i++) frame();
    EQ(r(0, R(3, CNT)) >> 31, 0); EQ(c->ndma[0].srcAddrs[3], SRC + 120); gxDrain();
    for (uint32_t i = 0; i < 1000; i++) w(0, SRC + i * 4, 0x15151515);
    // ARM7 mode 0xA is AES, so GXFIFO events and rules must not touch it
    clear(); arm(1, 0, C(0xA), 0xFF, 1); EQ(c->ndma[1].runMask, 0); frame(1); off(1, 0); EQ(count(), 0);
    // A full FIFO waits
    for (int i = 0; i < 60; i++) w(0, 0x4000400, 0x15151515); // 240 entries, as a CPU would stop at 256
    EQ(c->gpu3D.fifoHalfEmpty(), 0);
    arm(0, 2, C(0xA, 0, 2), 10, 10, SRC, 0x4000400); EQ(c->ndma[0].runMask, 0);
    for (int i = 0; i < 40 && (r(0, R(2, CNT)) >> 31); i++) frame();
    EQ(r(0, R(2, CNT)) >> 31, 0); EQ(c->ndma[0].srcAddrs[2], SRC + 40); gxDrain();

    // DS cart
    FILE *rom = fopen(argv[2], "rb"); static uint8_t romData[0x200]; fseek(rom, 0x100000, SEEK_SET); fread(romData, 1, 0x200, rom); fclose(rom);
    for (int cpu = 0; cpu < 2; cpu++) {
        snprintf(tag, sizeof tag, "cart arm%d", cpu ? 7 : 9);
        uint16_t ex = c->memory.read<uint16_t>(0, 0x4000204, false);
        wh(0, 0x4000204, cpu ? (ex | BIT(11)) : (ex & ~BIT(11)));
        clear();
        arm(cpu, 1, C(4, 2, 0, BIT(30)), 0x80, 1, 0x4100010, DST);
        wh(cpu, 0x40001A0, 0x8000);
        w(cpu, 0x40001A8, 0xB7 | (0x00 << 8) | (0x10 << 16) | (0x00 << 24)); w(cpu, 0x40001AC, 0);
        w(cpu, 0x40001A4, 0xA1000000);
        frame(2);
        EQ(r(cpu, R(1, CNT)) >> 31, 0); EQ(irq(cpu, 1), 1);
        EQ(c->ndma[cpu].srcAddrs[1], 0x4100010);
        int diff = 0; for (int i = 0; i < 0x200; i += 4) { uint32_t want; memcpy(&want, romData + i, 4); diff += r(0, DST + i) != want; }
        EQ(diff, 0); EQ(r(cpu, 0x40001A4) >> 31, 0);
        // Enabled after the transfer started, while a word is on its way
        clear();
        w(cpu, 0x40001A8, 0xB7 | (0x00 << 8) | (0x10 << 16) | (0x00 << 24)); w(cpu, 0x40001AC, 0);
        w(cpu, 0x40001A4, 0xA1000000);
        arm(cpu, 1, C(4, 2, 0), 0x80, 1, 0x4100010, DST);
        frame(2);
        EQ(r(cpu, R(1, CNT)) >> 31, 0); diff = 0; for (int i = 0; i < 0x200; i += 4) { uint32_t want; memcpy(&want, romData + i, 4); diff += r(0, DST + i) != want; }
        EQ(diff, 0); EQ(r(cpu, 0x40001A4) >> 31, 0);
    }
    printf("%d checks, %d failed\n", checks, fails);
    fflush(stdout);
    _exit(fails != 0);
}
