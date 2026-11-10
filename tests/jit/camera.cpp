// Checks the DSi's cameras with both CPUs halted, through the real registers: the sensors on I2C, their MCU variables,
// pictures copied by NDMA as the DSi Camera does it, mirroring, RGB555, trimming, overruns and save states
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
static const uint32_t CAM_MCNT = 0x4004200, CAM_CNT = 0x4004202, CAM_DAT = 0x4004204, CAM_SOFS = 0x4004210, CAM_EOFS = 0x4004214,
    I2C_DATA = 0x4004500, I2C_CNT = 0x4004501, NDMA1 = 0x4004120, DST = 0x2300000;
static uint32_t r9(uint32_t a) { return c->memory.read<uint32_t>(0, a, false); }
static uint16_t rh9(uint32_t a) { return c->memory.read<uint16_t>(0, a, false); }
static void w9(uint32_t a, uint32_t v) { c->memory.write<uint32_t>(0, a, v, false); }
static void wh9(uint32_t a, uint16_t v) { c->memory.write<uint16_t>(0, a, v, false); }
static void frame(int n = 1) { while (n--) do c->runCore(); while (!c->frameEnded.exchange(false)); }

// I2C on the ARM7: a start with the device address, then bytes; returns whether the device acknowledged
static uint8_t i2c(uint8_t data, uint8_t cnt) {
    c->memory.write<uint8_t>(1, I2C_DATA, data, false); c->memory.write<uint8_t>(1, I2C_CNT, cnt, false);
    return c->memory.read<uint8_t>(1, (cnt & 0x20) ? I2C_DATA : I2C_CNT, false);
}
static bool select(uint8_t device) { return i2c(device, 0x82) & 0x10; }
static void camWrite(uint8_t device, uint16_t reg, uint16_t value) {
    select(device); i2c(reg >> 8, 0x80); i2c(reg, 0x80); i2c(value >> 8, 0x80); i2c(value, 0x81);
}
static uint16_t camRead(uint8_t device, uint16_t reg) {
    select(device); i2c(reg >> 8, 0x80); i2c(reg, 0x81);
    select(device | 1); uint8_t hi = i2c(0, 0xA0); uint8_t lo = i2c(0, 0xA1);
    return hi << 8 | lo;
}
static void mcuWrite(uint8_t device, uint16_t var, uint16_t value) { camWrite(device, 0x098C, var); camWrite(device, 0x0990, value); }

// A picture where each word says where it is, and a check of what a 256x192 preview of it gives
static uint32_t input[640 * 480 / 2];
static uint32_t at(int y, int x) { return input[y * 320 + x]; }

static void boot() {
    delete c; c = nullptr;
    try { c = new Core(""); } catch (CoreError e) { printf("core error %d\n", int(e)); exit(2); }
    for (int cpu = 0; cpu < 2; cpu++) { c->interpreter[cpu].halt(0); c->memory.write<uint32_t>(cpu, 0x4000208, 0, false); c->memory.write<uint32_t>(cpu, 0x4000210, 0, false); }
    frame(2);
}

// Sets a sensor up for 256x192 preview pictures and starts it, with read mode bit 0 (no mirroring) as asked
static void startSensor(uint8_t device, bool mirror) {
    mcuWrite(device, 0x2703, 256); mcuWrite(device, 0x2705, 192); mcuWrite(device, 0x2717, mirror ? 0 : 1);
    mcuWrite(device, 0x2103, 1); // Preview
    camWrite(device, 0x0018, 0x0000); // Out of standby
    camWrite(device, 0x001A, 0x0200); // Sending data
}

// Starts NDMA1 copying a 256x192 picture in blocks of four scanlines, as GBATEK's camera init does
// Runs until NDMA has copied a whole picture, at most 20 frames
static void untilCopied() { for (int i = 0; i < 20 && (r9(NDMA1 + 0x18) >> 31); i++) frame(1); }
static void startNdma(uint32_t words = 0x6000, uint32_t block = 0x200) {
    for (uint32_t i = 0; i < 0x6000 * 4; i += 4) w9(DST + i, 0xDEADBEEF);
    w9(NDMA1 + 0x0, CAM_DAT); w9(NDMA1 + 0x4, DST); w9(NDMA1 + 0x8, words); w9(NDMA1 + 0xC, block); w9(NDMA1 + 0x10, 2);
    w9(NDMA1 + 0x18, 0x8B044000);
}

