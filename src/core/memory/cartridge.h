/*
    Copyright 2019-2026 Hydr8gon

    This file is part of NooDS.

    NooDS is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    NooDS is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with NooDS. If not, see <https://www.gnu.org/licenses/>.
*/

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "../defines.h"
#include "../worker_thread.h"
#include "../file_lock.h"

class Core;

enum NdsCmdMode {
    CMD_NONE = 0,
    CMD_HEADER,
    CMD_CHIP,
    CMD_SECURE,
    CMD_DATA
};

class Cartridge {
public:
    // How much of a ROM being loaded into RAM is loaded, from 0 to 100, or -1 if none is being loaded
    static std::atomic<int> loadPercent;
    static std::atomic<bool> cancelLoad; // Set to stop waiting for a ROM to fully load, which cancels creating the core

    Cartridge(Core *core): core(core) {}
    ~Cartridge();

    bool setRom(std::string romPath, int romFd = -1, int saveFd = -1, int stateFd = -1, int cheatFd = -1);
    void writeSave();

    void trimRom();
    void resizeSave(int newSize, bool dirty = true);

    int getRomSize() { return romSize; }
    int getSaveSize() { return saveSize; }

protected:
    Core *core;

    FILE *romFile = nullptr;
    uint8_t *rom = nullptr, *save = nullptr;
    int romSize = 0, saveSize = -1;
    bool saveDirty = false;
    std::mutex mutex;

    std::vector<uint32_t> saveSizes;
    uint32_t romMask = 0;

    virtual bool loadRom();
    void loadRomSection(size_t offset, size_t size);
    bool startRomLoad();
    void waitRomLoad();
    void ensureLoaded(size_t offset, size_t size);

private:
    std::string romPath, savePath;
    int romFd = -1, saveFd = -1;

    // Large chunks of a ROM file kept in memory, so that small streamed reads don't each go to the file
    // Chunks are filled in blocks: a block that's needed is read right away, while another thread loads the rest of
    // its chunk and the chunk after it, a few blocks per read, so a read that misses the cache doesn't stall for long
    static const uint32_t CHUNK_SIZE = 0x40000;
    static const uint32_t BLOCK_SIZE = 0x4000;
    static const int CHUNK_COUNT = 8;
    static const int PREFETCH_BLOCKS = 2;

    struct RomChunk {
        uint8_t *alloc = nullptr;
        uint8_t *data = nullptr;
        uint32_t index = -1;
        uint32_t lastUse = 0;
        uint16_t valid = 0; // Blocks that are loaded
        uint16_t reading = 0; // Blocks being loaded
    };

    RomChunk chunks[CHUNK_COUNT];
    uint32_t chunkUses = 0;
    uint32_t prefetchWant[2] = { uint32_t(-1), uint32_t(-1) }; // The chunk last read from and the one after it
    int prefetchStart = 0; // The block after the last one read, where loading ahead in its chunk starts
    bool stopPrefetch = false;
    size_t romBufSize = 0;
    std::mutex chunkMutex;
    std::condition_variable chunkCond;
    WorkerThread *prefetchThread = nullptr;

    // One worker owns ROM file reads, prioritizing missing blocks requested by the emulator before preloading ahead
    // Reads of loaded blocks need no lock, and the file is closed once the entire ROM is loaded
    std::atomic<uint8_t> *romBlocks = nullptr; // For each block: 0 = not loaded, 1 = loading, 2 = loaded
    std::atomic<bool> romLoaded{false}, stopLoad{false};
    size_t blocksLoaded = 0; // Only changed by the loading thread, once it's started
    size_t loadWant = size_t(-1);
    std::mutex loadMutex;
    std::condition_variable loadCond;
    FILE *loadFile = nullptr;
    WorkerThread *loadThread = nullptr;

    void readRomFile(FILE *file, uint8_t *data, size_t offset, size_t size);
    void loadRomBlocks(size_t first, size_t count);
    void runRomLoad();
    int findChunk(uint32_t index);
    int claimChunk(uint32_t index);
    void readBlocks(int slot, int first, int count, std::unique_lock<std::mutex> &lock);
    void runPrefetch();
};

class CartridgeNds: public Cartridge {
public:
    CartridgeNds(Core *core): Cartridge(core) {}
    void saveState(FILE *file);
    void loadState(FILE *file);

    void directBoot();
    void wordReady(bool cpu);

    uint16_t readAuxSpiCnt(bool cpu) { return auxSpiCnt[cpu]; }
    uint8_t readAuxSpiData(bool cpu) { return auxSpiData[cpu]; }
    uint32_t readRomCtrl(bool cpu) { return romCtrl[cpu]; }
    uint32_t readRomDataIn(bool cpu);

    void writeAuxSpiCnt(bool cpu, uint16_t mask, uint16_t value);
    void writeAuxSpiData(bool cpu, uint8_t value);
    void writeRomCtrl(bool cpu, uint32_t mask, uint32_t value);
    void writeRomCmdOutL(bool cpu, uint32_t mask, uint32_t value);
    void writeRomCmdOutH(bool cpu, uint32_t mask, uint32_t value);

private:
    uint32_t romCode = 0;
    bool romEncrypted = false;
    NdsCmdMode cmdMode = CMD_NONE;

    uint32_t encTable[0x412] = {};
    uint32_t encCode[3] = {};

    uint32_t romAddrReal[2] = {}, romAddrVirt[2] = {};
    uint16_t blockSize[2] = {}, readCount[2] = {};
    uint32_t wordCycles[2] = {};
    bool encrypted[2] = {};

    uint8_t auxCommand[2] = {};
    uint32_t auxAddress[2] = {};
    uint32_t auxWriteCount[2] = {};

    uint16_t auxSpiCnt[2] = {};
    uint8_t auxSpiData[2] = {};
    uint32_t romCtrl[2] = {};
    uint64_t romCmdOut[2] = {};

    virtual bool loadRom();
    void loadNdsCode(bool arm7);
    void loadDsiCode(bool arm7);

    uint64_t encrypt64(uint64_t value);
    uint64_t decrypt64(uint64_t value);
    void initKeycode(int level);
    void applyKeycode();
};

class CartridgeGba: public Cartridge {
public:
    CartridgeGba(Core *core): Cartridge(core) {}
    void saveState(FILE *file);
    void loadState(FILE *file);

    uint8_t *getRom(uint32_t address);
    bool isEeprom(uint32_t address);

    uint8_t eepromRead();
    void eepromWrite(uint8_t value);

    uint8_t sramRead(uint32_t address);
    void sramWrite(uint32_t address, uint8_t value);

private:
    uint8_t eepromCount = 0;
    uint16_t eepromCmd = 0;
    uint64_t eepromData = 0;
    bool eepromDone = false;

    uint8_t flashCmd = 0;
    bool bankSwap = false;
    bool flashErase = false;

    bool findString(std::string string);
    virtual bool loadRom();
};

FORCE_INLINE uint8_t *CartridgeGba::getRom(uint32_t address) {
    return ((address &= romMask) < romSize) ? &rom[address] : nullptr;
}

FORCE_INLINE bool CartridgeGba::isEeprom(uint32_t address) {
    return (saveSize == -1 || saveSize == 0x200 || saveSize == 0x2000) && (romSize <= 0x1000000 || address >= 0x0DFFFF00);
}
