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

#include <algorithm>
#include <cstring>
#include <thread>
#include <new>

#include "../core.h"
#include "../file_lock.h"
#include "../heartbeat.h"

#include <malloc.h>
#ifdef ROM_LOAD_TEST
#include <coreinit/time.h>
#endif

std::atomic<int> Cartridge::loadPercent{-1};
std::atomic<bool> Cartridge::cancelLoad{false};
Cartridge::KeptRom Cartridge::keptRoms[2];
std::string Cartridge::launchSavePath;
int Cartridge::launchSaveSize = -1;

static uint8_t *allocRom(size_t size) {
    // Reading from file into memory that's aligned for it is much faster on Wii U
    return (uint8_t*)memalign(64, size);
}

Cartridge::~Cartridge() {
    // Update the save file before exiting
    writeSave();

    // Stop loading the ROM into RAM
    if (loadThread) {
        stopLoad.store(true);
        loadThread->join();
        delete loadThread;
        if (!romLoaded.load()) loadPercent.store(-1);
    }
    if (loadFile) fclose(loadFile);
    delete[] romBlocks;

    // Stop loading ROM chunks ahead
    if (prefetchThread) {
        chunkMutex.lock();
        stopPrefetch = true;
        chunkMutex.unlock();
        chunkCond.notify_all();
        prefetchThread->join();
        delete prefetchThread;
    }

    // Free the ROM and save memory
    if (romFile) fclose(romFile);
    free(rom);
    if (save) delete[] save;
    for (int i = 0; i < CHUNK_COUNT; i++)
        delete[] chunks[i].alloc;
}

bool Cartridge::setRom(std::string romPath, int romFd, int saveFd, int stateFd, int cheatFd) {
    // Derive file names with extensions based on instance ID
    std::string basePath = romPath.substr(0, romPath.rfind('.'));
    std::string savePath = basePath + (core->id ? (".sv" + std::to_string(core->id + 1)) : ".sav");
    std::string statePath = basePath + (core->id ? (".no" + std::to_string(core->id + 1)) : ".noo");
    std::string cheatPath = basePath + ".cht";

    // Relocate files to separate folders if enabled
    if (Settings::savesFolder)
        savePath = Settings::basePath + "/saves" + savePath.substr(savePath.find_last_of("/\\"));
    if (Settings::statesFolder)
        statePath = Settings::basePath + "/states" + statePath.substr(statePath.find_last_of("/\\"));
    if (Settings::cheatsFolder)
        cheatPath = Settings::basePath + "/cheats" + cheatPath.substr(cheatPath.find_last_of("/\\"));

    // Load files using paths or descriptors if provided
    bool gba = (this == &core->cartridgeGba);
    if (romFd == -1) this->romPath = romPath; else this->romFd = romFd;
    if (saveFd == -1) this->savePath = savePath; else this->saveFd = saveFd;
    (stateFd == -1) ? core->saveStates.setPath(statePath, gba) : core->saveStates.setFd(stateFd, gba);
    if (!gba) (cheatFd == -1) ? core->actionReplay.setPath(cheatPath) : core->actionReplay.setFd(cheatFd);
    if (!loadRom()) return false;

    // Apply a save size chosen for this launch, now that the ROM is loaded
    if (launchSaveSize >= 0 && romPath == launchSavePath)
        resizeSave(launchSaveSize);
    return true;
}

bool Cartridge::loadRom() {
    // Attempt to open a ROM file
    romFile = (romFd == -1) ? fopen(romPath.c_str(), "rb") : fdopen(dup(romFd), "rb");
    if (!romFile) return false;

    // Read without the standard library's buffering, which splits reads into small pieces that are each slow on
    // some systems; reads are already in large blocks that get cached
    setvbuf(romFile, nullptr, _IONBF, 0);
    fseek(romFile, 0, SEEK_END);
    romSize = ftell(romFile);
    fseek(romFile, 0, SEEK_SET);

    // Attempt to load the ROM's save into memory
    if (FILE *saveFile = (saveFd == -1) ? fopen(savePath.c_str(), "rb") : fdopen(dup(saveFd), "rb")) {
        fseek(saveFile, 0, SEEK_END);
        saveSize = ftell(saveFile);
        fseek(saveFile, 0, SEEK_SET);
        save = new uint8_t[saveSize];
        fread(save, sizeof(uint8_t), saveSize, saveFile);
        fclose(saveFile);
    }

    // Verify the save size; invalid sizes fall back to auto-detection
    for (size_t i = 0; i < saveSizes.size(); i++)
        if (saveSize == saveSizes[i]) return true;
    saveSize = -1;
    return true;
}

void Cartridge::loadRomSection(size_t offset, size_t size) {
    // Make sure the ROM buffer can hold the section, keeping it between loads
    if (size > romBufSize) {
        free(rom);
        rom = allocRom(size);
        romBufSize = size;
    }

    // Load the whole ROM directly, since caching it in chunks wouldn't help
    if (offset == 0 && size >= (size_t)romSize) {
        readRomFile(romFile, rom, 0, size);
        if (!core->dsiMode) core->dldi.patchRom(rom, offset, size);
        return;
    }

    // Start loading chunks ahead the first time a section is loaded
    if (!prefetchThread)
        prefetchThread = new WorkerThread(WorkerThread::CORE_2, &Cartridge::runPrefetch, this);

    // Copy the section from cached chunks, reading any missing blocks right away
    std::unique_lock<std::mutex> lock(chunkMutex);
    size_t pos = offset, end = offset + size;
    while (pos < end) {
        uint32_t index = pos / CHUNK_SIZE;
        int slot = findChunk(index);
        if (slot == -1 && (slot = claimChunk(index)) == -1) {
            // Fall back to reading the rest from the file if a chunk can't be allocated
            lock.unlock();
            readRomFile(romFile, &rom[pos - offset], pos, end - pos);
            if (!core->dsiMode) core->dldi.patchRom(&rom[pos - offset], pos, end - pos);
            return;
        }

        // Wait if the block is being loaded ahead, or read it along with the next few if not, looking up the chunk
        // again afterwards
        uint32_t start = pos - index * CHUNK_SIZE;
        int block = start / BLOCK_SIZE;
        if (!(chunks[slot].valid & BIT(block))) {
            HB_TIMER(hbMiss);
            if (chunks[slot].reading & BIT(block)) {
                chunkCond.wait(lock);
            }
            else {
                uint16_t missing = ~(chunks[slot].valid | chunks[slot].reading);
                int count = 1, blocks = CHUNK_SIZE / BLOCK_SIZE;
                while (count < PREFETCH_BLOCKS && block + count < blocks && (missing & BIT(block + count)))
                    count++;
                readBlocks(slot, block, count, lock);
            }
            HB_INC(HB_ROM_MISSES);
            HB_ADD_US(HB_ROM_WAIT_US, hbMiss);
            continue;
        }

        uint32_t count = std::min<size_t>(end - pos, (block + 1) * BLOCK_SIZE - start);
        memcpy(&rom[pos - offset], &chunks[slot].data[start], count);
        chunks[slot].lastUse = ++chunkUses;
        pos += count;
    }

    // Have the rest of the chunk after the last block read and the chunk after it loaded ahead
    uint32_t last = (end - 1) / CHUNK_SIZE;
    int next = (end - 1 - last * CHUNK_SIZE) / BLOCK_SIZE + 1;
    if (prefetchWant[0] != last || prefetchStart != next) {
        prefetchWant[0] = last;
        prefetchWant[1] = ((last + 1) * CHUNK_SIZE < (size_t)romSize) ? (last + 1) : -1;
        prefetchStart = next;
        lock.unlock();
        chunkCond.notify_all();
    }
}

void Cartridge::readRomFile(FILE *file, uint8_t *data, size_t offset, size_t size) {
    // Let other file access go first, since this is called block after block while a ROM loads, and taking the lock
    // right back each time would keep them waiting until it's all loaded
    while (fileLock().contended())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    // Read data from the ROM file, with 0xFFs past the end
    std::lock_guard<FileLock> guard(fileLock());
    fseek(file, offset, SEEK_SET);
    size_t count = fread(data, sizeof(uint8_t), size, file);
    memset(&data[count], 0xFF, size - count);
}

int Cartridge::findChunk(uint32_t index) {
    // Find the slot holding a ROM chunk, or -1 if it isn't cached
    for (int i = 0; i < CHUNK_COUNT; i++)
        if (chunks[i].index == index) return i;
    return -1;
}

int Cartridge::claimChunk(uint32_t index) {
    // Give a ROM chunk the least recently used slot with no blocks being loaded, with no blocks loaded yet
    int slot = -1;
    for (int i = 0; i < CHUNK_COUNT; i++)
        if (!chunks[i].reading && (slot == -1 || chunks[i].lastUse < chunks[slot].lastUse)) slot = i;
    if (slot == -1) return -1;

    // Allocate chunk memory, with room for DLDI patches that run past the end
    // It's aligned to 64 bytes, which some systems need to read files straight into it
    RomChunk &chunk = chunks[slot];
    if (!chunk.alloc) {
        if (!(chunk.alloc = new (std::nothrow) uint8_t[CHUNK_SIZE + 0x100 + 0x3F]))
            return -1;
        chunk.data = (uint8_t*)(((uintptr_t)chunk.alloc + 0x3F) & ~0x3F);
    }
    chunk.index = index;
    chunk.valid = 0;
    chunk.lastUse = ++chunkUses;
    return slot;
}

