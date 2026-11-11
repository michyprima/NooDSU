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

#include <cstring>
#include "../core.h"
#include "../file_lock.h"

// Little-endian fields, whatever the host's byte order
static void put16(uint8_t *data, uint16_t value) {
    data[0] = value;
    data[1] = value >> 8;
}

static void put32(uint8_t *data, uint32_t value) {
    put16(data, value);
    put16(data + 2, value >> 16);
}

DsiWareSd::~DsiWareSd() {
    flush();
}

std::vector<uint8_t> DsiWareSd::newSave(uint32_t size) {
    // Format a new save as the DSi does when a title is installed: a FAT12 volume filling it, with no files
    // The geometry fits as many sectors as possible as tracks times heads squared, after NTM's sav.c (via melonDS)
    std::vector<uint8_t> data(size, 0);
    if (size < 0x200) return data;
    uint32_t maxSectors = std::min(size >> 9, 0xFFFFU), tracks = 1, heads = 1, sectors = 0, next = 0;
    while (next <= maxSectors) {
        next = tracks * (heads + 1) * (heads + 1);
        if (next <= maxSectors) {
            heads++;
            sectors = next;
            tracks++;
            next = tracks * heads * heads;
            if (next <= maxSectors) sectors = next;
        }
    }
    if ((tracks + 1) * heads * heads <= maxSectors) {
        tracks++;
        sectors = tracks * heads * heads;
    }
    uint32_t cluster = (sectors > (8 << 10)) ? 8 : (sectors > (1 << 10)) ? 4 : 1;
    uint32_t clusters = (sectors + cluster - 1) / cluster;
    uint32_t fatSectors = (((clusters + 1) / 2) * 3 + 0x1FF) >> 9;

    data[0x00] = 0xE9;
    memcpy(&data[0x03], "MSWIN4.1", 8);
    put16(&data[0x0B], 0x200); // Bytes per sector
    data[0x0D] = cluster;
    put16(&data[0x0E], 1); // Reserved sectors
    data[0x10] = 2; // FATs
    put16(&data[0x11], (size < 0x8C000) ? 0x20 : 0x200); // Root folder entries
    put16(&data[0x13], sectors);
    data[0x15] = 0xF8; // Media
    put16(&data[0x16], fatSectors);
    put16(&data[0x18], tracks);
    put16(&data[0x1A], heads);
    data[0x24] = 0x05; // Drive number
    data[0x26] = 0x29; // Extended boot signature
    put32(&data[0x27], 0x12345678); // Serial number
    memcpy(&data[0x2B], "VOLUMELABEL", 11);
    memcpy(&data[0x36], "FAT12   ", 8);
    put16(&data[0x1FE], 0xAA55);
    return data;
}