int main(int argc, char **argv) {
    std::string files = argv[1];
    Settings::load(".");
    Settings::ndsBios9Path = files + "/bios9.bin"; Settings::ndsBios7Path = files + "/bios7.bin"; Settings::ndsFirmPath = files + "/firmware.bin";
    Settings::dsiBios9Path = files + "/bios9i.bin"; Settings::dsiBios7Path = files + "/bios7i.bin"; Settings::dsiFirmPath = files + "/firmwarei.bin";
    Settings::dsiNandPath = "nand.bin"; Settings::sdImagePath = "missing.img"; Settings::jit = 0; Settings::dsiMode = 1; Settings::directBoot = 0; Settings::savesFolder = 1;
    Settings::threaded2D = 0; Settings::threaded3D = 0; Settings::arm7Hle = 0; Settings::emulateAudio = 0; Settings::fast2D = 0; Settings::highRes3D = 0; Settings::hardware3D = 0; Settings::fpsLimiter = 0;
    Rtc::fixedTime = 1767225600;
    for (int y = 0; y < 480; y++) for (int x = 0; x < 320; x++) input[y * 320 + x] = (y << 20) | (x << 4) | 0x80008;
    boot();

    // Only the MCU and the two cameras answer on I2C; the alternate cameras some DSi software looks for aren't there
    strcpy(tag, "i2c");
    EQ(select(0x4A), 1);
    EQ(select(0x78), 1);
    EQ(select(0x7A), 1);
    EQ(select(0xA0), 0);
    EQ(select(0xE0), 0);
    EQ(select(0x40), 0);
    select(0xA1); EQ(i2c(0, 0xA1), 0xFF);

    // The sensors read 0xFF while CAM_MCNT holds them in reset, then give their chip ID
    strcpy(tag, "reset");
    EQ(camRead(0x7A, 0x0000), 0xFFFF);
    wh9(CAM_MCNT, 0x0000); wh9(CAM_MCNT, 0x0022);
    EQ(rh9(CAM_MCNT), 0x00A2);
    EQ(camRead(0x7A, 0x0000), 0x2280);
    EQ(camRead(0x78, 0x0000), 0x2280);
    EQ(camRead(0x7A, 0x0018), 0x4029); // In standby
    EQ(c->memory.read<uint16_t>(1, CAM_MCNT, false), 0); // The ARM7 doesn't have the module

    // MCU variables go through the window at 0x0990, and the sequencer switches between preview and capture
    strcpy(tag, "mcu");
    mcuWrite(0x7A, 0x2703, 0x0123); EQ(camRead(0x7A, 0x0990), 0x0123);
    camWrite(0x7A, 0x098C, 0xA704); EQ(camRead(0x7A, 0x0990), 0x0001); // Byte access with bit 15
    EQ(camRead(0x7A, 0x098C), 0xA704);
    mcuWrite(0x7A, 0x2103, 2); camWrite(0x7A, 0x098C, 0xA104); EQ(camRead(0x7A, 0x0990), 7);
    mcuWrite(0x7A, 0x2103, 1); camWrite(0x7A, 0x098C, 0xA104); EQ(camRead(0x7A, 0x0990), 3);
    camWrite(0x7A, 0x098C, 0xA103); EQ(camRead(0x7A, 0x0990), 0); // SEQ_CMD reads back as done

    // A preview frame through NDMA, as the DSi Camera does it: the interrupt, then 256x192 scaled from the picture
    for (int mirror = 0; mirror < 2; mirror++) {
        snprintf(tag, sizeof tag, "preview mirror %d", mirror);
        boot();
        DsiCamera::sendFrame(input);
        wh9(CAM_MCNT, 0x0000); wh9(CAM_MCNT, 0x0022);
        startSensor(0x7A, mirror);
        EQ(DsiCamera::streaming.load(), 1);
        wh9(CAM_CNT, 0x0020); wh9(CAM_CNT, 0x0A03); wh9(CAM_CNT, 0x8A03);
        startNdma();
        c->interpreter[0].irf &= ~BITL(25);
        untilCopied();
        EQ((c->interpreter[0].irf >> 25) & 1, 1);
        EQ(r9(NDMA1 + 0x18) >> 31, 0); // Finished the whole picture
        EQ(rh9(CAM_CNT) & 0x0010, 0); // With no overrun
        int bad = 0;
        for (int row = 0; row < 192; row++) {
            int sy = (row * 5 + 1) / 2;
            for (int x = 0; x < 128; x++) {
                uint32_t want = mirror ? at(sy, 319 - x * 640 / 256) : at(sy, x * 640 / 256);
                if (mirror) want = (want & 0xFF00FF00) | ((want >> 16) & 0xFF) | ((want & 0xFF) << 16);
                bad += r9(DST + (row * 128 + x) * 4) != want;
            }
        }
        EQ(bad, 0);
    }

    // RGB555 conversion, with the pixels' alpha bits set
    strcpy(tag, "rgb");
    {
        static uint32_t gray[640 * 480 / 2], blue[640 * 480 / 2];
        for (auto &v : gray) v = 0x80808080; // Y 0x80 and neutral color
        for (auto &v : blue) v = 0x6E29F029; // The BT.601 blue bar
        for (int i = 0; i < 2; i++) {
            boot();
            DsiCamera::sendFrame(i ? blue : gray);
            wh9(CAM_MCNT, 0x0000); wh9(CAM_MCNT, 0x0022);
            startSensor(0x7A, false);
            wh9(CAM_CNT, 0x0020); wh9(CAM_CNT, 0x2A03); wh9(CAM_CNT, 0xAA03);
            startNdma();
            untilCopied();
            EQ(r9(DST), i ? 0xF441F441 : 0xC210C210); // From the YUV-to-RGB formula GBATEK gives
        }
    }

    // Trimming keeps only the window from CAM_SOFS to CAM_EOFS, in words and scanlines
    strcpy(tag, "crop");
    boot();
    DsiCamera::sendFrame(input);
    wh9(CAM_MCNT, 0x0000); wh9(CAM_MCNT, 0x0022);
    startSensor(0x7A, false);
    w9(CAM_SOFS, (10 << 16) | (8 << 1)); w9(CAM_EOFS, (41 << 16) | (71 << 1)); // 64 words of 32 scanlines
    EQ(r9(CAM_SOFS), (10u << 16) | 16);
    wh9(CAM_CNT, 0x0020); wh9(CAM_CNT, 0x4A03); wh9(CAM_CNT, 0xCA03);
    startNdma(64 * 32, 64 * 4);
    untilCopied();
    EQ(r9(NDMA1 + 0x18) >> 31, 0);
    {
        int bad = 0;
        for (int row = 0; row < 32; row++)
            for (int x = 0; x < 64; x++)
                bad += r9(DST + (row * 64 + x) * 4) != at(((row + 10) * 5 + 1) / 2, (x + 8) * 640 / 256);
        EQ(bad, 0);
        EQ(r9(DST + 64 * 32 * 4), 0xDEADBEEF);
    }
    w9(CAM_SOFS, 0); EQ(r9(CAM_SOFS), (10u << 16) | 16); // Locked while transferring

    // Without NDMA reading it, the module overruns, and bit 5 clears that
    strcpy(tag, "overrun");
    boot();
    DsiCamera::sendFrame(input);
    wh9(CAM_MCNT, 0x0000); wh9(CAM_MCNT, 0x0022);
    startSensor(0x7A, false);
    wh9(CAM_CNT, 0x0020); wh9(CAM_CNT, 0x8A03);
    frame(10);
    EQ(rh9(CAM_CNT) & 0x0010, 0x0010);
    wh9(CAM_CNT, 0x0A23);
    EQ(rh9(CAM_CNT) & 0x0010, 0);

    // Standby stops the pictures, and the frontend's camera with them
    strcpy(tag, "standby");
    camWrite(0x7A, 0x0018, 0x0001);
    EQ(DsiCamera::streaming.load(), 0);

    // Signing a photo is an AES-CCM job with only associated data and no payload, which has to finish once that data
    // runs out: the busy bit clears, the ARM7 gets the AES interrupt (IF2 bit 12), and the MAC waits in the read FIFO
    strcpy(tag, "aes mac only");
    for (int blocks : { 1, 3 }) {
        auto w7 = [](uint32_t a, uint32_t v) { c->memory.write<uint32_t>(1, a, v, false); };
        auto r7 = [](uint32_t a) { return c->memory.read<uint32_t>(1, a, false); };
        c->interpreter[1].irf &= ~BITL(44);
        w7(0x4004404, blocks); // No payload blocks, and this many of associated data
        w7(0x4004400, 0xC0076C00 | (1 << 28)); // Start CCM encryption with the interrupt, a 16-byte MAC, empty FIFOs
        EQ(r7(0x4004400) >> 31, 1);
        for (int i = 0; i < blocks * 4; i++) w7(0x4004408, 0x12345678 * (i + 1));
        frame(1);
        EQ(r7(0x4004400) >> 31, 0);
        EQ((c->interpreter[1].irf >> 44) & 1, 1);
        EQ((r7(0x4004400) >> 5) & 0x1F, 4);
    }

    // Everything survives a save state
    strcpy(tag, "state");
    startSensor(0x78, true);
    {
        FILE *f = tmpfile(); c->dsiCamera.saveState(f);
        wh9(CAM_MCNT, 0x0000); EQ(camRead(0x78, 0x0000), 0xFFFF);
        rewind(f); c->dsiCamera.loadState(f); fclose(f);
        EQ(camRead(0x78, 0x0000), 0x2280);
        camWrite(0x78, 0x098C, 0x2703); EQ(camRead(0x78, 0x0990), 256);
        EQ(DsiCamera::streaming.load(), 1);
    }

    printf("%d of %d checks passed\n", checks - fails, checks);
    return fails != 0;
}
