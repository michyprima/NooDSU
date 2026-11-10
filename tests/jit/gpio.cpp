// Checks the DSi's GPIO pins and the touchscreen controller's headset detection with both CPUs halted, through the real
// registers: the headphone pin following the GamePad's jack, its interrupt on either edge, and the codec's headset flags
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
static const uint32_t GPIO_DATA = 0x4004C00, GPIO_DIR = 0x4004C01, GPIO_IEDGE = 0x4004C02, GPIO_IE = 0x4004C03, GPIO_WIFI = 0x4004C04,
    SPICNT = 0x40001C0, SPIDATA = 0x40001C2;
static uint8_t rb(uint32_t a, int cpu = 1) { return c->memory.read<uint8_t>(cpu, a, false); }
static uint16_t rh(uint32_t a) { return c->memory.read<uint16_t>(1, a, false); }
static void wb(uint32_t a, uint8_t v) { c->memory.write<uint8_t>(1, a, v, false); }
static void wh(uint32_t a, uint16_t v) { c->memory.write<uint16_t>(1, a, v, false); }
static void frame(int n = 1) { while (n--) do c->runCore(); while (!c->frameEnded.exchange(false)); }
static bool irq() { return c->interpreter[1].irf & BITL(32 + 5); }
static void noIrq() { c->interpreter[1].irf &= ~BITL(32 + 5); }
static void plug(bool in) { DsiGpio::headphonesIn.store(in); frame(1); }

// One transfer with the touchscreen controller: a register index, then a byte to write or a dummy to read
static uint8_t tsc(uint8_t index, uint8_t value) {
    wh(SPICNT, 0x8A00); wh(SPIDATA, index);
    wh(SPICNT, 0x8200); wh(SPIDATA, value);
    return rh(SPIDATA);
}
static void tscWrite(int page, int reg, uint8_t value) { tsc(0x00, page); tsc(reg << 1, value); }
static uint8_t tscRead(int page, int reg) { tsc(0x00, page); return tsc((reg << 1) | 1, 0); }

static void boot(bool dsi, bool headphones) {
    delete c; c = nullptr;
    Settings::dsiMode = dsi;
    DsiGpio::headphonesIn.store(headphones);
    try { c = new Core(""); } catch (CoreError e) { printf("core error %d\n", int(e)); exit(2); }
    for (int cpu = 0; cpu < 2; cpu++) { c->interpreter[cpu].halt(0); c->memory.write<uint32_t>(cpu, 0x4000208, 0, false); c->memory.write<uint32_t>(cpu, 0x4000210, 0, false); }
    frame(2);
}

