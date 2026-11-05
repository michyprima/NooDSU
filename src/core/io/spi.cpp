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

#include <algorithm>
#include <cstring>
#include "../core.h"
#include "../file_lock.h"
#include "wifi_ap.h"

Language Spi::language = LG_ENGLISH;
std::u16string Spi::nickname;

Spi::~Spi() {
    // Save any access point settings that are waiting, and free any dynamic memory
    writeWfcSettings();
    if (firmware) delete[] firmware;
}

void Spi::saveState(FILE *file) {
    // Write state data to the file
    fwrite(&writeCount, sizeof(writeCount), 1, file);
    fwrite(&address, sizeof(address), 1, file);
    fwrite(&command, sizeof(command), 1, file);
    fwrite(&spiCnt, sizeof(spiCnt), 1, file);
    fwrite(&spiData, sizeof(spiData), 1, file);
}

void Spi::loadState(FILE *file) {
    // Read state data from the file
    fread(&writeCount, sizeof(writeCount), 1, file);
    fread(&address, sizeof(address), 1, file);
    fread(&command, sizeof(command), 1, file);
    fread(&spiCnt, sizeof(spiCnt), 1, file);
    fread(&spiData, sizeof(spiData), 1, file);
}

uint16_t Spi::crc16(uint32_t value, uint8_t *data, size_t size) {
    static const uint16_t table[] = { 0xC0C1, 0xC181, 0xC301, 0xC601, 0xCC01, 0xD801, 0xF001, 0xA001 };

    // Calculate a CRC16 value for the given data
    for (size_t i = 0; i < size; i++) {
        value ^= data[i];
        for (size_t j = 0; j < 8; j++)
            value = (value >> 1) ^ ((value & 1) ? (table[j] << (7 - j)) : 0);
    }

    return value;
}

void Spi::writeNickname(uint8_t *settings) {
    // Write the nickname specified by the frontend to a copy of the user settings, if there is one
    if (nickname.empty()) return;
    size_t length = std::min<size_t>(nickname.size(), 10);
    for (size_t i = 0; i < 10; i++) {
        uint16_t value = (i < length) ? nickname[i] : 0;
        settings[0x06 + i * 2] = value >> 0;
        settings[0x07 + i * 2] = value >> 8;
    }
    settings[0x1A] = length;
    settings[0x1B] = 0;
}

