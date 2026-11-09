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
#include <cstdio>

class Core;

class Input {
public:
    Input(Core *core): core(core) {}
    void saveState(FILE *file);
    void loadState(FILE *file);

    void pressKey(int key);
    void releaseKey(int key);
    void pressScreen();
    void releaseScreen();
    void update();

    uint16_t readKeyInput() { return keyInput; }
    uint16_t readExtKeyIn() { return extKeyIn; }
    uint16_t readKeyCnt(int cpu) { return keyCnt[cpu]; }
    void writeKeyCnt(int cpu, uint16_t mask, uint16_t value);

private:
    Core *core;

    uint16_t keyInput = 0x03FF;
    uint16_t extKeyIn = 0x007F;
    uint16_t keyCnt[2] = {};
    uint16_t lastKeys = 0x03FF;
};