int main(int argc, char **argv) {
    std::string files = argv[1];
    Settings::load(".");
    Settings::ndsBios9Path = files + "/bios9.bin"; Settings::ndsBios7Path = files + "/bios7.bin"; Settings::ndsFirmPath = files + "/firmware.bin";
    Settings::dsiBios9Path = files + "/bios9i.bin"; Settings::dsiBios7Path = files + "/bios7i.bin"; Settings::dsiFirmPath = files + "/firmwarei.bin";
    Settings::dsiNandPath = "nand.bin"; Settings::sdImagePath = "missing.img"; Settings::jit = 0; Settings::directBoot = 0; Settings::savesFolder = 1;
    Settings::threaded2D = 0; Settings::threaded3D = 0; Settings::arm7Hle = 0; Settings::emulateAudio = 0; Settings::fast2D = 0; Settings::highRes3D = 0; Settings::hardware3D = 0; Settings::fpsLimiter = 0;
    Rtc::fixedTime = 1767225600;

    // The pins as inputs: GPIO18 and the testpoint, power button and sound enable lines idle high, the headphone pin
    // shows the jack, and the state at power on is taken without an interrupt
    for (bool hp : { false, true }) {
        snprintf(tag, sizeof tag, "power on %d", hp);
        boot(true, hp);
        EQ(rb(GPIO_DATA), hp ? 0xF7 : 0xD7);
        EQ(irq(), 0);
    }
    strcpy(tag, "regs");
    wb(GPIO_DIR, 0xFF); EQ(rb(GPIO_DIR), 0xF7);
    wb(GPIO_IEDGE, 0xFF); EQ(rb(GPIO_IEDGE), 0xF7);
    wb(GPIO_IE, 0xFF); EQ(rb(GPIO_IE), 0xF7);
    wh(GPIO_WIFI, 0xFFFF); EQ(rh(GPIO_WIFI), 0x0101);
    EQ(rb(GPIO_DATA, 0), 0); // The ARM9 doesn't have them

    // Outputs give the value written ANDed with the line, so the headphone pin as an output reads low without headphones
    strcpy(tag, "outputs");
    wb(GPIO_IE, 0); wb(GPIO_DIR, 0x80); wb(GPIO_DATA, 0x00); EQ(rb(GPIO_DATA), 0x77);
    wb(GPIO_DATA, 0x80); EQ(rb(GPIO_DATA), 0xF7);
    wb(GPIO_DIR, 0xA0); wb(GPIO_DATA, 0xA0); EQ(rb(GPIO_DATA), 0xF7);
    plug(false); EQ(rb(GPIO_DATA), 0xD7);
    wb(GPIO_DIR, 0x80);

    // The interrupt comes on the selected edge only: rising for plugging in, falling for unplugging
    for (int edge : { 0, 1 }) for (bool enabled : { false, true }) {
        snprintf(tag, sizeof tag, "irq edge %d enabled %d", edge, enabled);
        plug(false); noIrq();
        wb(GPIO_IEDGE, edge << 5); wb(GPIO_IE, enabled << 5);
        plug(true);
        EQ(rb(GPIO_DATA) & 0x20, 0x20);
        EQ(irq(), enabled && edge == 1); noIrq();
        plug(true); EQ(irq(), 0); // No change, no interrupt
        plug(false);
        EQ(rb(GPIO_DATA) & 0x20, 0);
        EQ(irq(), enabled && edge == 0); noIrq();
    }
    strcpy(tag, "irq other pins");
    wb(GPIO_IE, 0xD7); wb(GPIO_IEDGE, 0xD7); plug(true); EQ(irq(), 0); plug(false);

    // The codec's headset detection: nothing while off, then the state, a sticky change flag cleared by reading, and the
    // GPI2 pin unless its input is powered down
    strcpy(tag, "codec off");
    plug(true);
    EQ(tscRead(0, 0x43), 0x00);
    EQ(tscRead(0, 0x2E), 0x00);
    plug(false); plug(true);
    EQ(tscRead(0, 0x2C), 0x00);
    strcpy(tag, "codec on");
    tscWrite(0, 0x43, 0xFF);
    EQ(tscRead(0, 0x43), 0xBF); // Enabled, headset without a microphone, debounce bits kept
    EQ(tscRead(0, 0x2E), 0x10);
    EQ(tscRead(0, 0x2C), 0x00);
    plug(false);
    EQ(tscRead(0, 0x43), 0x9F);
    EQ(tscRead(0, 0x2E), 0x00);
    EQ(tscRead(0, 0x2C), 0x10);
    EQ(tscRead(0, 0x2C), 0x00);
    plug(true);
    EQ(tscRead(0, 0x2C), 0x10);
    strcpy(tag, "codec pins");
    EQ(tscRead(0, 0x39), 0x00);
    tscWrite(0, 0x39, 0x06); EQ(tscRead(0, 0x39), 0x07);
    plug(false); EQ(tscRead(0, 0x39), 0x06);
    tscWrite(0, 0x3A, 0xFF); EQ(tscRead(0, 0x3A), 0x60);
    tscWrite(0, 0x30, 0x80); EQ(tscRead(0, 0x30), 0x80);
    tscWrite(0, 0x31, 0x41); EQ(tscRead(0, 0x31), 0x41);
    EQ(tscRead(0, 0x44), 0x00); // Others still read 0
    EQ(tscRead(3, 0x02), 0x18); // Page 3 is untouched

    // Everything survives a save state
    strcpy(tag, "state");
    wb(GPIO_DIR, 0x80); wb(GPIO_IEDGE, 0x20); wb(GPIO_IE, 0x20); plug(true);
    FILE *f = tmpfile(); c->dsiGpio.saveState(f); c->spi.saveState(f);
    wb(GPIO_DIR, 0); wb(GPIO_IE, 0); tscWrite(0, 0x43, 0); DsiGpio::headphonesIn.store(false); c->dsiGpio.headphones = false;
    rewind(f); c->dsiGpio.loadState(f); c->spi.loadState(f); fclose(f);
    DsiGpio::headphonesIn.store(true);
    EQ(rb(GPIO_DIR), 0x80); EQ(rb(GPIO_IE), 0x20); EQ(rb(GPIO_DATA) & 0x20, 0x20); EQ(tscRead(0, 0x43), 0xBF);

    // In DS mode nothing changes
    strcpy(tag, "ds mode");
    boot(false, false);
    plug(true);
    EQ(rb(GPIO_DATA), 0);
    EQ(c->interpreter[1].irf & BITL(32 + 5), 0);

    printf("%d of %d checks passed\n", checks - fails, checks);
    return fails != 0;
}