bool Spi::loadFirmware() {
    // Ensure firmware memory isn't already allocated
    if (firmware)
        delete[] firmware;

    // Load the firmware from a file if it exists
    std::string &path = core->dsiMode ? Settings::dsiFirmPath : Settings::ndsFirmPath;
    if (FILE *file = fopen(path.c_str(), "rb")) {
        fseek(file, 0, SEEK_END);
        firmSize = ftell(file);
        fseek(file, 0, SEEK_SET);
        firmware = new uint8_t[firmSize];
        fread(firmware, sizeof(uint8_t), firmSize, file);
        fclose(file);

        if (core->id > 0) {
            // Increment the MAC address based on the instance ID
            // This allows instances to be detected as separate systems
            firmware[0x3B] += core->id;

            // Recalculate the WiFi config CRC
            uint16_t crc = crc16(0, &firmware[0x2C], 0x138);
            firmware[0x2A] = crc >> 0;
            firmware[0x2B] = crc >> 8;
        }

        // Set the language and nickname specified by the frontend in both copies of the user settings
        // A copy that already fails its CRC is left alone, so that it doesn't start looking valid
        for (uint32_t addr = firmSize - 0x200; firmSize >= 0x200 && addr < firmSize; addr += 0x100) {
            if (crc16(0xFFFF, &firmware[addr], 0x70) != U8TO16(firmware, addr + 0x72))
                continue;
            firmware[addr + 0x64] = (firmware[addr + 0x64] & ~0x7) | language;
            writeNickname(&firmware[addr]);
            uint16_t crc = crc16(0xFFFF, &firmware[addr], 0x70);
            firmware[addr + 0x72] = crc >> 0;
            firmware[addr + 0x73] = crc >> 8;
        }

        // Apply saved Wi-Fi settings and the emulated access point
        loadWfcSettings();
        if (Settings::wifi) addAccessPoint();

        // Check if the firmware is from the correct system based on its size
        return core->dsiMode ? (firmSize <= 0x20000) : (firmSize > 0x20000);
    }

    // Create a basic, non-bootable firmware if one isn't provided
    firmSize = 0x20000;
    firmware = new uint8_t[firmSize];
    memset(firmware, 0, firmSize);

    // Set some firmware header data
    firmware[0x20] = 0xC0; // User settings offset / 8, byte 1
    firmware[0x21] = 0x3F; // User settings offset / 8, byte 2

    // Set some WiFi config data
    firmware[0x2C] = 0x38; // Config length, byte 1
    firmware[0x2D] = 0x01; // Config length, byte 2
    firmware[0x36] = 0x00; // MAC address, byte 1
    firmware[0x37] = 0x09; // MAC address, byte 2
    firmware[0x38] = 0xBF; // MAC address, byte 3
    firmware[0x39] = 0x12; // MAC address, byte 4
    firmware[0x3A] = 0x34; // MAC address, byte 5
    firmware[0x3B] = core->id; // MAC address, byte 6
    firmware[0x3C] = 0xFE; // Enabled channels, byte 1
    firmware[0x3D] = 0x3F; // Enabled channels, byte 2

    // Calculate the WiFi config CRC
    uint16_t crc = crc16(0, &firmware[0x2C], 0x138);
    firmware[0x2A] = crc >> 0;
    firmware[0x2B] = crc >> 8;

    // Configure the WiFi access points
    for (uint32_t addr = 0x1FA00; addr <= 0x1FC00; addr += 0x100) {
        // Set some access point data
        firmware[addr + 0xE7] = 0xFF; // Not configured
        firmware[addr + 0xF5] = 0x28; // Unknown

        // Calculate the access point CRC
        crc = crc16(0, &firmware[addr], 0xFE);
        firmware[addr + 0xFE] = crc >> 0;
        firmware[addr + 0xFF] = crc >> 8;
    }

    // Configure the user settings
    for (uint32_t addr = 0x1FE00; addr <= 0x1FF00; addr += 0x100) {
        // Set some user settings data
        firmware[addr + 0x00] = 5; // Version
        firmware[addr + 0x02] = 2; // Favorite color
        firmware[addr + 0x03] = 5; // Birthday month
        firmware[addr + 0x04] = 25; // Birthday day
        firmware[addr + 0x06] = 'N'; // Nickname, char 1
        firmware[addr + 0x08] = 'o'; // Nickname, char 2
        firmware[addr + 0x0A] = 'o'; // Nickname, char 3
        firmware[addr + 0x0C] = 'D'; // Nickname, char 4
        firmware[addr + 0x0E] = 'S'; // Nickname, char 5
        firmware[addr + 0x1A] = 5; // Nickname length

        // Set the touch calibration data
        firmware[addr + 0x5E] = 0xF0; // ADC X2, byte 1
        firmware[addr + 0x5F] = 0x0F; // ADC X2, byte 2
        firmware[addr + 0x60] = 0xF0; // ADC Y2, byte 1
        firmware[addr + 0x61] = 0x0B; // ADC Y2, byte 2
        firmware[addr + 0x62] = 0xFF; // SCR X2
        firmware[addr + 0x63] = 0xBF; // SCR Y2

        // Set the language and nickname specified by the frontend
        firmware[addr + 0x64] = language;
        writeNickname(&firmware[addr]);

        // Calculate the user settings CRC
        crc = crc16(0xFFFF, &firmware[addr], 0x70);
        firmware[addr + 0x72] = crc >> 0;
        firmware[addr + 0x73] = crc >> 8;
    }

    // Apply saved Wi-Fi settings and the emulated access point
    loadWfcSettings();
    if (Settings::wifi) addAccessPoint();
    return false;
}

size_t Spi::apOffset() {
    // Get where the 3 access point settings are, right before the user settings, or 0 if that's out of range
    size_t offset = (firmware[0x20] | (firmware[0x21] << 8)) * 8;
    return (offset >= 0x400 && offset + 0x200 <= firmSize) ? offset - 0x400 : 0;
}

