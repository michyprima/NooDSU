// Checks what the DSi's SD/MMC controller reports about its cards, with both CPUs halted, through the real registers: the CSD
// of the NAND and of standard- and high-capacity SD cards, the SCR, the SD status, and an empty SD slot
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
static const uint32_t SD_CMD = 0x4004800, SD_PORT_SELECT = 0x4004802, SD_CMD_PARAM = 0x4004804, SD_RESPONSE = 0x400480C,
    SD_IRQ_STATUS = 0x400481C, SD_IRQ_MASK = 0x4004820, SD_CARD_OPTION = 0x4004828, SD_SOFT_RESET = 0x40048E0, SD_DATA16_BLKLEN = 0x4004826, SD_DATA16_FIFO = 0x4004830, SDIO_IRQ_STATUS = 0x4004A1C;
static uint32_t r(uint32_t a) { return c->memory.read<uint32_t>(1, a, false); }
static uint16_t rh(uint32_t a) { return c->memory.read<uint16_t>(1, a, false); }
static void w(uint32_t a, uint32_t v) { c->memory.write<uint32_t>(1, a, v, false); }
static void wh(uint32_t a, uint16_t v) { c->memory.write<uint16_t>(1, a, v, false); }
static bool irq() { return c->interpreter[1].irf & BITL(40); }
static void noIrq() { c->interpreter[1].irf &= ~BITL(40); }
static void reset() { wh(SD_SOFT_RESET, 6); wh(SD_SOFT_RESET, 7); }
static void frame(int n = 1) { while (n--) do c->runCore(); while (!c->frameEnded.exchange(false)); }

static void boot(const char *sd) {
    // Start a core with the given SD image, or none, and halt both CPUs so only the checks touch the controller
    delete c; c = nullptr;
    Settings::sdImagePath = sd;
    try { c = new Core(""); } catch (CoreError e) { printf("core error %d\n", int(e)); exit(2); }
    for (int cpu = 0; cpu < 2; cpu++) { c->interpreter[cpu].halt(0); c->memory.write<uint32_t>(cpu, 0x4000208, 0, false); c->memory.write<uint32_t>(cpu, 0x4000210, 0, false); }
    frame(2);
}

static void cmd(int port, int index, uint32_t param = 0, bool app = false) {
    // Send a command to a port, clearing the interrupt bits first
    wh(SD_PORT_SELECT, port); w(SD_IRQ_STATUS, 0); w(SD_CMD_PARAM, param);
    wh(SD_CMD, index | (app << 6));
}

// The 128-bit CSD from the response registers, which hold its bits 127-8 in bits 119-0
static uint32_t csdBits(int hi, int lo) {
    uint32_t value = 0;
    for (int i = hi; i >= lo; i--)
        value = (value << 1) | ((r(SD_RESPONSE + ((i - 8) >> 5) * 4) >> ((i - 8) & 31)) & 1);
    return value;
}

static int64_t capacity() {
    // Work out the capacity the way a driver does, from either CSD version
    if (csdBits(127, 126) == 1) return (int64_t(csdBits(69, 48)) + 1) << 19;
    return (int64_t(csdBits(73, 62)) + 1) << (csdBits(49, 47) + 2 + csdBits(83, 80));
}

static void readData(uint8_t *data, int size) {
    // Read data sent after a command from the 16-bit FIFO, its first byte lowest
    for (int i = 0; i < size; i += 2) { uint16_t v = rh(SD_DATA16_FIFO); data[i] = v; data[i + 1] = v >> 8; }
}

static void makeImage(const char *path, int64_t size) {
    // Make a sparse image of a size
    FILE *f = fopen(path, "wb"); fseeko(f, size - 1, SEEK_SET); fputc(0, f); fclose(f);
}