void Cartridge::readBlocks(int slot, int first, int count, std::unique_lock<std::mutex> &lock) {
    // Load blocks of a chunk from file without holding the lock, so cached blocks can still be used
    RomChunk &chunk = chunks[slot];
    uint16_t mask = ((1 << count) - 1) << first;
    uint32_t start = first * BLOCK_SIZE;
    size_t offset = chunk.index * CHUNK_SIZE + start;
    bool prevValid = (first > 0 && (chunk.valid & BIT(first - 1)));
    chunk.reading |= mask;
    lock.unlock();
    readRomFile(romFile, &chunk.data[start], offset, count * BLOCK_SIZE);

    // Patch DLDI drivers, including any in the end of the block before that run into these
    if (!core->dsiMode) {
        uint32_t back = prevValid ? 0x100 : 0;
        core->dldi.patchRom(&chunk.data[start - back], offset - back, count * BLOCK_SIZE + back);
    }
    lock.lock();
    chunk.valid |= mask;
    chunk.reading &= ~mask;
    chunkCond.notify_all();
}

void Cartridge::runPrefetch() {
    // Load the rest of the chunk last read from and the one after it, a few blocks at a time
    std::unique_lock<std::mutex> lock(chunkMutex);
    while (!stopPrefetch) {
        bool loaded = false;
        for (int i = 0; i < 2; i++) {
            uint32_t index = prefetchWant[i];
            if (index == (uint32_t)-1) continue;
            int slot = findChunk(index);
            if (slot == -1 && (slot = claimChunk(index)) == -1) continue;

            // In the chunk last read from, only load the blocks after the last one read
            uint16_t missing = ~(chunks[slot].valid | chunks[slot].reading);
            if (i == 0) missing &= ~((1 << prefetchStart) - 1);
            if (!missing) continue;

            // Load the first run of missing blocks, then check again from the start in case what's wanted changed
            int first = __builtin_ctz(missing), count = 1, blocks = CHUNK_SIZE / BLOCK_SIZE;
            while (count < PREFETCH_BLOCKS && first + count < blocks && (missing & BIT(first + count)))
                count++;
            readBlocks(slot, first, count, lock);
            loaded = true;
            break;
        }
        if (!loaded) chunkCond.wait(lock);
    }
}

bool Cartridge::startRomLoad() {
    // Allocate memory for the whole ROM, with room for DLDI patches that run past the end, and track its blocks
    size_t blocks = (romSize + BLOCK_SIZE - 1) / BLOCK_SIZE;
    free(rom);
    rom = allocRom(romSize + 0x100);
    romBlocks = new (std::nothrow) std::atomic<uint8_t>[blocks];
    if (!rom || !romBlocks) {
        free(rom);
        delete[] romBlocks;
        rom = nullptr;
        romBlocks = nullptr;
        return false;
    }
    romBufSize = romSize;
    for (size_t i = 0; i < blocks; i++)
        romBlocks[i].store(0);

    // Hand the file over to loading, so the ROM is read from memory, and start loading it on another thread
    loadFile = romFile;
    romFile = nullptr;
    blocksLoaded = 0;
    loadPercent.store(0);
#ifdef ROM_LOAD_TEST
    loadStart = OSGetSystemTime();
#endif
    if (Settings::romInRam != 2) {
        loadThread = new WorkerThread(WorkerThread::CORE_2, &Cartridge::runFullLoad, this);
        return true;
    }

    // When streaming, the game starts right away, so load the start of the ROM, which has the header and secure area
    loadRomBlocks(0, std::min<size_t>(blocks, (0x8200 + BLOCK_SIZE - 1) / BLOCK_SIZE));
    loadThread = new WorkerThread(WorkerThread::CORE_2, &Cartridge::runRomLoad, this);
    return true;
}

void Cartridge::freeKeptRom(KeptRom &kept) {
    free(kept.data);
    kept.data = nullptr;
}

void Cartridge::dropKeptRoms(std::string ndsPath, std::string gbaPath) {
    // Free what won't be used, before anything new is loaded, so two ROMs are never in memory at once
    if (keptRoms[0].path != ndsPath || !Settings::romInRam) freeKeptRom(keptRoms[0]);
    if (keptRoms[1].path != gbaPath) freeKeptRom(keptRoms[1]);
}

void Cartridge::keepRom() {
    // Hand over a fully loaded ROM for the next core, with no loading progress left to show
    if (!rom || romFd != -1 || !romLoaded.load(std::memory_order_acquire)) return;
    KeptRom &kept = keptRoms[this == &core->cartridgeGba];
    freeKeptRom(kept);
    kept.path = romPath;
    kept.data = rom;
    kept.size = romSize;
    kept.dsiMode = core->dsiMode;
    kept.dldiPatched = core->dldi.isPatched();
    rom = nullptr;
    loadPercent.store(-1);
}

bool Cartridge::takeKeptRom() {
    // Use the ROM kept from the last core if it's this one, or free it to make room
    KeptRom &kept = keptRoms[this == &core->cartridgeGba];
    if (!kept.data) return false;
    if (romFd != -1 || kept.path != romPath || kept.size != romSize || kept.dsiMode != core->dsiMode) {
        freeKeptRom(kept);
        return false;
    }

    // Read from memory like a ROM that was just loaded into RAM, which needs the file no more
    free(rom);
    rom = kept.data;
    romBufSize = romSize;
    kept.data = nullptr;
    if (kept.dldiPatched) core->dldi.setPatched();
    {
        std::lock_guard<FileLock> guard(fileLock());
        fclose(romFile);
    }
    romFile = nullptr;
    setRomLoaded();
    return true;
}

void Cartridge::waitRomLoad() {
    // Wait for a ROM that's being loaded into RAM to be fully loaded, giving up if loading is cancelled
    // The loading thread is stopped when the cartridge is destroyed, as the core is thrown out
    SpinWait spin;
    while (romBlocks && !romLoaded.load(std::memory_order_acquire)) {
        if (cancelLoad.load()) throw ERROR_CANCELLED;
        spin.pause();
    }
}

void Cartridge::ensureLoaded(size_t offset, size_t size) {
    // Make sure part of a ROM that's being loaded into RAM is there, loading blocks that aren't from file
    if (!romBlocks || romLoaded.load(std::memory_order_acquire)) return;
    size_t end = std::min(offset + size, (size_t)romSize);
    for (size_t block = offset / BLOCK_SIZE; block * BLOCK_SIZE < end; block++) {
        if (romBlocks[block].load(std::memory_order_acquire) == 2) continue;
        HB_TIMER(hbMiss);
        HB_SET(HB_ROM_WAIT_BLOCK, block + 1);
        // Let the file's worker handle the miss too, sleeping until it publishes the completed block
        std::unique_lock<std::mutex> lock(loadMutex);
        loadWant = block;
        loadCond.wait(lock, [&] { return romBlocks[block].load(std::memory_order_acquire) == 2; });
        loadWant = size_t(-1);
        HB_SET(HB_ROM_WAIT_BLOCK, 0);
        HB_INC(HB_ROM_MISSES);
        HB_ADD_US(HB_ROM_WAIT_US, hbMiss);
    }
}

void Cartridge::loadRomBlocks(size_t first, size_t count) {
    // Read and patch privately: file I/O and DLDI patches must not touch blocks already published to the emulator
    // Include the preceding driver's tail and enough lookahead to patch drivers crossing the end of this read
    alignas(64) uint8_t data[BLOCK_SIZE * PREFETCH_BLOCKS * 2 + 0x200];
    size_t start = first * BLOCK_SIZE;
    size_t size = std::min(count * BLOCK_SIZE, (size_t)romSize - start);
    size_t back = first ? 0x100 : 0;
    readRomFile(loadFile, data, start - back, size + back + 0x100);

    if (!core->dsiMode)
        core->dldi.patchRom(data, start - back, size + back);
    memcpy(&rom[start], &data[back], size);
    for (size_t i = first; i < first + count; i++)
        romBlocks[i].store(2, std::memory_order_release);
    blocksLoaded += count;
    loadPercent.store(blocksLoaded * 100 / ((romSize + BLOCK_SIZE - 1) / BLOCK_SIZE));
}

