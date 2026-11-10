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
#include <cstdio>
#include "../atomic.h"
#include "../defines.h"

class Core;

// The accessories that can be put in the GBA slot when no GBA ROM is loaded there, in the order of the setting
enum Slot2Device {
    SLOT2_NONE,
    SLOT2_RUMBLE, // Rumble Pak
    SLOT2_EXPANSION, // Memory Expansion Pak
    SLOT2_GUITAR, // Guitar Grip
    SLOT2_PIANO, // Easy Piano keyboard
    SLOT2_PADDLE, // Taito paddle controller
    SLOT2_SOLAR, // A Boktai cart, for its solar sensor
    SLOT2_MAX
};

class Slot2 {
public:
    Slot2(Core *core): core(core) {}
    ~Slot2() { delete[] ram; }
    void saveState(FILE *file);
    void loadState(FILE *file);

    void init();
    int getDevice() { return device; }

    // Input from the frontend, which runs on another thread
    void setGuitarKeys(uint8_t keys) { guitarKeys.store(keys); } // Bits 0-3: green, red, yellow, blue
    void setPianoKeys(uint16_t keys) { pianoKeys.store(keys); } // Bits 0-12: C to high C
    void turnPaddle(int amount) { paddleTurn.store(paddleTurn.load() + amount); }
    bool takeRumble();

    // The GBA slot's memory as the CPU with access to it sees it; the maps point straight at ROM and expansion RAM
    uint8_t *getRom(uint32_t address);
    uint8_t *getRam(uint32_t address);
    template <typename T> T romRead(bool arm7, uint32_t address);
    template <typename T> void romWrite(bool arm7, uint32_t address, T value);
    template <typename T> T sramRead(bool arm7, uint32_t address);
    template <typename T> void sramWrite(bool arm7, uint32_t address, T value);

    // The GPIO pins of GBA carts with a solar sensor or a rumble motor, also used in GBA mode
    bool hasSolar() { return solar; }
    void writeGpioPins(uint8_t pins, uint8_t direction);
    uint8_t readGpioPins(uint8_t direction);

private:
    Core *core;
    int device = SLOT2_NONE;
    bool solar = false; // A Boktai cart is in the slot, real or made up
    bool gbaRumble = false; // A GBA cart with a rumble motor on GPIO pin 3 is in the slot

    // A made-up Boktai ROM header, for the solar sensor without a Boktai ROM; it's zero up to the GPIO registers
    uint8_t fakeRom[0x100] = {};

    // The GPIO registers at 0x80000C4, for GBA carts in DS mode; GBA mode shares the RTC's
    uint8_t gpioData = 0, gpioDirection = 0, gpioControl = 0;

    // The solar sensor's counter, which the game clocks until it passes the light level
    uint8_t solarCounter = 0, solarSample = 0xFF;
    bool solarEdge = false;

    // The Rumble Pak flips its actuator on each change, and GBA carts drive a motor while their pin is high
    uint16_t rumbleState = 0;
    Atomic32<uint32_t> rumbleFlips;
    Atomic32<bool> motorOn;

    // The Memory Expansion Pak's 8MB of RAM, at 0x9000000 while unlocked
    uint8_t *ram = nullptr;
    bool ramUnlocked = true;

    Atomic32<uint8_t> guitarKeys;
    Atomic32<uint16_t> pianoKeys;
    Atomic32<int> paddleTurn;
    uint16_t paddle = 0;

    bool owns(bool arm7);
    uint16_t openBus(bool arm7, uint32_t address);
    uint16_t read16(bool arm7, uint32_t address);
    void write16(uint32_t address, uint16_t value);
    uint8_t read8Sram(uint32_t address);
};
