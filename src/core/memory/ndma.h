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

class Core;

class Ndma {
public:
    Ndma(Core *core, bool arm7): core(core), arm7(arm7) {}
    void saveState(FILE *file);
    void loadState(FILE *file);

    void trigger(uint8_t type);
    void setDrq(uint8_t type);
    void clearDrq(uint8_t type);
    void update();
    bool cartArmed();
    void transferNow(uint8_t type);

    uint32_t readGcnt() { return ndmaGcnt; }
    uint32_t readSad(int i) { return ndmaSad[i]; }
    uint32_t readDad(int i) { return ndmaDad[i]; }
    uint32_t readTcnt(int i) { return ndmaTcnt[i]; }
    uint32_t readWcnt(int i) { return ndmaWcnt[i]; }
    uint32_t readBcnt(int i) { return ndmaBcnt[i]; }
    uint32_t readFdata(int i) { return ndmaFdata[i]; }
    uint32_t readCnt(int i) { return ndmaCnt[i]; }

    void writeGcnt(uint32_t mask, uint32_t value);
    void writeSad(int i, uint32_t mask, uint32_t value);
    void writeDad(int i, uint32_t mask, uint32_t value);
    void writeTcnt(int i, uint32_t mask, uint32_t value);
    void writeWcnt(int i, uint32_t mask, uint32_t value);
    void writeBcnt(int i, uint32_t mask, uint32_t value);
    void writeFdata(int i, uint32_t mask, uint32_t value);
    void writeCnt(int i, uint32_t mask, uint32_t value);

private:
    Core *core;
    bool arm7;

    uint32_t srcAddrs[4] = {};
    uint32_t dstAddrs[4] = {};
    uint32_t totalLeft[4] = {};
    uint32_t blockLeft[4] = {};
    uint32_t drqMask = 0xFFFF0000;
    uint8_t runMask = 0;

    uint32_t ndmaGcnt = 0;
    uint32_t ndmaSad[4] = {};
    uint32_t ndmaDad[4] = {};
    uint32_t ndmaTcnt[4] = {};
    uint32_t ndmaWcnt[4] = {};
    uint32_t ndmaBcnt[4] = {};
    uint32_t ndmaFdata[4] = {};
    uint32_t ndmaCnt[4] = {};

    bool shouldTransfer(int i, uint8_t type);
    bool transferBlock(int i);
};