void Cartridge::runRomLoad() {
    // Own all file reads after startup, loading urgent blocks before continuing the full-ROM scan
    size_t blocks = (romSize + BLOCK_SIZE - 1) / BLOCK_SIZE;
    for (size_t block = 0; block < blocks && !stopLoad.load();) {
        HB_SET(HB_ROM_LOAD_STAGE, 1);
        size_t first, count = 1;
        {
            std::lock_guard<std::mutex> guard(loadMutex);
            if (loadWant < blocks && romBlocks[loadWant].load(std::memory_order_acquire) != 2) {
                first = loadWant;
            }
            else {
                while (block < blocks && romBlocks[block].load(std::memory_order_acquire) == 2) block++;
                if (block == blocks) break;
                first = block;
                while (count < PREFETCH_BLOCKS * 2 && first + count < blocks &&
                        romBlocks[first + count].load(std::memory_order_acquire) == 0)
                    count++;
                block += count;
            }
            for (size_t i = first; i < first + count; i++)
                romBlocks[i].store(1, std::memory_order_relaxed);
        }

        HB_SET(HB_ROM_LOAD_STAGE, 2);
        loadRomBlocks(first, count);
        // Pair notification with the waiter's mutex so a completed read can't lose its wakeup
        {
            std::lock_guard<std::mutex> guard(loadMutex);
            loadCond.notify_all();
        }
        HB_SET(HB_ROM_LOAD_BLOCK, block);
    }

    // Every claimed read has completed on this worker, so no other thread can still be using the file
    if (!stopLoad.load()) finishRomLoad();
}

void Cartridge::runFullLoad() {
    // Nothing reads the ROM until it's all loaded, so read it straight into place, in pieces as large as reading gets
    // faster for while still being able to stop; patches can read and write a little past the end of the ROM
    const size_t romEnd = romSize, pieceSize = 0x100000, lookahead = 0x100;
    memset(&rom[romEnd], 0xFF, lookahead);
    size_t scanned = 0;
    for (size_t pos = 0; pos < romEnd && !stopLoad.load(); pos += pieceSize) {
        HB_SET(HB_ROM_LOAD_STAGE, 2);
        size_t end = std::min(pos + pieceSize, romEnd);
        readRomFile(loadFile, &rom[pos], pos, end - pos);

        // Patch what's been read, leaving the end for the next piece in case a driver continues there
        size_t patchEnd = (end == romEnd) ? end : end - lookahead;
        if (!core->dsiMode)
            core->dldi.patchRom(&rom[scanned], scanned, patchEnd - scanned);
        scanned = patchEnd;
        loadPercent.store(uint64_t(end) * 100 / romEnd);
        HB_SET(HB_ROM_LOAD_BLOCK, end / BLOCK_SIZE);
    }
    if (!stopLoad.load()) finishRomLoad();
}

void Cartridge::finishRomLoad() {
    HB_SET(HB_ROM_LOAD_STAGE, 5);
    {
        std::lock_guard<FileLock> guard(fileLock());
        fclose(loadFile);
    }
    loadFile = nullptr;
    romLoaded.store(true, std::memory_order_release);
    HB_SET(HB_ROM_LOAD_STAGE, 6);
#ifdef ROM_LOAD_TEST
    // Log how long the load took
    if (FILE *log = fopen((Settings::basePath + "/romload.txt").c_str(), "w")) {
        uint64_t us = OSTicksToMicroseconds(OSGetSystemTime() - loadStart);
        fprintf(log, "Keep ROM in RAM = %d: %.2f s for %d bytes, %.2f MB/s\n", Settings::romInRam, us / 1000000.0, romSize,
            romSize / 1048576.0 / (us / 1000000.0));
        fclose(log);
    }
#endif
}

void Cartridge::writeSave() {
    // Update the save file if the data changed
    mutex.lock();
    if (saveDirty) {
        std::lock_guard<FileLock> guard(fileLock());
        if (FILE *saveFile = (saveFd == -1) ? fopen(savePath.c_str(), "wb") : fdopen(dup(saveFd), "wb")) {
            // Overwrite and resize without closing the file descriptor
            if (saveFd != -1) {
                fseek(saveFile, 0, SEEK_SET);
                ftruncate(saveFd, saveSize);
            }

            LOG_INFO("Writing save file to disk\n");
            fwrite(save, sizeof(uint8_t), saveSize, saveFile);
            fclose(saveFile);
            saveDirty = false;
        }
    }
    mutex.unlock();
}

void Cartridge::trimRom() {
    // Wait for a ROM that's being loaded into RAM to be fully loaded
    SpinWait spin;
    while (romBlocks && !romLoaded.load())
        spin.pause();

    // Starting from the end, reduce the ROM size until a non-filler word is found
    int newSize;
    for (newSize = romSize & ~3; newSize > 0; newSize -= 4) {
        if (U8TO32(rom, newSize - 4) != 0xFFFFFFFF)
            break;
    }

    if (newSize < romSize) {
        // Update the ROM in memory
        romSize = newSize;
        uint8_t *newRom = allocRom(newSize);
        memcpy(newRom, rom, newSize * sizeof(uint8_t));
        free(rom);
        rom = newRom;
        romBufSize = newSize;

        // Update the ROM file
        FILE *romFile = (romFd == -1) ? fopen(romPath.c_str(), "wb") : fdopen(dup(romFd), "wb");
        if (romFile) {
            if (newSize > 0)
                fwrite(rom, sizeof(uint8_t), newSize, romFile);
            fclose(romFile);
        }
    }
}

void Cartridge::resizeSave(int newSize, bool dirty) {
    mutex.lock();
    uint8_t *newSave = new uint8_t[newSize];

    // Resize the save
    if (saveSize < newSize) { // New save is larger
        // Copy all of the old save and fill the rest with 0xFF
        if (saveSize < 0) saveSize = 0;
        memcpy(newSave, save, saveSize * sizeof(uint8_t));
        memset(&newSave[saveSize], 0xFF, (newSize - saveSize) * sizeof(uint8_t));
    }
    else { // New save is smaller
        // Copy as much of the old save as possible
        memcpy(newSave, save, newSize * sizeof(uint8_t));
    }

    // Swap the old save for the new one
    delete[] save;
    save = newSave;
    saveSize = newSize;
    if (dirty) saveDirty = true;
    mutex.unlock();
}

void CartridgeNds::saveState(FILE *file) {
    // Write state data to the file
    fwrite(&saveSize, sizeof(saveSize), 1, file);
    if (saveSize > 0) fwrite(save, 1, saveSize, file);
    fwrite(&cmdMode, sizeof(cmdMode), 1, file);
    fwrite(encTable, 4, sizeof(encTable) / 4, file);
    fwrite(encCode, 4, sizeof(encCode) / 4, file);
    fwrite(romAddrReal, 4, sizeof(romAddrReal) / 4, file);
    fwrite(romAddrVirt, 4, sizeof(romAddrVirt) / 4, file);
    fwrite(blockSize, 2, sizeof(blockSize) / 2, file);
    fwrite(readCount, 2, sizeof(readCount) / 2, file);
    fwrite(wordCycles, 4, sizeof(wordCycles) / 4, file);
    fwrite(encrypted, sizeof(bool), sizeof(encrypted) / sizeof(bool), file);
    fwrite(dsiKey, sizeof(bool), sizeof(dsiKey) / sizeof(bool), file);
    fwrite(auxCommand, 1, sizeof(auxCommand), file);
    fwrite(auxAddress, 4, sizeof(auxAddress) / 4, file);
    fwrite(auxWriteCount, 4, sizeof(auxWriteCount) / 4, file);
    fwrite(auxSpiCnt, 2, sizeof(auxSpiCnt) / 2, file);
    fwrite(auxSpiData, 1, sizeof(auxSpiData), file);
    fwrite(romCtrl, 4, sizeof(romCtrl) / 4, file);
    fwrite(romCmdOut, 8, sizeof(romCmdOut) / 8, file);
}

void CartridgeNds::loadState(FILE *file) {
    // Read state data from the file
    fread(&saveSize, sizeof(saveSize), 1, file);
    if (saveSize > 0) fread(save, 1, saveSize, file);
    fread(&cmdMode, sizeof(cmdMode), 1, file);
    fread(encTable, 4, sizeof(encTable) / 4, file);
    fread(encCode, 4, sizeof(encCode) / 4, file);
    fread(romAddrReal, 4, sizeof(romAddrReal) / 4, file);
    fread(romAddrVirt, 4, sizeof(romAddrVirt) / 4, file);
    fread(blockSize, 2, sizeof(blockSize) / 2, file);
    fread(readCount, 2, sizeof(readCount) / 2, file);
    fread(wordCycles, 4, sizeof(wordCycles) / 4, file);
    fread(encrypted, sizeof(bool), sizeof(encrypted) / sizeof(bool), file);
    fread(dsiKey, sizeof(bool), sizeof(dsiKey) / sizeof(bool), file);
    fread(auxCommand, 1, sizeof(auxCommand), file);
    fread(auxAddress, 4, sizeof(auxAddress) / 4, file);
    fread(auxWriteCount, 4, sizeof(auxWriteCount) / 4, file);
    fread(auxSpiCnt, 2, sizeof(auxSpiCnt) / 2, file);
    fread(auxSpiData, 1, sizeof(auxSpiData), file);
    fread(romCtrl, 4, sizeof(romCtrl) / 4, file);
    wordWaiting = false;
    fread(romCmdOut, 8, sizeof(romCmdOut) / 8, file);

    // Don't overwrite the save file right away; wait until it's modified
    saveDirty = false;
}

