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

#include <cstdint>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

enum Language {
    LG_JAPANESE = 0,
    LG_ENGLISH,
    LG_FRENCH,
    LG_GERMAN,
    LG_ITALIAN,
    LG_SPANISH
};

class Core;

class Spi {
public:
    uint16_t touchX = 0x000;
    uint16_t touchY = 0xFFF;

    Spi(Core *core): core(core) {}
    ~Spi();

    void saveState(FILE *file);
    void loadState(FILE *file);

    bool loadFirmware();
    void directBoot();
    const uint8_t *getFirmware(size_t &size) { size = firmSize; return firmware; }
    void writeWfcSettings();

    void setTouch(int x, int y);
    void clearTouch();

    static void setLanguage(Language lang) { language = lang; }
    static void setNickname(const std::u16string &name) { nickname = name; }
    void sendMicData(const int16_t *samples, size_t count, size_t rate);
    void setMicBlow(bool blow) { micBlow.store(blow); }
    int16_t readMic(uint32_t cycles);
    void resetCycles();

    uint16_t readSpiCnt() { return spiCnt; }
    uint8_t readSpiData() { return spiData; }

    void writeSpiCnt(uint16_t mask, uint16_t value);
    void writeSpiData(uint8_t value);

private:
    Core *core;

    static Language language;
    static std::u16string nickname; // Empty keeps the firmware's own
    uint8_t *firmware = nullptr;
    size_t firmSize = 0;
    uint8_t firmStatus = 0; // The flash status register, with the write enable latch
    bool apWritten = false; // Whether the access point settings were written since the chip was selected
    uint8_t wfcCopy[0x300] = {}; // Access point settings waiting to be saved, copied under the mutex
    bool wfcDirty = false;

    static const uint32_t MIC_SIZE = 0x4000; // Samples in the microphone ring buffer, a power of 2
    int16_t micBuffer[MIC_SIZE] = {};
    uint32_t micWrite = 0; // Total samples sent, which wraps around the buffer
    uint32_t micRead = 0; // Total samples played, kept behind micWrite
    uint32_t micRate = 0;
    uint32_t micCycles = 0; // When the sample at micRead started
    uint32_t micStep = 0; // Cycles per sample
    uint32_t micNoise = 1; // Noise generator state for blowing
    std::atomic<bool> micBlow{false};
    uint16_t micSample = 0;
    std::mutex mutex;

    uint32_t writeCount = 0;
    uint32_t address = 0;
    uint8_t command = 0;

    // The DSi's touchscreen controller starts in its own mode, with registers in pages, until DS mode is chosen
    bool tscDsiMode = false;
    uint8_t tscIndex = 0; // The register being accessed, times 2, plus 1 to read
    uint8_t tscPage = 0;
    uint8_t tscPage3[0x80] = {};
    bool tscPressed = false; // The pen state last reported, and whether X and Y are yet to report a change
    bool tscChangedX = false;
    bool tscChangedY = false;

    uint16_t spiCnt = 0;
    uint8_t spiData = 0;

    static uint16_t crc16(uint32_t value, uint8_t *data, size_t size);
    static void writeNickname(uint8_t *settings);
    size_t apOffset();
    void loadWfcSettings();
    void addAccessPoint();
    void writeTscDsi(uint8_t value);
};
