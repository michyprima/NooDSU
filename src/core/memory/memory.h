/*
    Copyright 2019-2026 Hydr8gon
    Copyright 2026 michyprima

    This file is part of NooDS U.

    NooDS U is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    NooDS U is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with NooDS U. If not, see <https://www.gnu.org/licenses/>.
*/

#pragma once

#include <cstdint>
#include "../defines.h"

class Core;

struct VramMapping {
    uint8_t *mappings[7] = {};
    uint8_t count = 0;

    void add(uint8_t *mapping);
    template <typename T> T read(uint32_t address);
    template <typename T> void write(uint32_t address, T value);
};

class Memory {
public:
    friend class Jit;

    // 32-bit address space, split into 4KB blocks
    uint8_t *readMap9A[0x100000] = {};
    uint8_t *readMap9B[0x100000] = {};
    uint8_t *readMap7[0x100000] = {};
    uint8_t *writeMap9A[0x100000] = {};
    uint8_t *writeMap9B[0x100000] = {};
    uint8_t *writeMap7[0x100000] = {};

    // Where pages in main RAM and WRAM (0x2000000 to 0x3FFFFFF) point when unmapped for having compiled code, then
    // pages in the DSi's main RAM mirror (0xC000000 to 0xCFFFFFF)
    uint8_t *jitPages9A[0x3000] = {};
    uint8_t *jitPages9B[0x3000] = {};
    uint8_t *jitPages7[0x3000] = {};

    // Recently written lines in pages with compiled code that have none themselves, by host address
    static const int CLEAN_LINES = 64;
    uintptr_t cleanLines[CLEAN_LINES] = {};

    uint8_t palette[0x800] = {}; // 2KB palette
    uint8_t oam[0x800] = {}; // 2KB OAM
    uint8_t *engAExtPal[5] = {};
    uint8_t *engBExtPal[5] = {};
    uint8_t *tex3D[4] = {};
    uint8_t *pal3D[6] = {};
    volatile uint32_t textureMapChanges = 0; // Counts when 3D texture data might have changed, which needs a remap

    Memory(Core *core);
    ~Memory();
    void saveState(FILE *file);
    void loadState(FILE *file);
    void allocRam(bool dsi);

    bool loadBios9();
    bool loadBios7();
    bool loadGbaBios();
    void copyBiosLogo(uint8_t *logo);

    void updateMap9(uint32_t start, uint32_t end, bool tcm = false);
    void updateMap7(uint32_t start, uint32_t end);
    void forgetCleanLines();
    void updateVram(bool all = false);

    template <typename T> T read(bool arm7, uint32_t address, bool tcm = true);
    template <typename T> void write(bool arm7, uint32_t address, T value, bool tcm = true);

    // Get pointers to memory that can be accessed directly, or null if accesses need special handling
    uint8_t *getReadPtr(bool arm7, uint32_t address, bool tcm = true) {
        uint8_t *data = (arm7 ? readMap7 : (tcm ? readMap9A : readMap9B))[address >> 12];
        return data ? &data[address & 0xFFF] : nullptr;
    }
    uint8_t *getWritePtr(bool arm7, uint32_t address, bool tcm = true) {
        uint8_t *data = (arm7 ? writeMap7 : (tcm ? writeMap9A : writeMap9B))[address >> 12];
        return data ? &data[address & 0xFFF] : nullptr;
    }

    uint32_t readDmaFill(int channel) { return dmaFill[channel]; }
    uint8_t readVramCnt(int block) { return vramCnt[block]; }
    uint8_t readVramStat() { return vramStat; }
    uint8_t readWramCnt() { return wramCnt; }
    uint16_t readExMemCnt() { return exMemCnt; }
    uint16_t readExMemStat() { return (exMemCnt & 0xFF80) | (exMemStat & 0x7F); }
    uint8_t readHaltCnt() { return haltCnt; }