bool CartridgeNds::loadRom() {
    // Set the valid NDS save sizes
    if (saveSizes.empty()) {
        saveSizes.push_back(0x000000); // None
        saveSizes.push_back(0x000200); // EEPROM 0.5KB
        saveSizes.push_back(0x002000); // EEPROM 8KB
        saveSizes.push_back(0x008000); // FRAM 32KB
        saveSizes.push_back(0x010000); // EEPROM 64KB
        saveSizes.push_back(0x020000); // EEPROM 128KB
        saveSizes.push_back(0x040000); // FLASH 256KB
        saveSizes.push_back(0x080000); // FLASH 512KB
        saveSizes.push_back(0x100000); // FLASH 1024KB
        saveSizes.push_back(0x800000); // FLASH 8192KB
    }

    // Try to load the ROM into RAM if enabled; otherwise fall back to file-based loading
    if (!Cartridge::loadRom()) {
        return false;
    }
    else if (Settings::romInRam && takeKeptRom()) {
        // The ROM is already in RAM from the last core
    }
    else if (Settings::romInRam) {
        // Load the ROM into RAM on another thread, or fall back to reading from file if there isn't room
        // With streaming, the game starts right away and reads load what they need first; otherwise, wait for it all
        if (!startRomLoad()) {
            loadRomSection(0, 0x5000);
        }
        else if (Settings::romInRam != 2) {
            waitRomLoad();
        }
    }
    else {
        loadRomSection(0, 0x5000);
    }

    // Get logo data from the ROM header
    core->memory.copyBiosLogo(&rom[0xC0]);

    // Calculate the mask for ROM mirroring
    for (romMask = 1; romMask < romSize; romMask <<= 1);
    romMask -= 1;

    // Save the ROM code, which is mainly used for encryption
    romCode = U8TO32(rom, 0x0C);

    // ROM dumps don't provide a chip ID, so make one up from the ROM's size like melonDS, marking DSi titles with a
    // region; the DSi menu doesn't show a card whose ID doesn't fit it
    uint32_t size = std::max(romMask + 1, 0x100000U);
    chipId = 0xC2 | (((size <= 0x8000000) ? ((size >> 20) - 1) : (0x100 - (size >> 28))) << 8);
    dsiCart = (rom[0x12] & BIT(1)) && U8TO32(rom, 0x1B0);
    if (dsiCart) chipId |= BIT(30);

    // DSi cards keep their DSi code in an area of its own, given in 512KB units
    dsiBase = U8TO16(rom, 0x92) << 19;

    // Check if the ROM is encrypted
    if (romSize >= 0x8000) { // ROM has secure area
        // Decrypt the 'encryObj' string
        uint64_t data = U8TO64(rom, 0x4000);
        initKeycode(2);
        data = decrypt64(data);
        initKeycode(3);
        data = decrypt64(data);

        // If decryption was successful, the ROM is encrypted
        if (data == 0x6A624F7972636E65) { // encryObj
            LOG_INFO("Detected an encrypted ROM!\n");
            romEncrypted = true;
        }
    }
    return true;
}

void CartridgeNds::loadNdsCode(bool arm7) {
    // Extract information about initial NDS code from the header
    if (romFile) loadRomSection(0, 0x200);
    uint32_t offset = U8TO32(rom, arm7 ? 0x30 : 0x20);
    core->interpreter[arm7].entryAddr = U8TO32(rom, arm7 ? 0x34 : 0x24);
    uint32_t address = U8TO32(rom, arm7 ? 0x38 : 0x28);
    uint32_t size = U8TO32(rom, arm7 ? 0x3C : 0x2C);
    LOG_INFO("ARM%d code ROM offset: 0x%X\n", arm7 ? 7 : 9, offset);
    LOG_INFO("ARM%d code entry address: 0x%X\n", arm7 ? 7 : 9, core->interpreter[arm7].entryAddr);
    LOG_INFO("ARM%d RAM address: 0x%X\n", arm7 ? 7 : 9, address);
    LOG_INFO("ARM%d code size: 0x%X\n", arm7 ? 7 : 9, size);

    // Load the code data from file if needed
    uint32_t offset2 = offset;
    if (romFile) {
        loadRomSection(offset, size);
        offset = 0;
    }
    else {
        ensureLoaded(offset, size);
    }

    // Copy the initial NDS code into memory
    for (uint32_t i = 0; i < size && offset2 + i < romSize; i += 4) {
        if (romEncrypted && offset2 + i >= 0x4000 && offset2 + i < 0x4800) {
            if (offset2 + i < 0x4008) {
                // Overwrite the 'encryObj' string
                core->memory.write<uint32_t>(arm7, address + i, 0xE7FFDEFF);
            }
            else {
                // Decrypt the first 2KB of the secure area
                initKeycode(3);
                uint64_t data = decrypt64(U8TO64(rom, (offset + i) & ~0x7));
                core->memory.write<uint32_t>(arm7, address + i, data >> (((offset + i) & 0x4) ? 32 : 0));
            }
        }
        else {
            core->memory.write<uint32_t>(arm7, address + i, U8TO32(rom, offset + i));
        }
    }
}

void CartridgeNds::loadDsiCode(bool arm7) {
    // Extract information about initial DSi code from the header
    if (romFile) loadRomSection(0, 0x200);
    uint32_t offset = U8TO32(rom, arm7 ? 0x1D0 : 0x1C0);
    uint32_t address = U8TO32(rom, arm7 ? 0x1D8 : 0x1C8);
    uint32_t size = U8TO32(rom, arm7 ? 0x1DC : 0x1CC);
    LOG_INFO("ARM%di code ROM offset: 0x%X\n", arm7 ? 7 : 9, offset);
    LOG_INFO("ARM%di RAM address: 0x%X\n", arm7 ? 7 : 9, address);
    LOG_INFO("ARM%di code size: 0x%X\n", arm7 ? 7 : 9, size);

    // Load the code data from file if needed
    uint32_t offset2 = offset;
    if (romFile) {
        loadRomSection(offset, size);
        offset = 0;
    }
    else {
        ensureLoaded(offset, size);
    }

    // Get the initial DSi code
    std::vector<uint32_t> code;
    for (uint32_t i = 0; i < size && offset2 + i < romSize; i += 4)
        code.push_back(U8TO32(rom, offset + i));

    // Decrypt the parts in modcrypt areas, which the DSi's launcher does with AES-CTR before starting a game
    // The key comes from "Nintendo" and the game code twice, and the start of the ARM9i hash; the IV is the start of the
    // ARM9 or ARM7 hash, for the first or second area
    if (romFile) loadRomSection(0, 0x400);
    if (rom[0x1C] & BIT(1)) {
        uint32_t code9 = U8TO32(rom, 0xC);
        uint32_t keyX[4] = { 0x746E694E, 0x6F646E65, code9, __builtin_bswap32(code9) };
        uint32_t keyY[4], iv[4];
        for (int area = 0; area < 2; area++) {
            uint32_t start = U8TO32(rom, 0x220 + area * 8), end = start + U8TO32(rom, 0x224 + area * 8);
            uint32_t from = std::max(start, offset2), to = std::min(end, offset2 + uint32_t(code.size() * 4));
            if (from >= to) continue;
            for (int i = 0; i < 4; i++) {
                keyY[i] = U8TO32(rom, 0x350 + i * 4);
                iv[i] = U8TO32(rom, 0x300 + area * 0x14 + i * 4);
            }
            LOG_INFO("Decrypting modcrypt area %d of the ARM%di code: 0x%X bytes\n", area + 1, arm7 ? 7 : 9, to - from);
            core->aes.decryptCtr(keyX, keyY, iv, (from - start) >> 4, &code[(from - offset2) >> 2], (to - from) >> 2);
        }
    }

    // Copy the initial DSi code into memory
    for (uint32_t i = 0; i < code.size(); i++)
        core->memory.write<uint32_t>(arm7, address + i * 4, code[i]);
}