void Spi::loadWfcSettings() {
    // Load the access point settings games saved, which are kept apart so a firmware dump is never changed
    size_t offset = apOffset();
    if (!offset) return;
    std::lock_guard<FileLock> guard(fileLock());
    if (FILE *file = fopen((Settings::basePath + "/bios/wfcsettings.bin").c_str(), "rb")) {
        uint8_t data[0x300];
        if (fread(data, 1, sizeof(data), file) == sizeof(data))
            memcpy(&firmware[offset], data, sizeof(data));
        fclose(file);
    }
}

void Spi::writeWfcSettings() {
    // Save the access point settings games changed, which hold the Nintendo WFC user ID they create
    // This runs on the thread that writes save files, so file access stays off the emulator's core
    // The settings are copied under the mutex and written after, so the emulator never waits on the file
    uint8_t data[sizeof(wfcCopy)];
    mutex.lock();
    bool dirty = wfcDirty;
    if (dirty) memcpy(data, wfcCopy, sizeof(data));
    wfcDirty = false;
    mutex.unlock();
    if (!dirty) return;

    std::lock_guard<FileLock> guard(fileLock());
    if (FILE *file = fopen((Settings::basePath + "/bios/wfcsettings.bin").c_str(), "wb")) {
        fwrite(data, 1, sizeof(data), file);
        fclose(file);
    }
    else {
        // Try again next time if the file couldn't be opened, unless newer settings came in meanwhile
        mutex.lock();
        if (!wfcDirty) {
            memcpy(wfcCopy, data, sizeof(data));
            wfcDirty = true;
        }
        mutex.unlock();
    }
}

void Spi::addAccessPoint() {
    // Set up an access point slot for the emulated access point, reusing one that's already set up for it, or
    // else taking the first unused one, or the last one if they're all used
    size_t offset = apOffset();
    if (!offset) return;
    int slot = -1;
    for (int i = 0; i < 3 && slot < 0; i++)
        if (!memcmp(&firmware[offset + i * 0x100 + 0x40], WifiAp::ssid, sizeof(WifiAp::ssid)))
            slot = i;
    for (int i = 0; i < 3 && slot < 0; i++)
        if (firmware[offset + i * 0x100 + 0xE7] == 0xFF)
            slot = i;
    if (slot < 0) slot = 2;

    // Keep everything in a slot already set up for it, like the WFC user ID, otherwise start it from scratch
    uint8_t *ap = &firmware[offset + slot * 0x100];
    if (memcmp(&ap[0x40], WifiAp::ssid, sizeof(WifiAp::ssid))) {
        memset(ap, 0, 0x100);
        memcpy(&ap[0x40], WifiAp::ssid, sizeof(WifiAp::ssid));
        ap[0xEF] = 0x01; // Configured
    }
    // Get the IP address automatically, and use the DNS server from the settings, or else get it automatically too
    memset(&ap[0xC0], 0, 0x11);
    unsigned int dns[4];
    if (sscanf(Settings::wfcDns.c_str(), "%u.%u.%u.%u", &dns[0], &dns[1], &dns[2], &dns[3]) == 4 &&
            dns[0] < 256 && dns[1] < 256 && dns[2] < 256 && dns[3] < 256)
        for (int i = 0; i < 4; i++)
            ap[0xC8 + i] = dns[i];
    ap[0xE6] = 0; // No WEP
    ap[0xE7] = 0; // Normal connection
    uint16_t crc = crc16(0, ap, 0xFE);
    ap[0xFE] = crc >> 0;
    ap[0xFF] = crc >> 8;
}

void Spi::directBoot() {
    // Load the user settings into memory
    for (uint32_t i = 0; i < 0x70; i++)
        core->memory.write<uint8_t>(0, 0x2FFFC80 + i, firmware[firmSize - 0x100 + i]);
}

