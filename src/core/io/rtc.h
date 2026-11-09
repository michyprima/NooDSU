/*
    Copyright 2019-2026 Hydr8gon
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
#include <ctime>
#include "../defines.h"

class Core;

class Rtc {
public:
    static const uint32_t TICK_CYCLES = (60 * 263 * 355 * 6) / 32;
    static std::time_t fixedTime; // Used instead of the current time if set, for deterministic testing

    Rtc(Core *core): core(core) {}
    void saveState(FILE *file);
    void loadState(FILE *file);

    void enableGpRtc() { gpRtc = true; }
    void reset();
    void tick();

    uint8_t readRtc();
    uint16_t readRcnt() { return rcnt; }
    uint16_t readGpData();
    uint16_t readGpDirection() { return gpDirection; }
    uint16_t readGpControl() { return gpControl; }

    void writeRtc(uint8_t value);
    void writeRcnt(uint16_t mask, uint16_t value) { rcnt = (rcnt & ~mask) | (value & mask); }
    void writeGpData(uint16_t value, uint16_t mask);
    void writeGpDirection(uint16_t value, uint16_t mask);
    void writeGpControl(uint16_t value, uint16_t mask);

private:
    Core *core;
    bool gpRtc = false;

    bool csCur = false;
    bool sckCur = false;
    bool sioCur = false;

    uint8_t writeCount = 0;
    uint8_t command = 0;
    uint8_t control = 0;
    uint8_t dateTime[7] = {};
    uint8_t status2 = 0;
    uint8_t alarm[2][3] = {};
    uint8_t alarmDate[2][3] = {};
    uint8_t adjust = 0;
    uint8_t freeReg = 0;
    uint8_t fout[2] = {};
    int64_t dateOffset = 0; // Seconds between the clock the system set and the real one
    uint8_t dayShift = 0;

    bool ticking = false;
    uint8_t subTick = 0;
    uint8_t lastMinute = 0;
    uint8_t irqFlag = 0;
    uint16_t rcnt = 0;

    uint8_t rtc = 0;
    uint16_t gpDirection = 0;
    uint16_t gpControl = 0;

    void updateRtc(bool cs, bool sck, bool sio);
    void updateDateTime(uint8_t *out);
    void applyDateTime();
    void updateTicking();
    void setIrq(uint8_t bits);
    bool alarmMatches(int i, const uint8_t *clock);
    void updateIrq(int type, const uint8_t *clock);

    bool readRegister(uint8_t index);
    void writeRegister(uint8_t index, bool value);
};
