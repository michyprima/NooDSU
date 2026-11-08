/*
    Copyright 2019-2026 Hydr8gon

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

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>

#include "file_lock.h"

// Reads a file in large chunks and keeps the most recent ones, for callers that read a small block at a time
// On Wii U every file access takes a few milliseconds whatever its size, so reading sector by sector is very slow
// Writes go straight to the file, and also update any chunk holding that data
class FileCache {
public:
    ~FileCache() {
        delete[] alloc;
    }

    void read(FILE *file, uint64_t offset, uint8_t *data, uint32_t size) {
        // Set up the chunks the first time they're needed, reading directly if there's no memory for them
        if (!alloc && !allocate()) {
            std::lock_guard<FileLock> guard(fileLock());
            readFile(file, offset, data, size);
            return;
        }

        // Copy from each chunk the read touches, loading any that are missing
        while (size) {
            uint32_t start = offset % CHUNK_SIZE;
            uint32_t count = std::min(size, CHUNK_SIZE - start);
            memcpy(data, chunk(file, offset / CHUNK_SIZE) + start, count);
            offset += count;
            data += count;
            size -= count;
        }
    }

    void write(FILE *file, uint64_t offset, const uint8_t *data, uint32_t size) {
        // Write to the file
        {
            std::lock_guard<FileLock> guard(fileLock());
            fseeko(file, offset, SEEK_SET);
            fwrite(data, sizeof(uint8_t), size, file);
        }

        // Update the part of each chunk the write touches, if it's held
        while (size && alloc) {
            uint32_t start = offset % CHUNK_SIZE;
            uint32_t count = std::min(size, CHUNK_SIZE - start);
            int held = find(offset / CHUNK_SIZE);
            if (held != -1) memcpy(&buffer[held * CHUNK_SIZE + start], data, count);
            offset += count;
            data += count;
            size -= count;
        }
    }

private:
    static const uint32_t CHUNK_SIZE = 0x10000;
    static const int CHUNK_COUNT = 64;
    static const uint32_t NO_CHUNK = 0xFFFFFFFF;

    uint8_t *alloc = nullptr, *buffer = nullptr;
    uint32_t indices[CHUNK_COUNT];
    uint32_t lastUse[CHUNK_COUNT];
    uint32_t uses = 0;
    int slot = 0; // The chunk last found or loaded

    bool allocate() {
        // Allocate the chunks, aligned to 64 bytes, which some systems need to read files straight into them
        if (!(alloc = new (std::nothrow) uint8_t[CHUNK_SIZE * CHUNK_COUNT + 0x3F])) return false;
        buffer = (uint8_t*)(((uintptr_t)alloc + 0x3F) & ~uintptr_t(0x3F));
        for (int i = 0; i < CHUNK_COUNT; i++) {
            indices[i] = NO_CHUNK;
            lastUse[i] = 0;
        }
        return true;
    }

    int find(uint32_t index) {
        // Find the slot holding a chunk, checking the last one used first since reads are mostly in order
        if (indices[slot] == index) return slot;
        for (int i = 0; i < CHUNK_COUNT; i++)
            if (indices[i] == index) return i;
        return -1;
    }

    uint8_t *chunk(FILE *file, uint32_t index) {
        // Use a chunk if it's held, or load it into the least recently used slot
        int held = find(index);
        if (held == -1) {
            held = 0;
            for (int i = 1; i < CHUNK_COUNT; i++)
                if (lastUse[i] < lastUse[held]) held = i;
            std::lock_guard<FileLock> guard(fileLock());
            indices[held] = index;
            readFile(file, uint64_t(index) * CHUNK_SIZE, &buffer[held * CHUNK_SIZE], CHUNK_SIZE);
        }
        lastUse[held] = ++uses;
        slot = held;
        return &buffer[held * CHUNK_SIZE];
    }

    static void readFile(FILE *file, uint64_t offset, uint8_t *data, uint32_t size) {
        // Read data from the file, with zeros past the end
        fseeko(file, offset, SEEK_SET);
        size_t count = fread(data, sizeof(uint8_t), size, file);
        memset(&data[count], 0, size - count);
    }
};
