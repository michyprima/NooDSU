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

// The DSi's microphone FIFO, along with SNDEXCNT, which sets its base rate and turns it on
class DsiMic {
public:
    DsiMic(Core *core): core(core) {}
    void saveState(FILE *file);
    void loadState(FILE *file);

    void update();
    void resetCycles();

    uint16_t readMicCnt();
    uint32_t readMicData();
    uint16_t readSndExCnt() { return sndExCnt; }

    void writeMicCnt(uint16_t mask, uint16_t value);
    void writeSndExCnt(uint16_t mask, uint16_t value);

private:
    Core *core;

    uint16_t fifo[32] = {}; // Halfwords, 2 to a word
    uint8_t fifoHead = 0;
    uint8_t fifoCount = 0;
    bool overrun = false;
    bool irqSent = false; // Whether the interrupt was sent since the FIFO was last below its level, or cleared in mode 2
    uint32_t nextSample = 0; // When the next slot the format takes comes
    bool nextRight = false; // Whether that slot is the right channel's
    int16_t lastSample = 0; // The last slot's sample, which the right slot repeats in stereo
    uint32_t lastData = 0; // The last word read, which reads of an empty FIFO give again
    bool drqHigh = false; // Whether NDMA has been asked for a block since the FIFO was last empty
    bool drqEdge = false; // Whether that request is yet to start NDMA
    uint32_t updateAt = 0; // When the earliest scheduled update is due
    bool updatePending = false;

    uint16_t micCnt = 0;
    uint16_t sndExCnt = 0;

    bool running();
    uint32_t frameCycles();
    uint32_t periodCycles();
    bool takes(bool right);
    void nextSlot(uint32_t &time, bool &right);
    void start();
    void catchUp();
    void push(int16_t value);
    uint32_t thresholdTime();
    void requestUpdate(uint32_t time);
};
