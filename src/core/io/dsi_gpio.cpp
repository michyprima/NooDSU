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

std::atomic<bool> DsiGpio::headphonesIn{false};

void DsiGpio::saveState(FILE *file) {
    // Write DSi state data to the file
    if (!core->dsiMode) return;
    fwrite(&headphones, sizeof(headphones), 1, file);
    fwrite(&gpioOut, sizeof(gpioOut), 1, file);
    fwrite(&gpioDir, sizeof(gpioDir), 1, file);
    fwrite(&gpioEdge, sizeof(gpioEdge), 1, file);
    fwrite(&gpioIe, sizeof(gpioIe), 1, file);
    fwrite(&gpioWifi, sizeof(gpioWifi), 1, file);
}

void DsiGpio::loadState(FILE *file) {
    // Read DSi state data from the file
    if (!core->dsiMode) return;
    fread(&headphones, sizeof(headphones), 1, file);
    fread(&gpioOut, sizeof(gpioOut), 1, file);
    fread(&gpioDir, sizeof(gpioDir), 1, file);
    fread(&gpioEdge, sizeof(gpioEdge), 1, file);
    fread(&gpioIe, sizeof(gpioIe), 1, file);
    fread(&gpioWifi, sizeof(gpioWifi), 1, file);
}

void DsiGpio::update() {
    // Take the headphone state once a frame, since the frontend changes it from another thread
    bool plugged = headphonesIn.load();
    if (!core->dsiMode || core->gbaMode || plugged == headphones) return;
    uint8_t before = readData();
    headphones = plugged;
    uint8_t after = readData();

    // Interrupt on the selected edge of the headphone pin, GPIO33[1], which is IF2 bit 5
    if ((gpioIe & BIT(5)) && ((before ^ after) & BIT(5)) && !((after ^ gpioEdge) & BIT(5)))
        core->interpreter[1].sendInterrupt(32 + 5);

    // The touchscreen controller's headset detection sees the change too
    core->spi.changeHeadset();
}

uint8_t DsiGpio::readData() {
    // Read the pins: GPIO18[0-2] and the testpoint and power button lines idle high, and GPIO33[1] is high with
    // headphones; pins set as outputs give the value written, ANDed with what's outside
    uint8_t inputs = 0xD7 | (headphones << 5);
    return (inputs & ~gpioDir) | (gpioOut & inputs & gpioDir);
}

void DsiGpio::writeWifi(uint16_t mask, uint16_t value) {
    // Write to the GPIO_WIFI register, which picks the DS or DSi Wi-Fi mode on some boards
    mask &= 0x0101;
    gpioWifi = (gpioWifi & ~mask) | (value & mask);
}