void CartridgeNds::directBoot() {
    // Load the ROM header and check for DSi mode
    if (romFile) loadRomSection(0, 0x1000);
    bool dsi = core->dsiMode && (rom[0x12] & BIT(1));

    // The system has read a DSi title's DSi area by now, which the title can then read too
    dsiKey[0] = dsiKey[1] = dsi && dsiCart;

    // Handle the header based on DSi mode
    if (dsi) {
        // Apply the header's WRAM mappings
        core->memory.writeWramCnt(rom[0x1AF]);
        for (int i = 0; i < 4; i++) {
            core->memory.writeMbk1(i, rom[0x180 + i]);
            core->memory.writeMbk23(i + 0, rom[0x184 + i]);
            core->memory.writeMbk23(i + 4, rom[0x188 + i]);
            core->memory.writeMbk45(i + 0, rom[0x18C + i]);
            core->memory.writeMbk45(i + 4, rom[0x190 + i]);
            if (i >= 2) continue;
            core->memory.writeMbk6(i, -1, U8TO32(rom, 0x194 + i * 0xC));
            core->memory.writeMbk7(i, -1, U8TO32(rom, 0x198 + i * 0xC));
            core->memory.writeMbk8(i, -1, U8TO32(rom, 0x19C + i * 0xC));
        }
        core->memory.writeMbk9(-1, U8TO32(rom, 0x1AC));

        // Enable the DSi features as the system does for a DSi title, with some of the ARM7's from the header. The system
        // also takes access to the SCFG and MBK registers from the header, and sets things up for titles it turns off;
        // like melonDS, leave access on instead, so the title's own startup code sets those up and turns it off
        core->memory.setScfgExt(0x8307F100, 0x93FBFB00 | (U8TO32(rom, 0x1B8) & 0x00040407));

        // Copy the DSi ROM header into memory
        for (uint32_t i = 0; i < 0x1000; i += 4)
            core->memory.write<uint32_t>(0, 0x2FFE000 + i, U8TO32(rom, i));
    }
    else if (core->dsiMode) {
        // Leave only the DS features, as the system does for a DS title
        core->memory.setScfgExt(0x03000000, 0x12A03000);
    }
    if (core->dsiMode)
        core->memory.prepareTitle(dsi);

    // Copy the NDS ROM header into memory, which DSi games read too, like for where their files are
    for (uint32_t i = 0; i < 0x170; i += 4)
        core->memory.write<uint32_t>(0, 0x2FFFE00 + i, U8TO32(rom, i));

    // Load initial NDS/DSi code segments
    loadNdsCode(false);
    loadNdsCode(true);
    if (!dsi) return;
    loadDsiCode(false);
    loadDsiCode(true);
}

uint64_t CartridgeNds::encrypt64(uint64_t value) {
    // Encrypt a 64-bit value using the Blowfish algorithm
    // This is a translation of the pseudocode from GBATEK to C++

    uint32_t y = value;
    uint32_t x = value >> 32;

    for (int i = 0x00; i <= 0x0F; i++) {
        uint32_t z = encTable[i] ^ x;
        x = encTable[0x012 + ((z >> 24) & 0xFF)];
        x = encTable[0x112 + ((z >> 16) & 0xFF)] + x;
        x = encTable[0x212 + ((z >> 8) & 0xFF)] ^ x;
        x = encTable[0x312 + ((z >> 0) & 0xFF)] + x;
        x ^= y;
        y = z;
    }

    return ((uint64_t)(y ^ encTable[0x11]) << 32) | (x ^ encTable[0x10]);
}

uint64_t CartridgeNds::decrypt64(uint64_t value) {
    // Decrypt a 64-bit value using the Blowfish algorithm
    // This is a translation of the pseudocode from GBATEK to C++

    uint32_t y = value;
    uint32_t x = value >> 32;

    for (int i = 0x11; i >= 0x02; i--) {
        uint32_t z = encTable[i] ^ x;
        x = encTable[0x012 + ((z >> 24) & 0xFF)];
        x = encTable[0x112 + ((z >> 16) & 0xFF)] + x;
        x = encTable[0x212 + ((z >> 8) & 0xFF)] ^ x;
        x = encTable[0x312 + ((z >> 0) & 0xFF)] + x;
        x ^= y;
        y = z;
    }

    return ((uint64_t)(y ^ encTable[0x00]) << 32) | (x ^ encTable[0x01]);
}

void CartridgeNds::powerReset() {
    // Start over as a card does when the DSi powers its slot off or holds it in reset
    cmdMode = CMD_NONE;
    for (int i = 0; i < 2; i++) {
        encrypted[i] = dsiKey[i] = false;
        romCtrl[i] &= ~BIT(29);
    }
}

void CartridgeNds::initKeycode(int level, bool dsi) {
    // Initialize the Blowfish encryption table, with the DSi's other table for the DSi parts of DSi cards
    // This is a translation of the pseudocode from GBATEK to C++

    for (int i = 0; i < 0x412; i++)
        encTable[i] = core->memory.readKeyTable(i, dsi);

    encCode[0] = romCode;
    encCode[1] = romCode / 2;
    encCode[2] = romCode * 2;

    if (level >= 1) applyKeycode();
    if (level >= 2) applyKeycode();

    encCode[1] *= 2;
    encCode[2] /= 2;

    if (level >= 3) applyKeycode();
}

void CartridgeNds::applyKeycode() {
    // Apply a keycode to the Blowfish encryption table
    // This is a translation of the pseudocode from GBATEK to C++
    
    uint64_t enc1 = encrypt64(((uint64_t)encCode[2] << 32) | encCode[1]);
    encCode[1] = enc1;
    encCode[2] = enc1 >> 32;

    uint64_t enc2 = encrypt64(((uint64_t)encCode[1] << 32) | encCode[0]);
    encCode[0] = enc2;
    encCode[1] = enc2 >> 32;

    for (int i = 0; i <= 0x11; i++) {
        uint32_t byteReverse = 0;
        for (int j = 0; j < 4; j++)
            byteReverse |= ((encCode[i % 2] >> (j * 8)) & 0xFF) << ((3 - j) * 8);

        encTable[i] ^= byteReverse;
    }

    uint64_t scratch = 0;

    for (int i = 0; i <= 0x410; i += 2) {
        scratch = encrypt64(scratch);
        encTable[i + 0] = scratch >> 32;
        encTable[i + 1] = scratch;
    }
}

void CartridgeNds::wordReady(bool cpu) {
    // Indicate that a word is ready, which can start a new burst of ARM7 words
    romCtrl[cpu] |= BIT(23);
    if (cpu == 1) {
        burstLeft = BURST_WORDS - 1;
        burstDone = 0;
    }

    // Trigger DS cartridge DMA transfers
    core->dma[cpu].trigger((cpu == 0) ? 5 : 2);
}

void CartridgeNds::scheduleWord(bool cpu) {
    // Games read a lot of words while loading, and a scheduled task for each makes the CPU stop running compiled code
    // So if nothing needs to happen when an ARM9 word is ready, just note when that is for reads to check
    // The ARM7 can run behind the global cycles, so its words are always scheduled
    if (cpu == 0 && !core->dma[0].cartArmed()) {
        wordWaiting = true;
        wordTime = core->globalCycles + wordCycles[0];
        return;
    }

    // The DSi's launcher reads cards with ARM7 DMA, and a task for each word keeps the ARM9 from running compiled code
    // So with fast timing, a DMA takes several words at once, and the next task comes later to make up for it
    if (cpu == 1 && burstLeft && Settings::fastTiming && core->dma[1].cartArmed()) {
        burstLeft--;
        burstDone++;
        romCtrl[1] |= BIT(23);
        return;
    }
    core->schedule(SchedTask(CART9_WORD_READY + cpu), wordCycles[cpu] * (cpu ? burstDone + 1 : 1));
}

void CartridgeNds::checkWord() {
    // Mark a noted word as ready once its time has come
    if (wordWaiting && int32_t(core->globalCycles - wordTime) >= 0) {
        wordWaiting = false;
        romCtrl[0] |= BIT(23);
    }
}

void CartridgeNds::flushWord() {
    // Schedule a noted word for real, for when something depends on the task being there
    checkWord();
    if (!wordWaiting) return;
    wordWaiting = false;
    core->schedule(CART9_WORD_READY, wordTime - core->globalCycles);
}

void CartridgeNds::resetCycles() {
    // Move a noted word's time along with the global cycles; one that's due is handled first, so it can't wrap
    checkWord();
    if (wordWaiting) wordTime -= core->globalCycles;
}

void CartridgeNds::writeAuxSpiCnt(bool cpu, uint16_t mask, uint16_t value) {
    // Write to one of the AUXSPICNT registers
    mask &= 0xE043;
    auxSpiCnt[cpu] = (auxSpiCnt[cpu] & ~mask) | (value & mask);
}

void CartridgeNds::writeRomCmdOutL(bool cpu, uint32_t mask, uint32_t value) {
    // Write to one of the ROMCMDOUT registers (low)
    romCmdOut[cpu] = (romCmdOut[cpu] & ~((uint64_t)mask)) | (value & mask);
}

void CartridgeNds::writeRomCmdOutH(bool cpu, uint32_t mask, uint32_t value) {
    // Write to one of the ROMCMDOUT registers (high)
    romCmdOut[cpu] = (romCmdOut[cpu] & ~((uint64_t)mask << 32)) | ((uint64_t)(value & mask) << 32);
}

