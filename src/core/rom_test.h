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

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

class Memory;

// Registers only the romtest build has, which tests/rom's ROM uses to get real time and save its results
class RomTest {
public:
    inline static std::string path; // Where results go, appended to
    inline static uint32_t progress = 0; // The last thing the ROM said it was starting
    inline static bool done = false;
    inline static uint32_t detail = 0; // 1 for each set of values to be reported too, 2 for what the ROM saves on a real DS

    static uint32_t read(uint32_t address) {
        switch (address & 0xFFF) {
            case 0x000: return 0x54534F48; // Magic number
            case 0x018: return detail;
            case 0x004: return std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            default: return 0;
        }
    }

    static void write(Memory *memory, uint32_t address, uint32_t value);

private:
    inline static uint32_t text = 0;
};
