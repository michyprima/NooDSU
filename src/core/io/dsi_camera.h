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

// The DSi's cameras: two Aptina MT9V113 sensors set up over I2C by the ARM7, and the module the ARM9 reads their
// pictures from, a few scanlines at a time through NDMA; pictures come from the frontend, like the GamePad's camera
class DsiCamera {
public:
    static const int WIDTH = 640, HEIGHT = 480;
    static std::atomic<bool> streaming; // Whether a sensor is sending pictures, for the frontend to start its camera
    static void sendFrame(const uint32_t *frame); // A 640x480 YUV422 picture, two pixels a word, from any thread

    DsiCamera(Core *core): core(core) {}
    void saveState(FILE *file);
    void loadState(FILE *file);

    void start();
    void frameStart();
    void transferLine();

    bool i2cSelect(uint8_t device);
    uint8_t i2cRead(bool last);
    void i2cWrite(uint8_t value, bool last);

    uint16_t readMcnt() { return mcnt | BIT(7); }
    uint16_t readCnt() { return cnt | (transferring << 15); }
    uint32_t readData();
    uint32_t readSofs() { return sofs; }
    uint32_t readEofs() { return eofs; }

    void writeMcnt(uint16_t mask, uint16_t value);
    void writeCnt(uint16_t mask, uint16_t value);
    void writeSofs(uint32_t mask, uint32_t value);
    void writeEofs(uint32_t mask, uint32_t value);

private:
    struct Sensor {
        bool resetHeld = true;
        uint8_t dataPos = 0; // Bytes into the current I2C transfer, the first two being the register address
        uint16_t regAddr = 0;
        uint16_t regData = 0;
        uint16_t pllDiv = 0x0366;
        uint16_t pllPDiv = 0x00F5;
        uint16_t pllCnt = 0x21F9;
        uint16_t clocksCnt = 0;
        uint16_t standbyCnt = 0x4029;
        uint16_t miscCnt = 0;
        uint16_t mcuAddr = 0;
        uint8_t mcuVars[0x8000] = {}; // The MCU's variables, which hold the picture sizes and modes

        void reset();
        bool active() const { return !(standbyCnt & 0x4000) && (miscCnt & 0x0200); }
        uint16_t readReg(uint16_t addr) const;
        void writeReg(uint16_t addr, uint16_t value);
        uint16_t mcuVar16(uint16_t addr) const { return mcuVars[addr] | (mcuVars[addr + 1] << 8); }
    };

    Core *core;
    Sensor sensors[2]; // The outer camera, I2C device 0x78, and the inner one facing the player, 0x7A
    int selected = -1; // The sensor addressed on I2C, or none

    uint16_t mcnt = 0;
    uint16_t cnt = 0;
    uint32_t sofs = 0;
    uint32_t eofs = 0;
    bool transferring = false;

    // The module has two buffers of 512 words; one fills from the sensor while NDMA empties the other
    uint32_t buffers[2][512] = {};
    uint16_t readPos[2] = {};
    uint16_t writePos[2] = {};
    uint8_t curBuffer = 0;
    uint8_t bufferLines = 0;

    // The frame being sent, which the sensor scales from its full picture
    int sending = -1;
    uint16_t frameWidth = 0, frameHeight = 0, readMode = 0;
    uint16_t line = 0;
    uint16_t barLine = 0; // Where the line moving down the color bars is
    int sensorY = 0, frameY = 0;
    uint32_t frame[WIDTH * HEIGHT / 2] = {};

    void updateStreaming();
    void swapBuffers();
    int sensorLine(uint32_t *out, int &lines);
};