void Spi::setTouch(int x, int y) {
    if (!firmware)
        return;

    // Read calibration points from the firmware
    uint16_t adcX1 = U8TO16(firmware, firmSize - 0xA8);
    uint16_t adcY1 = U8TO16(firmware, firmSize - 0xA6);
    uint8_t scrX1 = firmware[firmSize - 0xA4];
    uint8_t scrY1 = firmware[firmSize - 0xA3];
    uint16_t adcX2 = U8TO16(firmware, firmSize - 0xA2);
    uint16_t adcY2 = U8TO16(firmware, firmSize - 0xA0);
    uint8_t scrX2 = firmware[firmSize - 0x9E];
    uint8_t scrY2 = firmware[firmSize - 0x9D];

    // Ensure the coordinates are within bounds
    // A one pixel border around the screen is ignored to avoid potential underflow/overflow
    // This should be fine; GBATEK says that pressing near the screen borders may be impossible anyways
    if (x < 1) x = 1; else if (x > 254) x = 254;
    if (y < 1) y = 1; else if (y > 190) y = 190;

    // Convert the coordinates to ADC values
    if (scrX2 - scrX1 != 0) touchX = (x - (scrX1 - 1)) * (adcX2 - adcX1) / (scrX2 - scrX1) + adcX1;
    if (scrY2 - scrY1 != 0) touchY = (y - (scrY1 - 1)) * (adcY2 - adcY1) / (scrY2 - scrY1) + adcY1;
}

void Spi::clearTouch() {
    // Set the ADC values to their default state
    touchX = 0x000;
    touchY = 0xFFF;
}

void Spi::sendMicData(const int16_t *samples, size_t count, size_t rate) {
    mutex.lock();

    // Add the samples to the microphone buffer, dropping the oldest ones if it overflows
    for (size_t i = 0; i < count; i++)
        micBuffer[micWrite++ & (MIC_SIZE - 1)] = samples[i];
    if (micWrite - micRead > MIC_SIZE)
        micRead = micWrite - MIC_SIZE;

    // Set the number of cycles per sample
    micRate = rate;
    micStep = (60 * 263 * 355 * 6) / rate;

    mutex.unlock();
}

void Spi::resetCycles() {
    // Adjust the microphone's sample time for the global reset
    micCycles -= core->globalCycles;
}

void Spi::writeSpiCnt(uint16_t mask, uint16_t value) {
    // Write to the SPICNT register
    mask &= 0xCF03;
    spiCnt = (spiCnt & ~mask) | (value & mask);
}

