// Checks the GBA slot in DS mode with both CPUs halted: what an empty slot reads, EXMEMCNT handing it between the CPUs,
// each accessory through the real memory paths, and the GPIO pins of GBA carts with a solar sensor or a rumble motor,
// in DS mode and in GBA mode. The expected values come from GBATEK, DeSmuME and melonDS, not a real DS
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
static uint8_t rb(int cpu, uint32_t a) { return c->memory.read<uint8_t>(cpu, a, false); }
static uint16_t rh(int cpu, uint32_t a) { return c->memory.read<uint16_t>(cpu, a, false); }
static uint32_t r(int cpu, uint32_t a) { return c->memory.read<uint32_t>(cpu, a, false); }
static void wb(int cpu, uint32_t a, uint8_t v) { c->memory.write<uint8_t>(cpu, a, v, false); }
static void wh(int cpu, uint32_t a, uint16_t v) { c->memory.write<uint16_t>(cpu, a, v, false); }
static void w(int cpu, uint32_t a, uint32_t v) { c->memory.write<uint32_t>(cpu, a, v, false); }

// Give the slot to a CPU and set its ROM access time (0-3 = 10, 8, 6, 18 cycles) in its own register
static void slot(int cpu, int timing) {
    wh(0, 0x4000204, (cpu ? BIT(7) : 0) | (timing << 2));
    if (cpu) wh(1, 0x4000204, timing << 2);
}

static void boot(std::string nds, std::string gba, int device) {
    delete c;
    Settings::slot2Device = device;
    try { c = new Core(nds, gba); } catch (CoreError e) { printf("core error %d\n", int(e)); exit(2); }
    if (!c->gbaMode)
        for (int cpu = 0; cpu < 2; cpu++) c->interpreter[cpu].halt(0);
    c->slot2.takeRumble();
}

// Read the solar sensor the way GBATEK's example does, counting clocks until pin 3 goes high
static int solar(int cpu, int level) {
    Settings::sunlight = level;
    wh(cpu, 0x80000C8, 1); wh(cpu, 0x80000C6, 7); wh(cpu, 0x80000C4, 2); wh(cpu, 0x80000C4, 0);
    int count = 0;
    while (count < 0x100) {
        wh(cpu, 0x80000C4, 1); wh(cpu, 0x80000C4, 0);
        if (rh(cpu, 0x80000C4) & 8) break;
        count++;
    }
    return count;
}

// Make a small GBA ROM with a game code, a recognizable pattern, and the RTC string carts with one have
static std::string gbaRom(const char *code) {
    std::vector<uint8_t> rom(0x1000);
    for (size_t i = 0x100; i < rom.size(); i++) rom[i] = i * 7;
    memcpy(&rom[0xA0], "TEST", 4); memcpy(&rom[0xAC], code, 4); rom[0xB2] = 0x96;
    memcpy(&rom[0x800], "SIIRTC_V001", 11);
    for (int i = 0xC4; i < 0xCA; i++) rom[i] = 0;
    char cwd[1024];
    std::string path = std::string(getcwd(cwd, sizeof(cwd))) + "/slot2_" + code + ".gba";
    FILE *f = fopen(path.c_str(), "wb"); fwrite(rom.data(), 1, rom.size(), f); fclose(f);
    return path;
}