int main(int argc, char **argv) {
    std::string files = argv[1];
    Settings::load(".");
    Settings::ndsBios9Path = files + "/bios9.bin"; Settings::ndsBios7Path = files + "/bios7.bin"; Settings::ndsFirmPath = files + "/firmware.bin";
    Settings::dsiBios9Path = files + "/bios9i.bin"; Settings::dsiBios7Path = files + "/bios7i.bin"; Settings::dsiFirmPath = files + "/firmwarei.bin";
    Settings::dsiNandPath = "nand.bin"; Settings::jit = 0; Settings::dsiMode = 1; Settings::directBoot = 0; Settings::savesFolder = 1;
    Settings::threaded2D = 0; Settings::threaded3D = 0; Settings::emulateAudio = 0; Settings::fast2D = 0; Settings::highRes3D = 0; Settings::hardware3D = 0; Settings::fpsLimiter = 0;
    Rtc::fixedTime = 1767225600;
    makeImage("sd64m.img", 64 << 20);
    makeImage("sd1g.img", 1 << 30);
    makeImage("sd2g.img", (2LL << 30) + (300 << 10)); // Not a whole number of 512KB units
    makeImage("sd8g.img", 8LL << 30);

    // The NAND's CSD is a real DSi eMMC's, which describes its 240MB exactly
    boot("sd64m.img");
    strcpy(tag, "nand csd");
    cmd(1, 9);
    EQ(rh(SD_IRQ_STATUS) & 0x1, 1);
    uint32_t real[4] = { 0xE9964040, 0xDFF6DB7F, 0x2A0F5901, 0x00902601 };
    for (int i = 0; i < 4; i++) EQ(r(SD_RESPONSE + i * 4), real[i]);
    EQ(csdBits(127, 126), 2); // MMC
    EQ(capacity(), 0xF000000);

    // Standard capacity: CSD version 1.0 in units of 256KB
    struct { const char *path; int64_t size; int ver; } cards[] = {
        { "sd64m.img", 64 << 20, 0 }, { "sd1g.img", 1 << 30, 0 }, { "sd2g.img", 2LL << 30, 1 }, { "sd8g.img", 8LL << 30, 1 } };
    for (auto &card : cards) {
        snprintf(tag, sizeof tag, "csd %s", card.path);
        boot(card.path);
        cmd(0, 9);
        EQ(rh(SD_IRQ_STATUS) & 0x1, 1);
        EQ(r(SD_RESPONSE + 12) >> 24, 0); // The CRC byte is dropped, so the top byte is empty
        EQ(csdBits(127, 126), card.ver);
        EQ(capacity(), card.size);
        EQ(csdBits(83, 80), 9); // READ_BL_LEN
        EQ(csdBits(25, 22), 9); // WRITE_BL_LEN
        EQ(csdBits(103, 96), 0x32); // TRAN_SPEED

        // The OCR's capacity bit agrees with the CSD
        cmd(0, 55); cmd(0, 41, 0x40FF8000, true);
        EQ((r(SD_RESPONSE) >> 30) & 1, card.ver);

        // The SCR comes through the FIFO, version 2.00 with 1- and 4-bit buses
        snprintf(tag, sizeof tag, "scr %s", card.path);
        wh(SD_DATA16_BLKLEN, 8);
        cmd(0, 55); cmd(0, 51, 0, true);
        frame(1);
        uint8_t scr[8]; readData(scr, 8);
        EQ(scr[0], 0x02);
        EQ(scr[1], card.ver ? 0x35 : 0x25);
        EQ(rh(SD_IRQ_STATUS) & 0x4, 0x4); // Data end

        // The SD status gives the bus width last set with ACMD6
        for (int width : { 0, 2 }) {
            snprintf(tag, sizeof tag, "status %s width %d", card.path, width);
            cmd(0, 55); cmd(0, 6, width, true);
            wh(SD_DATA16_BLKLEN, 64);
            cmd(0, 55); cmd(0, 13, 0, true);
            frame(1);
            uint8_t status[64]; readData(status, 64);
            EQ(status[0] >> 6, width);
            EQ(status[8], 0x02);
            EQ(status[10] >> 4, 9);
            EQ(rh(SD_IRQ_STATUS) & 0x4, 0x4);
        }

        // A card's slot shows it as present and writable
        snprintf(tag, sizeof tag, "present %s", card.path);
        EQ(rh(SD_IRQ_STATUS) & 0xA0, 0xA0);
    }

    // A card is reported inserted at power on, and again once its detect time has passed after the controller is reset;
    // the time is 400h HCLKs shifted by SD_CARD_OPTION's low bits, or 100h for 0xF
    strcpy(tag, "detect");
    boot("sd64m.img");
    EQ(rh(SD_IRQ_STATUS) & 0x10, 0x10);
    EQ(rh(SD_CARD_OPTION), 0x40EE);
    EQ(rh(SD_SOFT_RESET), 7);
    w(SD_IRQ_MASK, 0x8B7F030D); // Only the insert interrupt
    cmd(1, 13); wh(SD_CARD_OPTION, 0x40EA);
    reset();
    EQ(rh(SD_IRQ_STATUS), 0xA0); // Reset acknowledges everything, the command end and the insert too
    EQ(rh(SD_CARD_OPTION), 0x40EE);
    wh(SD_CARD_OPTION, 0x40EA); noIrq();
    EQ(c->sdMmc.detectAt - c->globalCycles, 0x100000);
    frame(1); EQ(rh(SD_IRQ_STATUS) & 0x10, 0); EQ(irq(), 0);
    frame(1); EQ(rh(SD_IRQ_STATUS) & 0x10, 0x10); EQ(irq(), 1);
    w(SD_IRQ_STATUS, ~0x10); EQ(rh(SD_IRQ_STATUS) & 0x10, 0);
    for (int n : { 0, 5, 0xE, 0xF }) {
        snprintf(tag, sizeof tag, "detect time %X", n);
        reset(); wh(SD_CARD_OPTION, 0x40E0 | n);
        EQ(c->sdMmc.detectAt - c->globalCycles, (n == 0xF) ? 0x100 : (0x400 << n));
        frame(n == 0xE ? 31 : 1);
        EQ(rh(SD_IRQ_STATUS) & 0x10, 0x10);
        w(SD_IRQ_STATUS, ~0x10);
    }

    // Changing the detect time while detecting restarts the wait; resetting again stops it
    strcpy(tag, "detect change");
    reset(); wh(SD_CARD_OPTION, 0x40EE); frame(2);
    EQ(rh(SD_IRQ_STATUS) & 0x10, 0);
    wh(SD_CARD_OPTION, 0x40E9); frame(1);
    EQ(rh(SD_IRQ_STATUS) & 0x10, 0x10);
    w(SD_IRQ_STATUS, ~0x10);
    strcpy(tag, "detect stop");
    reset(); wh(SD_CARD_OPTION, 0x40EA); wh(SD_SOFT_RESET, 6); frame(40);
    EQ(rh(SD_IRQ_STATUS) & 0x10, 0);
    wh(SD_SOFT_RESET, 7); wh(SD_CARD_OPTION, 0x40EA); frame(4);
    EQ(rh(SD_IRQ_STATUS) & 0x10, 0x10);

    // A detect in progress survives a save state
    strcpy(tag, "detect state");
    reset(); wh(SD_CARD_OPTION, 0x40EA);
    FILE *f = tmpfile(); c->saveState(f); c->sdMmc.saveState(f); frame(4); w(SD_IRQ_STATUS, ~0x10);
    rewind(f); c->loadState(f); c->sdMmc.loadState(f); fclose(f);
    EQ(rh(SD_IRQ_STATUS) & 0x10, 0); EQ(rh(SD_CARD_OPTION), 0x40EA);
    frame(4);
    EQ(rh(SD_IRQ_STATUS) & 0x10, 0x10);

    // A multi-block write as Flipnote Studio's backup does it: the first block goes into the 32-bit FIFO before the
    // command, and NDMA, already enabled, sends the rest block by block once the command asks for more
    for (int blocks : { 1, 2, 4 }) {
        snprintf(tag, sizeof tag, "write %d", blocks);
        boot("sd64m.img");
        const uint32_t SRC = 0x2300000, ADDR = 0x40000 + blocks * 0x10000; // Away from what earlier cores wrote back on exit
        for (int i = 0; i < blocks * 128; i++) w(SRC + i * 4, 0x11110000 * (i / 128 + 1) + i);
        cmd(0, 16, 0x200);
        wh(0x4004900, 0x0402); wh(0x4004904, 0x200); wh(0x40048D8, 0x2); wh(0x400480A, blocks);
        for (int i = 0; i < 128; i++) w(0x400490C, r(SRC + i * 4));
        if (blocks > 1) {
            w(0x4004104, SRC + 0x200); w(0x4004108, 0x400490C); w(0x400410C, (blocks - 1) * 128); w(0x4004110, 128);
            w(0x4004114, 1); w(0x400411C, 0xC8000880); // Enabled with an interrupt, mode 8 (SD/MMC), fixed destination
        }
        cmd(0, 25, ADDR);
        frame(2);
        EQ(rh(SD_IRQ_STATUS) & 0x4, 0x4); // Data end
        EQ(r(0x400411C) >> 31, 0); // NDMA finished

        // Reading the blocks back gives the new data straight away, from the cache
        wh(0x4004900, 0x0002); wh(0x40048D8, 0x0); wh(SD_DATA16_BLKLEN, 0x200); wh(0x400480A, blocks);
        cmd(0, 18, ADDR);
        int badRead = 0;
        for (int i = 0; i < blocks * 128; i++) {
            if (i % 128 == 0) frame(1);
            uint32_t v = rh(SD_DATA16_FIFO); v |= rh(SD_DATA16_FIFO) << 16;
            badRead += v != 0x11110000 * (i / 128 + 1) + i;
        }
        EQ(badRead, 0);

        // The file only gets it when the cache is written back, about half a second later, in one go
        FILE *img = fopen("sd64m.img", "rb"); fseek(img, ADDR, SEEK_SET); uint8_t before[4]; fread(before, 1, 4, img); fclose(img);
        EQ(before[0] | before[1] | before[2] | before[3], 0);
        frame(40);
        img = fopen("sd64m.img", "rb"); fseek(img, ADDR, SEEK_SET);
        int bad = 0;
        for (int i = 0; i < blocks * 128; i++) {
            uint8_t b[4]; fread(b, 1, 4, img);
            bad += (b[0] | b[1] << 8 | b[2] << 16 | uint32_t(b[3]) << 24) != 0x11110000 * (i / 128 + 1) + i;
        }
        uint8_t after[4]; fread(after, 1, 4, img); fclose(img);
        EQ(bad, 0);
        EQ(after[0] | after[1] | after[2] | after[3], 0); // Nothing past the last block
    }

    // Writing back the NAND's last chunk leaves the footer after it alone, and a write past the end of the SD image
    // doesn't make the file bigger
    strcpy(tag, "write back ends");
    {
        auto size = [](const char *path) { FILE *f = fopen(path, "rb"); fseeko(f, 0, SEEK_END); int64_t n = ftello(f); fclose(f); return n; };
        uint8_t footer[0x40], after[0x40];
        FILE *f = fopen("nand.bin", "rb"); fseeko(f, -0x40, SEEK_END); fread(footer, 1, 0x40, f); fclose(f);
        int64_t nandSize = size("nand.bin"), sdSize = size("sd64m.img");
        boot("sd64m.img");
        wh(0x4004900, 0x0002); wh(0x40048D8, 0x0); wh(SD_DATA16_BLKLEN, 0x200);
        for (int port = 1; port >= 0; port--) {
            cmd(port, 16, 0x200); wh(0x400480A, 1);
            cmd(port, 24, port ? 0xEFFFE00 : 0x4000000); // The NAND's last sector, and just past the 64MB image
            for (int i = 0; i < 256; i++) wh(SD_DATA16_FIFO, 0xA5A5);
            frame(1);
        }
        frame(40);
        f = fopen("nand.bin", "rb"); fseeko(f, -0x40, SEEK_END); fread(after, 1, 0x40, f);
        fseeko(f, 0xEFFFE00, SEEK_SET); uint8_t last[2]; fread(last, 1, 2, f); fclose(f);
        EQ(memcmp(footer, after, 0x40), 0);
        EQ(last[0] << 8 | last[1], 0xA5A5);
        EQ(size("nand.bin"), nandSize);
        EQ(size("sd64m.img"), sdSize);
    }

    // With no SD image the slot is empty, its commands time out, and the NAND and Wi-Fi chip still answer
    strcpy(tag, "no card");
    boot("missing.img");
    EQ(rh(SD_IRQ_STATUS) & 0xB8, 0);
    w(SD_IRQ_STATUS, 0); EQ(rh(SD_IRQ_STATUS) & 0xA0, 0);
    reset(); wh(SD_CARD_OPTION, 0x40E0); frame(2);
    EQ(rh(SD_IRQ_STATUS) & 0x18, 0); // Never detected
    for (int index : { 0, 2, 8, 9, 13, 17, 55 }) {
        cmd(0, index);
        EQ(r(SD_IRQ_STATUS) & 0x400001, 0x400000);
    }
    cmd(0, 41, 0, true);
    EQ(r(SD_IRQ_STATUS) & 0x400001, 0x400000);
    cmd(1, 9);
    EQ(r(SD_IRQ_STATUS) & 0x400001, 0x1);
    EQ(capacity(), 0xF000000);
    EQ(rh(SDIO_IRQ_STATUS) & 0xA0, 0xA0);

    // The bus width survives a save state
    strcpy(tag, "state");
    boot("sd64m.img");
    cmd(0, 55); cmd(0, 6, 2, true);
    f = tmpfile(); c->sdMmc.saveState(f); c->sdMmc.busWidth = 0; rewind(f); c->sdMmc.loadState(f); fclose(f);
    EQ(c->sdMmc.busWidth, 2);

    for (const char *p : { "sd64m.img", "sd1g.img", "sd2g.img", "sd8g.img" }) remove(p);
    printf("%d of %d checks passed\n", checks - fails, checks);
    return fails != 0;
}
