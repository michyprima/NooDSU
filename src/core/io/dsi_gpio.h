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

#include <atomic>
#include <cstdint>
#include <cstdio>

class Core;

// The DSi's GPIO pins, where the ARM7 sees whether headphones are plugged in; they follow the GamePad's headphone jack
class DsiGpio {
public:
    static std::atomic<bool> headphonesIn; // Set by the frontend at any time, taken by the core once a frame

    DsiGpio(Core *core): core(core), headphones(headphonesIn.load()) {}
    void saveState(FILE *file);
    void loadState(FILE *file);

    void update();
    bool readHeadphones() { return headphones; }

    uint8_t readData();
    uint8_t readDir() { return gpioDir; }
    uint8_t readEdge() { return gpioEdge; }
    uint8_t readIe() { return gpioIe; }
    uint16_t readWifi() { return gpioWifi; }

    void writeData(uint8_t value) { gpioOut = value & 0xF7; }
    void writeDir(uint8_t value) { gpioDir = value & 0xF7; }
    void writeEdge(uint8_t value) { gpioEdge = value & 0xF7; }
    void writeIe(uint8_t value) { gpioIe = value & 0xF7; }
    void writeWifi(uint16_t mask, uint16_t value);

private:
    Core *core;
    bool headphones;

    uint8_t gpioOut = 0;
    uint8_t gpioDir = 0;
    uint8_t gpioEdge = 0;
    uint8_t gpioIe = 0;
    uint16_t gpioWifi = 0;
};
