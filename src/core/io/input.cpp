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

#include "../core.h"

void Input::saveState(FILE *file) {
    // Write state data to the file
    fwrite(keyCnt, 2, sizeof(keyCnt) / 2, file);
    fwrite(&lastKeys, sizeof(lastKeys), 1, file);
}

void Input::loadState(FILE *file) {
    // Read state data from the file
    fread(keyCnt, 2, sizeof(keyCnt) / 2, file);
    fread(&lastKeys, sizeof(lastKeys), 1, file);
}

void Input::update() {
    // Send keypad interrupts for key changes, once per frame since the frontend changes keys from another thread
    uint16_t keys = keyInput;
    if (keys == lastKeys) return;
    for (int i = 0; i < 2; i++) {
        if (~keyCnt[i] & BIT(14)) continue;

        // Interrupts are sent when the condition becomes true: all keys held for AND, any key held for OR
        uint16_t mask = keyCnt[i] & 0x03FF, before = lastKeys & mask, after = keys & mask;
        bool wasMet = (keyCnt[i] & BIT(15)) ? (before == 0) : (before != mask);
        bool isMet = (keyCnt[i] & BIT(15)) ? (after == 0) : (after != mask);
        if (!wasMet && isMet) core->interpreter[i].sendInterrupt(12);
    }
    lastKeys = keys;
}

void Input::writeKeyCnt(int cpu, uint16_t mask, uint16_t value) {
    // Write to one of the KEYCNT registers
    mask &= 0xC3FF;
    keyCnt[cpu] = (keyCnt[cpu] & ~mask) | (value & mask);
}

void Input::pressKey(int key) {
    // Clear key bits to indicate presses
    if (key < 10) // A, B, select, start, right, left, up, down, R, L
        keyInput &= ~BIT(key);
    else if (key < 12) // X, Y
        extKeyIn &= ~BIT(key - 10);
}

void Input::releaseKey(int key) {
    // Set key bits to indicate releases
    if (key < 10) // A, B, select, start, right, left, up, down, R, L
        keyInput |= BIT(key);
    else if (key < 12) // X, Y
        extKeyIn |= BIT(key - 10);
}

void Input::pressScreen() {
    // Clear the pen down bit to indicate a touch press
    extKeyIn &= ~BIT(6);
}

void Input::releaseScreen() {
    // Set the pen down bit to indicate a touch release
    extKeyIn |= BIT(6);
}