void CartridgeNds::writeAuxSpiData(bool cpu, uint8_t value) {
    // Do nothing if there is no save
    if (saveSize == 0) return;

    if (auxWriteCount[cpu] == 0) {
        // On the first write, set the command byte
        if (value == 0) return;
        auxCommand[cpu] = value;
        auxAddress[cpu] = 0;
        auxSpiData[cpu] = 0;
    }
    else {
        // Incredibly naive save type detection, based on commands that might be sent
        if (saveSize == -1) {
            switch (auxCommand[cpu]) {
            case 0x0B: // EEPROM 0.5KB: Read from upper memory
                LOG_INFO("Detected EEPROM 0.5KB save type\n");
                resizeSave(0x200, false);
                break;

            case 0x02: // EEPROM 64KB: Write to memory
                LOG_INFO("Detected EEPROM 64KB save type\n");
                resizeSave(0x10000, false);
                break;

            case 0x0A: // FLASH 512KB: Page write
                LOG_INFO("Detected FLASH 512KB save type\n");
                resizeSave(0x80000, false);
                break;

            case 0x03: // EEPROM 0.5KB: Read from lower memory
                // Only EEPROM 0.5KB has a 1 byte address, so a read that ends after it with 1 data byte must be from one
                // Other types need more address bytes, so a read that short would get no data from them
                if (auxWriteCount[cpu] == 2 && !(auxSpiCnt[cpu] & BIT(6))) {
                    LOG_INFO("Detected EEPROM 0.5KB save type\n");
                    resizeSave(0x200, false);
                    break;
                }
                if (auxWriteCount[cpu] == 1)
                    auxAddress[cpu] = value;
                // Keep track of the write count
                if (auxSpiCnt[cpu] & BIT(6)) // Keep chip selected
                    auxWriteCount[cpu]++;
                else // Deselect chip
                    auxWriteCount[cpu] = 0;
                return;

            default:
                // Deselect chip
                if (!(auxSpiCnt[cpu] & BIT(6)))
                    auxWriteCount[cpu] = 0;
                return;
            }
        }

        switch (saveSize) {
        case 0x200: // EEPROM 0.5KB
            switch (auxCommand[cpu]) {
            case 0x03: // Read from lower memory
                if (auxWriteCount[cpu] < 2) {
                    // On the second write, set the 1 byte address to read from
                    auxAddress[cpu] = value;
                    auxSpiData[cpu] = 0;
                }
                else {
                    // On writes 3+, read data from the save and send it back
                    auxSpiData[cpu] = (auxAddress[cpu] < 0x200) ? save[auxAddress[cpu]] : 0;
                    auxAddress[cpu]++;
                }
                break;

            case 0x0B: // Read from upper memory
                if (auxWriteCount[cpu] < 2) {
                    // On the second write, set the 1 byte address to read from
                    auxAddress[cpu] = 0x100 + value;
                    auxSpiData[cpu] = 0;
                }
                else {
                    // On writes 3+, read data from the save and send it back
                    auxSpiData[cpu] = (auxAddress[cpu] < 0x200) ? save[auxAddress[cpu]] : 0;
                    auxAddress[cpu]++;
                }
                break;

            case 0x02: // Write to lower memory
                if (auxWriteCount[cpu] < 2) {
                    // On the second write, set the 1 byte address to write to
                    auxAddress[cpu] = value;
                    auxSpiData[cpu] = 0;
                }
                else {
                    // On writes 3+, write data to the save
                    if (auxAddress[cpu] < 0x200) {
                        mutex.lock();
                        save[auxAddress[cpu]] = value;
                        saveDirty = true;
                        mutex.unlock();
                    }

                    auxAddress[cpu]++;
                    auxSpiData[cpu] = 0;
                }
                break;

            case 0x0A: // Write to upper memory
                if (auxWriteCount[cpu] < 2) {
                    // On the second write, set the 1 byte address to write to
                    auxAddress[cpu] = 0x100 + value;
                    auxSpiData[cpu] = 0;
                }
                else {
                    // On writes 3+, write data to the save
                    if (auxAddress[cpu] < 0x200) {
                        mutex.lock();
                        save[auxAddress[cpu]] = value;
                        saveDirty = true;
                        mutex.unlock();
                    }

                    auxAddress[cpu]++;
                    auxSpiData[cpu] = 0;
                }
                break;

            default:
                LOG_CRIT("Write to AUX SPI with unknown EEPROM 0.5KB command: 0x%X\n", auxCommand[cpu]);
                auxSpiData[cpu] = 0;
                break;
            }
            break;

        case 0x2000: case 0x8000: case 0x10000: case 0x20000: // EEPROM 8KB, 64KB, 128KB; FRAM 32KB
            switch (auxCommand[cpu]) {
            case 0x03: // Read from memory
                if (auxWriteCount[cpu] < ((saveSize == 0x20000) ? 4 : 3)) {
                    // On writes 2-3, set the 2 byte address to read from (not EEPROM 128KB)
                    // EEPROM 128KB uses a 3 byte address, so it's set on writes 2-4
                    auxAddress[cpu] |= value << ((((saveSize == 0x20000) ? 3 : 2) - auxWriteCount[cpu]) * 8);
                    auxSpiData[cpu] = 0;
                }
                else {
                    // On writes 4+, read data from the save and send it back
                    auxSpiData[cpu] = (auxAddress[cpu] < saveSize) ? save[auxAddress[cpu]] : 0;
                    auxAddress[cpu]++;
                }
                break;

            case 0x02: // Write to memory
                if (auxWriteCount[cpu] < ((saveSize == 0x20000) ? 4 : 3)) {
                    // On writes 2-3, set the 2 byte address to write to (not EEPROM 128KB)
                    // EEPROM 128KB uses a 3 byte address, so it's set on writes 2-4
                    auxAddress[cpu] |= value << ((((saveSize == 0x20000) ? 3 : 2) - auxWriteCount[cpu]) * 8);
                    auxSpiData[cpu] = 0;
                }
                else {
                    // On writes 4+, write data to the save
                    if (auxAddress[cpu] < saveSize) {
                        mutex.lock();
                        save[auxAddress[cpu]] = value;
                        saveDirty = true;
                        mutex.unlock();
                    }

                    auxAddress[cpu]++;
                    auxSpiData[cpu] = 0;
                }
                break;

            default:
                LOG_CRIT("Write to AUX SPI with unknown EEPROM/FRAM command: 0x%X\n", auxCommand[cpu]);
                auxSpiData[cpu] = 0;
                break;
            }
            break;

        case 0x40000: case 0x80000: case 0x100000: case 0x800000: // FLASH 256KB, 512KB, 1024KB, 8192KB
            switch (auxCommand[cpu]) {
            case 0x03: // Read data bytes
                if (auxWriteCount[cpu] < 4) {
                    // On writes 2-4, set the 3 byte address to read from
                    auxAddress[cpu] |= value << ((3 - auxWriteCount[cpu]) * 8);
                    auxSpiData[cpu] = 0;
                }
                else {
                    // On writes 5+, read data from the save and send it back
                    auxSpiData[cpu] = (auxAddress[cpu] < saveSize) ? save[auxAddress[cpu]] : 0xFF;
                    auxAddress[cpu]++;
                }
                break;

            case 0x0A: // Page write
                if (auxWriteCount[cpu] < 4) {
                    // On writes 2-4, set the 3 byte address to write to
                    auxAddress[cpu] |= value << ((3 - auxWriteCount[cpu]) * 8);
                    auxSpiData[cpu] = 0;
                }
                else {
                    // On writes 5+, write data to the save
                    // Commands don't reveal the FLASH size, so grow the save if a game writes past the end of it
                    if (auxAddress[cpu] >= saveSize) {
                        for (int size: { 0x80000, 0x100000, 0x800000 }) {
                            if (auxAddress[cpu] < size) {
                                LOG_INFO("Detected FLASH %dKB save type\n", size / 0x400);
                                resizeSave(size, false);
                                break;
                            }
                        }
                    }

                    if (auxAddress[cpu] < saveSize) {
                        mutex.lock();
                        save[auxAddress[cpu]] = value;
                        saveDirty = true;
                        mutex.unlock();
                    }

                    auxAddress[cpu]++;
                    auxSpiData[cpu] = 0;
                }
                break;

            case 0x08: // IR-related
                // If a gamecode starts with 'I', the game has an infrared port in its cartridge
                // This shares the same SPI as FLASH memory
                // Some games check this command as an anti-piracy measure
                auxSpiData[cpu] = ((romCode & 0xFF) == 'I') ? 0xAA : 0;
                break;

            default:
                LOG_CRIT("Write to AUX SPI with unknown FLASH command: 0x%X\n", auxCommand[cpu]);
                auxSpiData[cpu] = 0;
                break;
            }
            break;

        default:
            LOG_CRIT("Write to AUX SPI with unknown save size: 0x%X\n", saveSize);
            break;
        }
    }

    // Keep track of the write count
    if (auxSpiCnt[cpu] & BIT(6)) // Keep chip selected
        auxWriteCount[cpu]++;
    else // Deselect chip
        auxWriteCount[cpu] = 0;
}