void Spi::writeSpiData(uint8_t value) {
    // Don't do anything if the SPI isn't enabled
    if (!(spiCnt & BIT(15))) {
        spiData = 0;
        return;
    }

    if (writeCount == 0) {
        // On the first write, set the command byte
        command = value;
        address = 0;
        spiData = 0;

        // Set or clear the firmware's write enable latch, which commands without parameters do right away
        if (((spiCnt & 0x0300) >> 8) == 1) {
            if (command == 0x06) firmStatus |= BIT(1);
            else if (command == 0x04) firmStatus &= ~BIT(1);
        }
    }
    else {
        switch ((spiCnt & 0x0300) >> 8) { // Device
        case 1: // Firmware
            switch (command) {
            case 0x03: // Read data bytes
                if (writeCount < 4) {
                    // On writes 2-4, set the 3 byte address to read from
                    address |= value << ((3 - writeCount) * 8);
                }
                else {
                    // On writes 5+, read data from the firmware and send it back
                    spiData = (address < firmSize) ? firmware[address] : 0;

                    // Increment the address
                    // 16-bit mode is bugged; the address is incremented accordingly, but only the lower 8 bits are sent
                    address += (spiCnt & BIT(10)) ? 2 : 1;
                }
                break;

            case 0x0B: // Fast read data bytes
                if (writeCount < 4) {
                    // On writes 2-4, set the 3 byte address to read from
                    address |= value << ((3 - writeCount) * 8);
                }
                else if (writeCount > 4) {
                    // After a dummy byte, read data from the firmware and send it back
                    spiData = (address < firmSize) ? firmware[address] : 0;
                    address++;
                }
                break;

            case 0x05: // Read status register
                spiData = firmStatus;
                break;

            case 0x9F: // Read identification
                spiData = (writeCount == 1) ? 0x20 : (writeCount == 2) ? 0x40 : (writeCount == 3) ? 0x12 : 0;
                break;

            case 0x0A: case 0x02: // Page write, page program
                if (writeCount < 4) {
                    // On writes 2-4, set the 3 byte address to write to
                    address |= value << ((3 - writeCount) * 8);
                }
                else if ((firmStatus & BIT(1)) && address < firmSize) {
                    // Write data to the firmware, where programming can only clear bits, wrapping within the page
                    firmware[address] = (command == 0x0A) ? value : (firmware[address] & value);
                    size_t offset = apOffset();
                    if (offset && address >= offset && address < offset + 0x300)
                        apWritten = true;
                    address = (address & ~0xFF) | ((address + 1) & 0xFF);
                }
                break;

            case 0xDB: case 0xD8: // Page erase, sector erase
                if (writeCount < 4)
                    address |= value << ((3 - writeCount) * 8);
                if (writeCount == 3 && (firmStatus & BIT(1))) {
                    // Fill the page or sector with ones once the address is set
                    size_t size = (command == 0xDB) ? 0x100 : 0x10000;
                    size_t start = address & ~(size - 1);
                    if (start < firmSize)
                        memset(&firmware[start], 0xFF, std::min(size, firmSize - start));
                    size_t offset = apOffset();
                    if (offset && start < offset + 0x300 && start + size > offset)
                        apWritten = true;
                }
                break;

            default:
                LOG_CRIT("Write to SPI with unknown firmware command: 0x%X\n", command);
                spiData = 0;
                break;
            }
            break;

        case 2: // Touchscreen
            switch ((command & 0x70) >> 4) { // Channel
            case 1: // Y-coordinate
                // Send the ADC Y coordinate MSB first, with 3 dummy bits in front
                spiData = (writeCount & 1) ? (touchY >> 5) : (touchY << 3);
                break;

            case 5: // X-coordinate
                // Send the ADC X coordinate MSB first, with 3 dummy bits in front
                spiData = (writeCount & 1) ? (touchX >> 5) : (touchX << 3);
                break;

            case 6: // AUX input
                if (writeCount & 1) {
                    // Load the sample for the current time, or noise while blowing is held
                    // The sample is converted to an unsigned 12-bit value
                    int16_t value = 0;
                    if (micBlow.load()) {
                        micNoise ^= micNoise << 13;
                        micNoise ^= micNoise >> 17;
                        micNoise ^= micNoise << 5;
                        value = micNoise;
                    }
                    else {
                        mutex.lock();
                        if (micStep) {
                            // Move past the samples that played since the last read
                            uint32_t played = (core->globalCycles - micCycles) / micStep;
                            micRead += played;
                            micCycles += played * micStep;

                            // Stay a few frames behind the newest samples, skipping ahead if reads fell behind, and
                            // wait for more if they ran out, like when reading stopped or the emulator ran fast
                            if (int32_t(micWrite - micRead) > int32_t(micRate / 10))
                                micRead = micWrite - micRate / 30;
                            if (int32_t(micWrite - micRead) <= 0)
                                micRead = micWrite, micCycles = core->globalCycles;
                            else
                                value = micBuffer[micRead & (MIC_SIZE - 1)];
                        }
                        mutex.unlock();
                    }
                    micSample = (value >> 4) + 0x800;

                    // Send the most significant 7 bits of the sample first
                    spiData = micSample >> 5;
                    break;
                }

                // Send the last 5 bits of the sample. with 3 dummy bits in front
                spiData = micSample << 3;
                break;

            default:
                LOG_WARN("Write to SPI with unknown touchscreen channel: %d\n", (command & 0x70) >> 4);
                spiData = 0;
                break;
            }
            break;

        default:
            LOG_CRIT("Write to SPI with unknown device: %d\n", (spiCnt & 0x0300) >> 8);
            spiData = 0;
            break;
        }
    }

    // Keep track of the write count
    if (spiCnt & BIT(11)) { // Keep chip selected
        writeCount++;
    }
    else { // Deselect chip
        // Finish writes to the firmware, clearing the write enable latch and saving any access point changes
        if (((spiCnt & 0x0300) >> 8) == 1 && writeCount > 0 &&
            (command == 0x0A || command == 0x02 || command == 0xDB || command == 0xD8)) {
            firmStatus &= ~BIT(1);
            if (apWritten) {
                // Copy the access point settings to be saved
                mutex.lock();
                memcpy(wfcCopy, &firmware[apOffset()], sizeof(wfcCopy));
                wfcDirty = true;
                mutex.unlock();
            }
            apWritten = false;
        }
        writeCount = 0;
    }

    // Trigger a transfer finished IRQ if enabled
    if (spiCnt & BIT(14))
        core->interpreter[1].sendInterrupt(23);
}