    uint8_t readMbk1(int i) { return mbk1[i]; }
    uint8_t readMbk23(int i) { return mbk23[i]; }
    uint8_t readMbk45(int i) { return mbk45[i]; }
    uint32_t readMbk6(bool arm7) { return mbk6[arm7]; }
    uint32_t readMbk7(bool arm7) { return mbk7[arm7]; }
    uint32_t readMbk8(bool arm7) { return mbk8[arm7]; }
    uint32_t readMbk9() { return mbk9; }
    uint32_t readScfgExt(bool arm7) { return scfgExt[arm7]; }
    uint16_t readScfgRom() { return scfgRom; }
    uint32_t readKeyTable(int i, bool dsiCart = false);
    uint16_t readScfgMc();
    uint16_t readCardInsertDelay() { return cardInsertDelay; }
    uint16_t readCardPowerOffDelay() { return cardPowerOffDelay; }

    void writeDmaFill(int channel, uint32_t mask, uint32_t value);
    void writeVramCnt(int index, uint8_t value);
    void writeWramCnt(uint8_t value);
    void writeExMemCnt(uint16_t mask, uint16_t value);
    void writeExMemStat(uint16_t mask, uint16_t value);
    void writeHaltCnt(uint8_t value);
    void writeGbaHaltCnt(uint8_t value);

    void writeMbk1(int i, uint8_t value);
    void writeMbk23(int i, uint8_t value);
    void writeMbk45(int i, uint8_t value);
    void writeMbk6(bool arm7, uint32_t mask, uint32_t value);
    void writeMbk7(bool arm7, uint32_t mask, uint32_t value);
    void writeMbk8(bool arm7, uint32_t mask, uint32_t value);
    void writeMbk9(uint32_t mask, uint32_t value);
    void writeScfgExt9(uint32_t mask, uint32_t value);
    void writeScfgExt7(uint32_t mask, uint32_t value);
    void setScfgExt(uint32_t ext9, uint32_t ext7);
    void applyPendingRam();
    void writeScfgRom(uint16_t mask, uint16_t value);
    void writeScfgMc(uint16_t mask, uint16_t value);
    void writeCardInsertDelay(uint16_t mask, uint16_t value);
    void writeCardPowerOffDelay(uint16_t mask, uint16_t value);
    void prepareTitle(bool dsiTitle);
    void cardPowerOff();

private:
    Core *core;
    uint32_t gbaBiosAddr = 0;

    uint8_t bios9[0x10000] = {}; // 64KB ARM9 BIOS
    uint8_t bios7[0x10000] = {}; // 64KB ARM7 BIOS
    uint8_t ndsBios9[0x10000] = {}, ndsBios7[0x10000] = {}; // The DS BIOSes, which a DSi switches to for DS titles
    bool ndsBios = false;
    uint8_t gbaBios[0x4000] = {}; // 16KB GBA BIOS

    uint8_t *ram = nullptr; // 4MB main RAM, or 16MB in DSi mode
    uint32_t ramSize = 0;
    uint8_t wram[0x8000] = {}; // 32KB shared WRAM
    uint8_t instrTcm[0x8000] = {}; // 32KB instruction TCM
    uint8_t dataTcm[0x4000] = {}; // 16KB data TCM
    uint8_t wram7[0x10000] = {}; // 64KB ARM7 WRAM
    uint8_t wifiRam[0x2000] {}; // 8KB WiFi RAM

    uint8_t vramA[0x20000] = {}; // 128KB VRAM block A
    uint8_t vramB[0x20000] = {}; // 128KB VRAM block B
    uint8_t vramC[0x20000] = {}; // 128KB VRAM block C
    uint8_t vramD[0x20000] = {}; // 128KB VRAM block D
    uint8_t vramE[0x10000] = {}; // 64KB VRAM block E
    uint8_t vramF[0x4000] = {}; // 16KB VRAM block F
    uint8_t vramG[0x4000] = {}; // 16KB VRAM block G
    uint8_t vramH[0x8000] = {}; // 32KB VRAM block H
    uint8_t vramI[0x4000] = {}; // 16KB VRAM block I

    uint8_t nwramA[0x40000] = {}; // 256KB NWRAM block A
    uint8_t nwramB[0x40000] = {}; // 256KB NWRAM block B
    uint8_t nwramC[0x40000] = {}; // 256KB NWRAM block C