void CartridgeNds::writeRomCtrl(bool cpu, uint32_t mask, uint32_t value) {
    bool transfer = false;

    // Set the release reset bit, but never clear it
    romCtrl[cpu] |= (value & BIT(29));

    // Start a transfer if the start bit changes from 0 to 1
    if (!(romCtrl[cpu] & BIT(31)) && (value & BIT(31)))
        transfer = true;

    // Write to one of the ROMCTRL registers
    mask &= 0xDF7F7FFF;
    romCtrl[cpu] = (romCtrl[cpu] & ~mask) | (value & mask);

    // Set the time to transfer a word: 4 cycles at either 4.2MHz or 6.7MHz
    wordCycles[cpu] = (romCtrl[cpu] & BIT(27)) ? (4 * 8) : (4 * 5);

    if (!transfer) return;

    // Let a word from an earlier transfer finish on its own
    if (cpu == 0) flushWord();

    // Determine the size of the block to transfer
    uint8_t size = (romCtrl[cpu] & 0x07000000) >> 24;
    switch (size) {
        case 0: blockSize[cpu] = 0; break;
        case 7: blockSize[cpu] = 4; break;
        default: blockSize[cpu] = 0x100 << size; break;
    }

    // Reverse the byte order of the ROM command
    uint64_t command = 0;
    for (int i = 0; i < 8; i++)
        command |= ((romCmdOut[cpu] >> (i * 8)) & 0xFF) << ((7 - i) * 8);

    // Decrypt the ROM command if encryption is enabled
    // DSi cards use level 1 of the DSi's table for their DSi parts
    if (encrypted[cpu]) {
        initKeycode(dsiKey[cpu] ? 1 : 2, dsiKey[cpu]);
        command = decrypt64(command);
    }

    cmdMode = CMD_NONE;

    // Interpret the ROM command
    if (rom) {
        if (command == 0x0000000000000000) { // Get header
            cmdMode = CMD_HEADER;

            // Load the header from file if needed
            if (romFile)
                loadRomSection(0, blockSize[cpu]);
            else
                ensureLoaded(0, blockSize[cpu]);
        }
        else if (command == 0x9000000000000000 || (command >> 60) == 0x1 || command == 0xB800000000000000) { // Get chip ID
            cmdMode = CMD_CHIP;
        }
        else if ((command >> 56) == 0x3C) { // Activate KEY1 encryption mode
            // Initialize KEY1 encryption
            encrypted[cpu] = true;
            dsiKey[cpu] = false;
        }
        else if ((command >> 56) == 0x3D && core->dsiMode && dsiCart) { // Activate KEY1 encryption mode for the DSi area
            // Like 0x3C, but for the DSi parts of a DSi card, which only a DSi asks for
            encrypted[cpu] = true;
            dsiKey[cpu] = true;
        }
        else if ((command >> 60) == 0x2) { // Get secure area
            cmdMode = CMD_SECURE;
            romAddrReal[cpu] = ((command & 0x0FFFF00000000000) >> 44) * 0x1000;

            // The DSi area's secure area is at its start, which like the ROM's starts 0x1000 bytes before it
            if (dsiKey[cpu]) romAddrReal[cpu] += dsiBase - 0x1000;

            // Load the secure area block from file if needed
            if (romFile) {
                loadRomSection(romAddrReal[cpu], blockSize[cpu]);
                romAddrVirt[cpu] = 0;
            }
            else {
                ensureLoaded(romAddrReal[cpu], blockSize[cpu]);
                romAddrVirt[cpu] = romAddrReal[cpu];
            }
        }
        else if ((command >> 60) == 0xA) { // Enter main data mode
            // Disable KEY1 encryption
            // On hardware, this is where KEY2 encryption would start
            encrypted[cpu] = false;
        }
        else if ((command >> 56) == 0xB7) { // Get data
            cmdMode = CMD_DATA;
            romAddrReal[cpu] = (command >> 24) & romMask;

            // DSi cards only let the DSi area be read after a DSi asked for it, apart from its first 0x3000 bytes, which
            // read like the ROM's first 0x8000 (from melonDS)
            if (dsiCart && romAddrReal[cpu] >= dsiBase && (!dsiKey[cpu] || romAddrReal[cpu] < dsiBase + 0x3000))
                romAddrReal[cpu] = 0x8000 + (romAddrReal[cpu] & 0x1FF);

            // Load the ROM data from file if needed
            if (romFile) {
                if (romAddrReal[cpu] < 0x8000) {
                    // Workaround for address redirection when loading from file
                    loadRomSection(0, 0x8200 + blockSize[cpu]);
                    romAddrVirt[cpu] = romAddrReal[cpu];
                }
                else {
                    loadRomSection(romAddrReal[cpu], blockSize[cpu]);
                    romAddrVirt[cpu] = 0;
                }
            }
            else {
                ensureLoaded(romAddrReal[cpu], blockSize[cpu]);
                romAddrVirt[cpu] = romAddrReal[cpu];
            }
        }
        else if (command != 0x9F00000000000000) { // Unknown (not dummy)
            LOG_CRIT("ROM transfer with unknown command: 0x%llX\n", command);
        }
    }

    if (blockSize[cpu] == 0) {
        // End the transfer right away if the block size is 0
        romCtrl[cpu] &= ~BIT(23); // Word not ready
        romCtrl[cpu] &= ~BIT(31); // Block ready

        // Trigger a block ready IRQ if enabled
        if (auxSpiCnt[cpu] & BIT(14))
            core->interpreter[cpu].sendInterrupt(19);
    }
    else {
        // Schedule the first word to be ready
        if (cpu == 1) burstLeft = burstDone = 0;
        scheduleWord(cpu);
        readCount[cpu] = 0;
    }
}

uint32_t CartridgeNds::readRomCtrl(bool cpu) {
    // Read from one of the ROMCTRL registers
    if (cpu == 0) checkWord();
    return romCtrl[cpu];
}

uint32_t CartridgeNds::readRomDataIn(bool cpu) {
    // Don't transfer if the word ready bit isn't set
    if (cpu == 0) checkWord();
    if (!(romCtrl[cpu] & BIT(23)))
        return 0;

    // Mark the next word as not ready
    romCtrl[cpu] &= ~BIT(23);

    // Increment the read counter
    if ((readCount[cpu] += 4) == blockSize[cpu]) {
        // End the transfer when the block size has been reached
        romCtrl[cpu] &= ~BIT(31); // Block ready

        // Trigger a block ready IRQ if enabled
        if (auxSpiCnt[cpu] & BIT(14))
            core->interpreter[cpu].sendInterrupt(19);
    }
    else {
        // Schedule the next word to be ready
        scheduleWord(cpu);
    }

    // Return a value from the cart depending on the current command
    switch (cmdMode) {
    case CMD_HEADER:
        // Read the ROM header, repeated every 0x1000 bytes
        return U8TO32(rom, (readCount[cpu] - 4) & 0xFFF);

    case CMD_CHIP:
        // Read the chip ID, repeated every 4 bytes
        return chipId;

    case CMD_SECURE:
        // Encrypt the first 2KB of the secure area
        if (!romEncrypted && romAddrReal[cpu] == 0x4000 && readCount[cpu] <= 0x800) {
            // Supply the 'encryObj' string for the first 8 bytes (overwritten during decryption)
            uint64_t data = (readCount[cpu] <= 8) ? 0x6A624F7972636E65 :
                U8TO64(rom, (romAddrVirt[cpu] + readCount[cpu] - 4) & ~7);

            // Encrypt the data
            initKeycode(3);
            data = encrypt64(data);

            // Double-encrypt the 'encryObj' string
            if (readCount[cpu] <= 8) {
                initKeycode(2);
                data = encrypt64(data);
            }

            return data >> (((romAddrReal[cpu] + readCount[cpu]) & 4) ? 0 : 32);
        }

        // Read data from the selected secure area block
        return U8TO32(rom, romAddrVirt[cpu] + readCount[cpu] - 4);

    case CMD_DATA:
        // Read ROM data from the given address
        // This command can't read the first 32KB of a ROM, so it redirects the address
        // Some games verify that the first 32KB are unreadable as an anti-piracy measure
        uint32_t address = romAddrVirt[cpu] + readCount[cpu] - 4;
        if (romAddrReal[cpu] + readCount[cpu] <= 0x8000) address = 0x8000 + (address & 0x1FF);
        if (address < romSize) return U8TO32(rom, address);
    }

    // Default to endless 0xFFs if there's no actual data to read
    return 0xFFFFFFFF;
}

void CartridgeGba::saveState(FILE *file) {
    // Write state data to the file
    fwrite(&saveSize, sizeof(saveSize), 1, file);
    if (saveSize > 0) fwrite(save, 1, saveSize, file);
    fwrite(&eepromCount, sizeof(eepromCount), 1, file);
    fwrite(&eepromCmd, sizeof(eepromCmd), 1, file);
    fwrite(&eepromData, sizeof(eepromData), 1, file);
    fwrite(&eepromDone, sizeof(eepromDone), 1, file);
    fwrite(&flashCmd, sizeof(flashCmd), 1, file);
    fwrite(&bankSwap, sizeof(bankSwap), 1, file);
    fwrite(&flashErase, sizeof(flashErase), 1, file);
}

void CartridgeGba::loadState(FILE *file) {
    // Read state data from the file
    fread(&saveSize, sizeof(saveSize), 1, file);
    if (saveSize > 0) fread(save, 1, saveSize, file);
    fread(&eepromCount, sizeof(eepromCount), 1, file);
    fread(&eepromCmd, sizeof(eepromCmd), 1, file);
    fread(&eepromData, sizeof(eepromData), 1, file);
    fread(&eepromDone, sizeof(eepromDone), 1, file);
    fread(&flashCmd, sizeof(flashCmd), 1, file);
    fread(&bankSwap, sizeof(bankSwap), 1, file);
    fread(&flashErase, sizeof(flashErase), 1, file);

    // Don't overwrite the save file right away; wait until it's modified
    saveDirty = false;
}

