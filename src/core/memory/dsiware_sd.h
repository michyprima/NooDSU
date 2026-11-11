/*
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
#include <string>
#include <unordered_map>
#include <vector>

class Core;

// An SD card made up for a DSiWare title booted directly, holding the title and its saves in a FAT16 volume
// DSiWare finds its own files and saves through paths the DSi menu leaves it, normally on the NAND; pointing them at this
// card instead (as unlaunch does for titles on a real SD card) needs no NAND install, and the saves stay host files
class DsiWareSd {
public:
    DsiWareSd(Core *core): core(core) {}
    ~DsiWareSd();

    bool active() { return !files.empty(); }
    uint64_t size() { return uint64_t(totalSectors) << 9; }

    void create(const uint8_t *header, std::string savePath);
    void read(uint64_t offset, uint8_t *data, uint32_t size);
    void write(uint64_t offset, const uint8_t *data, uint32_t size);
    void flush();

    // The 8.3 names the title's files get in the card's root folder, for the device list
    std::string fileName(int i) { return files[i].shortName; }
    int fileCount() { return files.size(); }

private:
    Core *core;

    struct File {
        std::string shortName, path; // Path is for saves; the title's own file is the ROM
        uint32_t size = 0, firstCluster = 0, clusters = 0;
        std::vector<uint8_t> data;
        bool dirty = false;
    };

    std::vector<File> files;
    std::vector<uint8_t> meta; // Every sector before the first cluster: the MBR, boot sector, FATs and root folder
    std::unordered_map<uint32_t, std::vector<uint8_t>> written; // Other sectors written, kept until the core stops
    uint32_t totalSectors = 0, partStart = 0, dataStart = 0, sectorsPerCluster = 0;

    void readSector(uint32_t sector, uint8_t *data);
    void writeSector(uint32_t sector, const uint8_t *data);
    File *fileAt(uint32_t sector, uint32_t &offset);
    static std::vector<uint8_t> newSave(uint32_t size);
};