int main(int argc, char **argv) {
    std::string files = argv[1], nds = argv[2];
    Settings::load(".");
    Settings::ndsBios9Path = files + "/bios9.bin"; Settings::ndsBios7Path = files + "/bios7.bin"; Settings::ndsFirmPath = files + "/firmware.bin";
    Settings::gbaBiosPath = files + "/gba_bios.bin";
    Settings::jit = 0; Settings::dsiMode = 0; Settings::directBoot = 1; Settings::savesFolder = 1;
    Settings::threaded2D = 0; Settings::threaded3D = 0; Settings::emulateAudio = 0; Settings::fast2D = 0; Settings::highRes3D = 0; Settings::hardware3D = 0; Settings::fpsLimiter = 0;
    Rtc::fixedTime = 1767225600;

    // An empty slot reads the halfword address, garbage at 10 cycles or all high at 18; the other CPU reads zeros
    boot(nds, "", SLOT2_NONE);
    for (int cpu = 0; cpu < 2; cpu++) {
        snprintf(tag, sizeof tag, "empty arm%d", cpu ? 7 : 9);
        slot(cpu, 2);
        EQ(rh(cpu, 0x8000000), 0); EQ(rh(cpu, 0x8000002), 1); EQ(rh(cpu, 0x8123456), 0x1A2B); EQ(rh(cpu, 0x9FFFFFE), 0xFFFF);
        EQ(r(cpu, 0x8000010), 0x00090008); EQ(rb(cpu, 0x8000201), 0x01); EQ(rb(cpu, 0x8000200), 0x00);
        EQ(rb(cpu, 0xA000000), 0xFF); EQ(r(cpu, 0xA001234), 0xFFFFFFFF);
        EQ(rh(!cpu, 0x8000002), 0); EQ(r(!cpu, 0x9000000), 0); EQ(rb(!cpu, 0xA000000), 0);
        slot(cpu, 1); EQ(rh(cpu, 0x8000002), 1);
        slot(cpu, 0); EQ(rh(cpu, 0x8000002), 0xFE09);
        slot(cpu, 3); EQ(rh(cpu, 0x8000002), 0xFFFF);
    }
    snprintf(tag, sizeof tag, "exmemcnt");
    slot(1, 2); EQ(rh(1, 0x4000204) & 0x80, 0x80); EQ(rh(0, 0x4000204) & 0x80, 0x80);

    // The Rumble Pak pulls address line 1 low and moves when bit 1 of what's written changes
    boot(nds, "", SLOT2_RUMBLE);
    for (int cpu = 0; cpu < 2; cpu++) {
        snprintf(tag, sizeof tag, "rumble arm%d", cpu ? 7 : 9);
        slot(cpu, 2);
        int bad = 0;
        for (uint32_t i = 0; i < 0x1000; i++) bad += rh(cpu, 0x8000000 + i * 2) != (i & 0xFFFD);
        EQ(bad, 0);
        EQ(c->slot2.takeRumble(), false);
        wh(cpu, 0x8000000, 2); EQ(c->slot2.takeRumble(), true); EQ(c->slot2.takeRumble(), false);
        wh(cpu, 0x8000000, 2); EQ(c->slot2.takeRumble(), false);
        wh(cpu, 0x8000000, 1); EQ(c->slot2.takeRumble(), true);
        wb(cpu, 0x8000000, 2); EQ(c->slot2.takeRumble(), false);
        wh(!cpu, 0x8000000, 2); EQ(c->slot2.takeRumble(), false);
    }

    // The Memory Expansion Pak has ID values in ROM and 8MB of RAM at 0x9000000 that can be locked
    boot(nds, "", SLOT2_EXPANSION);
    for (int cpu = 0; cpu < 2; cpu++) {
        snprintf(tag, sizeof tag, "expansion arm%d", cpu ? 7 : 9);
        slot(cpu, 2);
        EQ(rh(cpu, 0x80000B0), 0xFFFF); EQ(rh(cpu, 0x80000B2), 0); EQ(rh(cpu, 0x80000B4), 0x2400); EQ(rh(cpu, 0x80000B6), 0x2424);
        EQ(rh(cpu, 0x80000BE), 0x7FFF); EQ(rh(cpu, 0x801FFFC), 0xFFFF); EQ(rh(cpu, 0x801FFFE), 0x7FFF); EQ(rh(cpu, 0x8000000), 0xFFFF);
        EQ(rh(cpu, 0x8240000), 1); EQ(rh(cpu, 0x8240002), 0);
        w(cpu, 0x9000000, 0x12345678 + cpu); w(cpu, 0x97FFFFC, 0xCAFEF00D); wb(cpu, 0x9001001, 0xAB); wh(cpu, 0x9400002, 0xBEEF);
        EQ(r(cpu, 0x9000000), 0x12345678 + cpu); EQ(r(cpu, 0x97FFFFC), 0xCAFEF00D); EQ(rb(cpu, 0x9001001), 0xAB);
        EQ(rh(cpu, 0x9400002), 0xBEEF); EQ(rh(cpu, 0x9800000), 0xFFFF); EQ(r(!cpu, 0x9000000), 0);
        wh(cpu, 0x8240000, 0); EQ(rh(cpu, 0x8240000), 0);
        EQ(r(cpu, 0x9000000), 0xFFFFFFFF); w(cpu, 0x9000000, 0); wb(cpu, 0x9000004, 0);
        wh(cpu, 0x8240000, 1); EQ(r(cpu, 0x9000000), 0x12345678 + cpu); EQ(rh(cpu, 0x9000004), 0xFFFF);
    }

    // The Guitar Grip reads 0xF9FF in ROM and its buttons in SRAM, low when pressed
    boot(nds, "", SLOT2_GUITAR);
    for (int cpu = 0; cpu < 2; cpu++) {
        snprintf(tag, sizeof tag, "guitar arm%d", cpu ? 7 : 9);
        slot(cpu, 2);
        EQ(rh(cpu, 0x8000000), 0xF9FF); EQ(rh(cpu, 0x9ABCDE0), 0xF9FF); EQ(rb(cpu, 0x8000001), 0xF9);
        c->slot2.setGuitarKeys(0); EQ(rb(cpu, 0xA000000), 0xFF);
        c->slot2.setGuitarKeys(BIT(0) | BIT(2)); EQ(rb(cpu, 0xA000000), 0xAF);
        c->slot2.setGuitarKeys(BIT(1) | BIT(3)); EQ(rb(cpu, 0xA000000), 0xD7); EQ(rh(cpu, 0xA000000), 0xD7D7);
        EQ(rb(!cpu, 0xA000000), 0);
    }

    // The Easy Piano reads 0xE7FF in ROM, with its keys at the end, low when pressed
    boot(nds, "", SLOT2_PIANO);
    for (int cpu = 0; cpu < 2; cpu++) {
        snprintf(tag, sizeof tag, "piano arm%d", cpu ? 7 : 9);
        slot(cpu, 2);
        c->slot2.setPianoKeys(0);
        EQ(rh(cpu, 0x8000000), 0xE7FF); EQ(rh(cpu, 0x9FFFFFE), 0xE7FF);
        c->slot2.setPianoKeys(BIT(0) | BIT(11) | BIT(12));
        EQ(rh(cpu, 0x9FFFFFE), 0x87FE); EQ(rb(cpu, 0x9FFFFFF), 0x87); EQ(rh(cpu, 0x9FFFFFC), 0xE7FF);
        c->slot2.setPianoKeys(0x7FF); EQ(rh(cpu, 0x9FFFFFE), 0xE000);
    }

    // The paddle reads 0xEFFF in ROM and its 12-bit position in SRAM; writing anything recalibrates it
    boot(nds, "", SLOT2_PADDLE);
    for (int cpu = 0; cpu < 2; cpu++) {
        snprintf(tag, sizeof tag, "paddle arm%d", cpu ? 7 : 9);
        slot(cpu, 2);
        wh(cpu, 0x8000000, 0);
        EQ(rh(cpu, 0x8000000), 0xEFFF); EQ(rb(cpu, 0xA000000), 0);
        c->slot2.turnPaddle(0x123);
        EQ(rb(cpu, 0xA000000), 0x23); EQ(rb(cpu, 0xA000001), 0x01); EQ(rh(cpu, 0xA000000), 0x2323); EQ(rb(cpu, 0xA000002), 0);
        c->slot2.turnPaddle(-0x124);
        EQ(rb(cpu, 0xA000000), 0xFF); EQ(rb(cpu, 0xA000001), 0x0F);
        wb(cpu, 0xA000000, 0); EQ(rb(cpu, 0xA000000), 0);
        c->slot2.turnPaddle(5); EQ(rb(cpu, 0xA000000), 5);
        wb(cpu, 0x8000000, 0); EQ(rb(cpu, 0xA000000), 0);
    }

    // Without a Boktai ROM, the solar sensor comes with a made-up header carrying the DS ROM's logo
    boot(nds, "", SLOT2_SOLAR);
    {
        uint8_t logo[0x9C] = {};
        FILE *f = fopen(nds.c_str(), "rb"); fseek(f, 0xC0, SEEK_SET); fread(logo, 1, sizeof(logo), f); fclose(f);
        for (int cpu = 0; cpu < 2; cpu++) {
            snprintf(tag, sizeof tag, "solar arm%d", cpu ? 7 : 9);
            slot(cpu, 2);
            int bad = 0;
            for (int i = 0; i < 0x9C; i++) bad += rb(cpu, 0x8000004 + i) != logo[i];
            EQ(bad, 0);
            EQ(r(cpu, 0x80000AC), 0x45493355); EQ(rb(cpu, 0x80000B2), 0x96); EQ(rh(cpu, 0x8000100), 0x0080);
            uint8_t check = 0;
            for (int i = 0xA0; i < 0xBE; i++) check += rb(cpu, 0x8000000 + i);
            EQ(uint8_t(check + 0x19), 0);
            EQ(rh(cpu, 0x80000C4), 0); // Still write-only
            EQ(solar(cpu, 0), 0xE8); EQ(solar(cpu, 5), 0xE8 - 42); EQ(solar(cpu, 10), 0x31);
            EQ(rh(cpu, 0x80000C6), 7); EQ(rh(cpu, 0x80000C8), 1);
            Settings::sunlight = 0;
            wh(cpu, 0x80000C4, 4); wh(cpu, 0x80000C4, 6); wh(cpu, 0x80000C4, 4);
            EQ(rh(cpu, 0x80000C4) & 8, 8); // Pin 2 high selects the RTC, so the reset was ignored
            wh(cpu, 0x80000C8, 0); EQ(rh(cpu, 0x80000C4), 0);
        }
    }

    // A real Boktai ROM in the slot, with the sensor on its GPIO pins; the rest of the ROM reads as normal
    boot(nds, gbaRom("U32E"), SLOT2_RUMBLE);
    for (int cpu = 0; cpu < 2; cpu++) {
        snprintf(tag, sizeof tag, "boktai rom arm%d", cpu ? 7 : 9);
        slot(cpu, 2);
        EQ(c->slot2.getDevice(), SLOT2_NONE);
        EQ(rh(cpu, 0x8000100), 0x0700); EQ(rb(cpu, 0x8000FFF), uint8_t(0xFFF * 7)); EQ(rh(cpu, 0x8001000), 0x0800);
        EQ(rh(cpu, 0x9000100), 0x0080); EQ(rb(cpu, 0xA000000), 0xFF);
        EQ(solar(cpu, 0), 0xE8); EQ(solar(cpu, 10), 0x31);
        wh(cpu, 0x80000C8, 0);
    }

    // WarioWare: Twisted's rumble motor runs while pin 3 is high
    boot(nds, gbaRom("RZWE"), SLOT2_NONE);
    for (int cpu = 0; cpu < 2; cpu++) {
        snprintf(tag, sizeof tag, "warioware arm%d", cpu ? 7 : 9);
        slot(cpu, 2);
        EQ(c->slot2.takeRumble(), false);
        wh(cpu, 0x80000C6, 8); wh(cpu, 0x80000C4, 8);
        EQ(c->slot2.takeRumble(), true); EQ(c->slot2.takeRumble(), true);
        wb(cpu, 0x80000C4, 0); EQ(c->slot2.takeRumble(), true); // Byte writes are ignored
        wh(cpu, 0x80000C4, 0); EQ(c->slot2.takeRumble(), false);
    }

    // A ROM that's neither gets mapped straight through and has no GPIO
    boot(nds, gbaRom("BPEE"), SLOT2_NONE);
    snprintf(tag, sizeof tag, "plain rom");
    slot(0, 2);
    EQ(c->memory.readMap9B[0x8000] != nullptr, true); EQ(rh(0, 0x8000100), 0x0700);
    wh(0, 0x80000C8, 1); EQ(rh(0, 0x80000C8), 0);
    slot(1, 2); EQ(c->memory.readMap9B[0x8000] == nullptr, true); EQ(c->memory.readMap7[0x8000] != nullptr, true);
    EQ(rh(0, 0x8000100), 0); EQ(rh(1, 0x8000100), 0x0700);

    // In GBA mode, Boktai's GPIO is shared with its RTC, and WarioWare's motor works too
    boot("", gbaRom("U3IE"), SLOT2_NONE);
    snprintf(tag, sizeof tag, "gba boktai");
    EQ(c->gbaMode, true);
    EQ(solar(1, 0), 0xE8); EQ(solar(1, 10), 0x31);
    boot("", gbaRom("RZWE"), SLOT2_NONE);
    snprintf(tag, sizeof tag, "gba warioware");
    wh(1, 0x80000C6, 8); wh(1, 0x80000C4, 8); EQ(c->slot2.takeRumble(), true);
    wh(1, 0x80000C4, 0); EQ(c->slot2.takeRumble(), false);

    printf("%d of %d checks passed\n", checks - fails, checks);
    return fails != 0;
}