void DsiWareSd::create(const uint8_t *header, std::string savePath) {
    // Name the files after the game code, with the title itself first and then its public and private saves, made new
    // if they're missing; a save of the wrong size keeps what fits, like a title moved between sizes would
    std::string code(reinterpret_cast<const char*>(&header[0xC]), 4);
    for (char &c : code)
        if (!isalnum((unsigned char)c)) c = '_';
    std::string base = savePath.substr(0, savePath.rfind('.'));
    files.clear();
    files.push_back({ code + ".APP", "", uint32_t(core->cartridgeNds.getRomSize()) });
    const char *exts[] = { "PUB", "PRV" };
    for (int i = 0; i < 2; i++) {
        uint32_t size = U8TO32(header, 0x238 + i * 4);
        if (!size) continue;
        File file = { code + "." + exts[i], base + (i ? ".prv" : ".pub"), size };
        file.data = newSave(size);
        std::lock_guard<FileLock> guard(fileLock());
        if (FILE *in = fopen(file.path.c_str(), "rb")) {
            fread(file.data.data(), 1, size, in);
            fclose(in);
        }
        else {
            file.dirty = true;
        }
        files.push_back(file);
    }

    // Size a FAT16 volume to hold the files with room to spare, which needs between 4085 and 65524 clusters
    sectorsPerCluster = 8;
    uint32_t clusters;
    while (true) {
        clusters = 1024;
        for (File &file : files)
            clusters += (file.size + sectorsPerCluster * 0x200 - 1) / (sectorsPerCluster * 0x200);
        clusters = std::max(clusters, 4200U);
        if (clusters <= 65000) break;
        sectorsPerCluster <<= 1;
    }
    uint32_t fatSectors = ((clusters + 2) * 2 + 0x1FF) >> 9;
    partStart = 0x80;
    dataStart = partStart + 1 + fatSectors * 2 + 32;
    uint32_t volume = dataStart - partStart + clusters * sectorsPerCluster;
    totalSectors = (partStart + volume + 0x1FF) & ~0x1FF; // Whole 256KB units, as the CSD gives the size

    // Make the MBR, with one partition for the volume
    meta.assign(dataStart << 9, 0);
    uint8_t *mbr = &meta[0];
    uint8_t *entry = &mbr[0x1BE];
    entry[0x4] = 0x06; // FAT16 over 32MB
    put32(&entry[0x8], partStart);
    put32(&entry[0xC], volume);
    put16(&mbr[0x1FE], 0xAA55);

    // Make the volume's boot sector
    uint8_t *boot = &meta[partStart << 9];
    boot[0x00] = 0xEB;
    boot[0x01] = 0x3C;
    boot[0x02] = 0x90;
    memcpy(&boot[0x03], "NOODS U ", 8);
    put16(&boot[0x0B], 0x200); // Bytes per sector
    boot[0x0D] = sectorsPerCluster;
    put16(&boot[0x0E], 1); // Reserved sectors
    boot[0x10] = 2; // FATs
    put16(&boot[0x11], 512); // Root folder entries
    if (volume < 0x10000) put16(&boot[0x13], volume); else put32(&boot[0x20], volume);
    boot[0x15] = 0xF8; // Media
    put16(&boot[0x16], fatSectors);
    put16(&boot[0x18], 63); // Sectors per track
    put16(&boot[0x1A], 255); // Heads
    put32(&boot[0x1C], partStart); // Hidden sectors
    boot[0x24] = 0x80; // Drive number
    boot[0x26] = 0x29; // Extended boot signature
    put32(&boot[0x27], U8TO32(header, 0xC)); // Serial number
    memcpy(&boot[0x2B], "NOODS U    ", 11);
    memcpy(&boot[0x36], "FAT16   ", 8);
    put16(&boot[0x1FE], 0xAA55);

    // Lay the files out one after another, each in a single chain of clusters, in both FATs
    uint8_t *fat = &meta[(partStart + 1) << 9];
    put16(&fat[0], 0xFFF8);
    put16(&fat[2], 0xFFFF);
    uint32_t next = 2;
    for (File &file : files) {
        file.clusters = (file.size + sectorsPerCluster * 0x200 - 1) / (sectorsPerCluster * 0x200);
        file.firstCluster = file.clusters ? next : 0;
        for (uint32_t i = 0; i < file.clusters; i++, next++)
            put16(&fat[next * 2], (i + 1 < file.clusters) ? (next + 1) : 0xFFFF);
    }
    memcpy(&fat[fatSectors << 9], fat, fatSectors << 9);

    // List the volume label and the files in the root folder, dated when the DSi came out
    uint8_t *root = &meta[(partStart + 1 + fatSectors * 2) << 9];
    memcpy(&root[0x00], "NOODS U    ", 11);
    root[0x0B] = 0x08; // Volume label
    for (size_t i = 0; i < files.size(); i++) {
        uint8_t *dir = &root[(i + 1) * 0x20];
        std::string name = files[i].shortName;
        size_t dot = name.find('.');
        memset(dir, ' ', 11);
        memcpy(dir, name.c_str(), dot);
        memcpy(&dir[8], name.c_str() + dot + 1, 3);
        dir[0x0B] = 0x20; // Archive
        put16(&dir[0x0E], 0); // Created time
        put16(&dir[0x10], ((2008 - 1980) << 9) | (11 << 5) | 1); // Created date
        put16(&dir[0x12], ((2008 - 1980) << 9) | (11 << 5) | 1); // Accessed date
        put16(&dir[0x16], 0); // Modified time
        put16(&dir[0x18], ((2008 - 1980) << 9) | (11 << 5) | 1); // Modified date
        put16(&dir[0x1A], files[i].firstCluster);
        put32(&dir[0x1C], files[i].size);
    }
    LOG_INFO("DSiWare SD card made with %d sectors, files from sector 0x%X\n", totalSectors, dataStart);
}