    VramMapping engABg[32];
    VramMapping engBBg[8];
    VramMapping engAObj[16];
    VramMapping engBObj[8];
    VramMapping lcdc[64];
    VramMapping vram7[2];

    uint32_t dmaFill[4] = {};
    uint8_t vramCnt[9] = {};
    uint8_t vramStat = 0;
    uint8_t wramCnt = 0;
    uint16_t exMemCnt = 0x6000, exMemStat = 0; // The ARM7 sees the ARM9's slot access bits and its own GBA slot timings
    uint8_t haltCnt = 0;

    uint8_t mbk1[4] = {};
    uint8_t mbk23[8] = {};
    uint8_t mbk45[8] = {};
    uint32_t mbk6[2] = {};
    uint32_t mbk7[2] = {};
    uint32_t mbk8[2] = {};
    uint32_t mbk9 = 0;
    uint32_t scfgExt[2] = { 0x8307F100, 0x93FFFB06 }; // As the DSi firmware runs with them
    uint16_t scfgRom = 0x0101; // Which BIOS each CPU sees; the boot stage before the firmware hides their upper halves
    bool bigRam = true, ramPending = false; // Whether main RAM is mapped as 16MB, and a change waiting for the launcher
    uint16_t scfgMc = 0x0010; // Card slot power states; the second slot is always empty on a DSi
    uint16_t cardInsertDelay = 0xFFFF, cardPowerOffDelay = 0xFFFF;
    uint8_t cardPowerOffs = 0; // Power-offs scheduled, of which only the last one counts

    uint32_t ramMask();
    void updateRamSize();

    template <typename T> T readFallback(bool arm7, uint32_t address);
    template <typename T> void writeFallback(bool arm7, uint32_t address, T value, bool tcm);
    template <typename T> bool writeJit(bool arm7, uint32_t address, T value, bool tcm);
    void protectJit(uint8_t **writeMap, uint32_t start, uint32_t end);
    void unmapJit(uint8_t **writeMap, uint32_t index);
    uint8_t **jitPages(uint8_t **writeMap);
    static int jitSlot(uint32_t index);

    template <typename T> T ioRead9(uint32_t address);
    template <typename T> T ioRead7(uint32_t address);
    template <typename T> T ioReadGba(uint32_t address);
    template <typename T> void ioWrite9(uint32_t address, T value);
    template <typename T> void ioWrite7(uint32_t address, T value);
    template <typename T> void ioWriteGba(uint32_t address, T value);
};

template uint8_t Memory::read(bool arm7, uint32_t address, bool tcm);
template uint16_t Memory::read(bool arm7, uint32_t address, bool tcm);
template uint32_t Memory::read(bool arm7, uint32_t address, bool tcm);
template <typename T> FORCE_INLINE T Memory::read(bool arm7, uint32_t address, bool tcm) {
    // Look up a pointer to readable memory and read a value from it LSB-first
    uint8_t **readMap = arm7 ? readMap7 : (tcm ? readMap9A : readMap9B);
    if (uint8_t *data = readMap[address >> 12])
        return readLsb<T>(&data[address & (0x1000 - sizeof(T))]);

    // Handle special read cases that can't be mapped
    return readFallback<T>(arm7, address);
}

template void Memory::write(bool arm7, uint32_t address, uint8_t value, bool tcm);
template void Memory::write(bool arm7, uint32_t address, uint16_t value, bool tcm);
template void Memory::write(bool arm7, uint32_t address, uint32_t value, bool tcm);
template <typename T> FORCE_INLINE void Memory::write(bool arm7, uint32_t address, T value, bool tcm) {
    // Look up a pointer to writable memory and write a value to it LSB-first
    uint8_t **writeMap = arm7 ? writeMap7 : (tcm ? writeMap9A : writeMap9B);
    if (uint8_t *data = writeMap[address >> 12])
        return writeLsb<T>(&data[address & (0x1000 - sizeof(T))], value);

    // Handle special write cases that can't be mapped
    return writeFallback<T>(arm7, address, value, tcm);
}