bool CartridgeGba::findString(std::string string) {
    // Scan a GBA ROM for a string and report if it was found
    for (int i = 0; i < romSize; i += 4) {
        bool found = true;

        for (size_t j = 0; j < string.length(); j++) {
            if (i + j >= romSize || rom[i + j] != string[j]) {
                found = false;
                break;
            }
        }

        if (found)
            return true;
    }

    return false;
}

bool CartridgeGba::loadRom() {
    // Set the valid GBA save sizes
    if (saveSizes.empty()) {
        saveSizes.push_back(0x00000); // None
        saveSizes.push_back(0x00200); // EEPROM 0.5KB
        saveSizes.push_back(0x02000); // EEPROM 8KB
        saveSizes.push_back(0x08000); // SRAM 32KB
        saveSizes.push_back(0x10000); // FLASH 64KB
        saveSizes.push_back(0x20000); // FLASH 128KB
    }

    // Load the ROM into memory
    if (!Cartridge::loadRom()) return false;
    if (!takeKeptRom()) {
        loadRomSection(0, romSize);
        fclose(romFile);
        romFile = nullptr;
        setRomLoaded();
    }

    // Calculate the mask for ROM mirroring
    if (romSize > 0xAC && rom[0xAC] == 'F') { // NES classic
        // NES classic ROMs are mirrored based on their size
        for (romMask = 1; romMask < romSize; romMask <<= 1);
        romMask--;
    }
    else {
        // Most ROMs are only mirrored each 32MB wait state area
        romMask = 0x1FFFFFF;
    }

    // Detect if the cart has an RTC by searching for the relevant string
    if (findString("SIIRTC_V"))
        core->rtc.enableGpRtc();

    // Update the memory maps at the GBA ROM locations
    core->memory.updateMap9(0x08000000, 0x0A000000);
    core->memory.updateMap7(0x08000000, 0x0D000000);

    // If the save size is unknown, try to detect it
    if (saveSize == -1) {
        const std::string saveStrs[] = { "EEPROM_V", "SRAM_V", "FLASH_V", "FLASH512_V", "FLASH1M_V" };

        // Unlike the DS, a GBA cart's save type can be detected by searching for strings in the ROM
        // Search the ROM for a save string so a new save of that type can be created
        for (int i = 0; i < 5; i++) {
            if (!findString(saveStrs[i]))
                continue;

            // Create a new GBA save of the detected type
            switch (i) {
            case 0: // EEPROM
                // EEPROM can be either 0.5KB or 8KB, so it must be guessed based on how the game uses it
                return true;

            case 1: // SRAM 32KB
                LOG_INFO("Detected SRAM 32KB save type\n");
                resizeSave(0x8000, false);
                return true;

            case 2: case 3: // FLASH 64KB
                LOG_INFO("Detected FLASH 64KB save type\n");
                resizeSave(0x10000, false);
                return true;

            case 4: // FLASH 128KB
                LOG_INFO("Detected FLASH 128KB save type\n");
                resizeSave(0x20000, false);
                return true;
            }
        }
    }
    return true;
}

uint8_t CartridgeGba::eepromRead() {
    if (saveSize == -1) {
        // Detect the save size based on how many command bits were sent before reading
        if (eepromCount == 9) {
            LOG_INFO("Detected EEPROM 0.5KB save type\n");
            resizeSave(0x200, false);
        }
        else {
            LOG_INFO("Detected EEPROM 8KB save type\n");
            resizeSave(0x2000, false);
        }
    }

    // EEPROM 0.5KB uses 8-bit commands, and EEPROM 8KB uses 16-bit commands
    uint8_t length = (saveSize == 0x200) ? 8 : 16;

    if (((eepromCmd & 0xC000) >> 14) == 0x3 && eepromCount >= length + 1) { // Read
        if (++eepromCount >= length + 6) {
            // Read the data bits, MSB first
            int bit = 63 - (eepromCount - (length + 6));
            uint16_t addr = (saveSize == 0x200) ? ((eepromCmd & 0x3F00) >> 8) : (eepromCmd & 0x03FF);
            uint8_t value = (save[addr * 8 + bit / 8] & BIT(bit % 8)) >> (bit % 8);

            // Reset the transfer at the end
            if (eepromCount >= length + 69) {
                eepromCount = 0;
                eepromCmd = 0;
                eepromData = 0;
            }

            return value;
        }
    }
    else if (eepromDone) {
        // Signal that a write has finished
        return 1;
    }

    return 0;
}

void CartridgeGba::eepromWrite(uint8_t value) {
    eepromDone = false;

    // EEPROM 0.5KB uses 8-bit commands, and EEPROM 8KB uses 16-bit commands
    uint8_t length = (saveSize == 0x200) ? 8 : 16;

    if (eepromCount < length) {
        // Get the command bits
        eepromCmd |= (value & BIT(0)) << (16 - ++eepromCount);
    }
    else if (((eepromCmd & 0xC000) >> 14) == 0x3) { // Read
        // Accept the last bit to finish the read command
        if (eepromCount < length + 1)
            eepromCount++;
    }
    else if (((eepromCmd & 0xC000) >> 14) == 0x2) { // Write
        // Get the data bits, MSB first
        if (++eepromCount <= length + 64)
            eepromData |= (uint64_t)(value & BIT(0)) << (length + 64 - eepromCount);

        if (eepromCount >= length + 65) {
            // Games will probably read first, so the save size can be detected from that
            // If something decides to write first, it becomes a lot harder to detect the size
            // In this case, just assume EEPROM 8KB and create an empty save
            if (saveSize == -1) {
                LOG_INFO("Detected EEPROM 8KB save type\n");
                resizeSave(0x2000, false);
            }

            // Write the data after all the bits have been received
            mutex.lock();
            uint16_t addr = (saveSize == 0x200) ? ((eepromCmd & 0x3F00) >> 8) : (eepromCmd & 0x03FF);
            for (unsigned int i = 0; i < 8; i++)
                save[addr * 8 + i] = eepromData >> (i * 8);
            saveDirty = true;
            mutex.unlock();

            // Reset the transfer
            eepromCount = 0;
            eepromCmd = 0;
            eepromData = 0;
            eepromDone = true;
        }
    }
}

uint8_t CartridgeGba::sramRead(uint32_t address) {
    if (saveSize == 0x8000 && address < 0xE008000) { // SRAM
        // Read a single byte because the data bus is only 8 bits
        return save[address - 0xE000000];
    }
    else if ((saveSize == 0x10000 || saveSize == 0x20000) && address < 0xE010000) { // FLASH
        // Run a FLASH command
        if (flashCmd == 0x90 && address == 0xE000000) {
            // Read the chip manufacturer ID
            return 0xC2;
        }
        else if (flashCmd == 0x90 && address == 0xE000001) {
            // Read the chip device ID
            return (saveSize == 0x10000) ? 0x1C : 0x09;
        }
        else {
            // Read a single byte
            if (bankSwap) address += 0x10000;
            return save[address - 0xE000000];
        }
    }

    return 0xFF;
}

void CartridgeGba::sramWrite(uint32_t address, uint8_t value) {
    if (saveSize == 0x8000 && address < 0xE008000) { // SRAM
        // Write a single byte because the data bus is only 8 bits
        mutex.lock();
        save[address - 0xE000000] = value;
        saveDirty = true;
        mutex.unlock();
    }
    else if ((saveSize == 0x10000 || saveSize == 0x20000) && address < 0xE010000) { // FLASH
        // Run a FLASH command
        if (flashCmd == 0xA0) {
            // Write a single byte
            if (bankSwap) address += 0x10000;
            mutex.lock();
            save[address - 0xE000000] = value;
            saveDirty = true;
            mutex.unlock();
            flashCmd = 0xF0;
        }
        else if (flashErase && (address & ~0x000F000) == 0xE000000 && (value & 0xFF) == 0x30) {
            // Erase a sector
            if (bankSwap) address += 0x10000;
            mutex.lock();
            memset(&save[address - 0xE000000], 0xFF, 0x1000 * sizeof(uint8_t));
            saveDirty = true;
            mutex.unlock();
            flashErase = false;
        }
        else if (saveSize == 0x20000 && flashCmd == 0xB0 && address == 0xE000000) {
            // Swap the ROM banks on 128KB carts
            bankSwap = value;
            flashCmd = 0xF0;
        }
        else if (address == 0xE005555) {
            // Write the FLASH command byte
            flashCmd = value;

            // Handle erase commands
            if (flashCmd == 0x80) {
                flashErase = true;
            }
            else if (flashCmd != 0xAA) {
                flashErase = false;
            }
            else if (flashErase && flashCmd == 0x10) {
                mutex.lock();
                memset(save, 0xFF, saveSize * sizeof(uint8_t));
                saveDirty = true;
                mutex.unlock();
            }
        }
    }
}