DsiWareSd::File *DsiWareSd::fileAt(uint32_t sector, uint32_t &offset) {
    // Find the file holding a data sector, and where in it the sector is
    uint32_t cluster = (sector - dataStart) / sectorsPerCluster + 2;
    for (File &file : files) {
        if (cluster < file.firstCluster || cluster >= file.firstCluster + file.clusters) continue;
        offset = (sector - dataStart - (file.firstCluster - 2) * sectorsPerCluster) << 9;
        return &file;
    }
    return nullptr;
}

void DsiWareSd::readSector(uint32_t sector, uint8_t *data) {
    // Read from the volume's structures, then the files, then anything else written, or zeros
    if (sector < dataStart) {
        memcpy(data, &meta[sector << 9], 0x200);
        return;
    }
    memset(data, 0, 0x200);
    uint32_t offset;
    if (File *file = fileAt(sector, offset)) {
        uint32_t count = std::min(0x200U, file->size - std::min(file->size, offset));
        if (file == &files[0])
            core->cartridgeNds.readRom(offset, data, count);
        else
            memcpy(data, &file->data[offset], count);
        return;
    }
    auto it = written.find(sector);
    if (it != written.end()) memcpy(data, it->second.data(), 0x200);
}

void DsiWareSd::writeSector(uint32_t sector, const uint8_t *data) {
    // Write to the volume's structures, a save, or memory for anything else; the title's own file can't change
    if (sector < dataStart) {
        memcpy(&meta[sector << 9], data, 0x200);
        return;
    }
    uint32_t offset;
    if (File *file = fileAt(sector, offset)) {
        if (file == &files[0]) return;
        uint32_t count = std::min(0x200U, file->size - std::min(file->size, offset));
        if (memcmp(&file->data[offset], data, count)) {
            memcpy(&file->data[offset], data, count);
            file->dirty = true;
        }
        return;
    }
    written[sector].assign(data, data + 0x200);
}

void DsiWareSd::read(uint64_t offset, uint8_t *data, uint32_t size) {
    // Read any range, a sector at a time
    uint8_t sector[0x200];
    while (size) {
        uint32_t start = offset & 0x1FF, count = std::min(size, 0x200 - start);
        if (offset >> 9 < totalSectors) readSector(offset >> 9, sector); else memset(sector, 0, 0x200);
        memcpy(data, &sector[start], count);
        offset += count;
        data += count;
        size -= count;
    }
}

void DsiWareSd::write(uint64_t offset, const uint8_t *data, uint32_t size) {
    // Write any range, a sector at a time, keeping the rest of a sector written in part
    uint8_t sector[0x200];
    while (size) {
        uint32_t start = offset & 0x1FF, count = std::min(size, 0x200 - start);
        if (offset >> 9 >= totalSectors) return;
        if (count < 0x200) readSector(offset >> 9, sector);
        memcpy(&sector[start], data, count);
        writeSector(offset >> 9, sector);
        offset += count;
        data += count;
        size -= count;
    }
}

void DsiWareSd::flush() {
    // Write the saves that changed back to their files, whole, since they're small
    std::lock_guard<FileLock> guard(fileLock());
    for (File &file : files) {
        if (!file.dirty) continue;
        if (FILE *out = fopen(file.path.c_str(), "wb")) {
            fwrite(file.data.data(), 1, file.size, out);
            fclose(out);
            file.dirty = false;
        }
    }
}
