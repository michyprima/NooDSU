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

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <queue>
#include <mutex>

#include "../defines.h"

class Core;

class Spu {
public:
    Spu(Core *core);
    ~Spu();

    void saveState(FILE *file);
    void loadState(FILE *file);

    uint32_t *getSamples(int count);
    bool ringLow();
    uint32_t ringLevel() { return ringHead.load() - ringTail.load(); }
    bool ringCatching() { return ringCatchingUp; }
#ifdef FRAME_LOG
    uint32_t ringDry = 0; // Output samples that had to repeat the last one because the ring ran dry
    uint32_t ringFill = 0; // Samples in the ring when output last read from it
#endif
    void runGbaSample();
    void runSamples();
    void resetCycles();
    void gbaFifoTimer(int timer);

    uint8_t readGbaSoundCntL(int channel);
    uint16_t readGbaSoundCntH(int channel);
    uint16_t readGbaSoundCntX(int channel);
    uint16_t readGbaMainSoundCntL() { return gbaMainSoundCntL; }
    uint16_t readGbaMainSoundCntH() { return gbaMainSoundCntH; }
    uint8_t readGbaMainSoundCntX() { return gbaMainSoundCntX; }
    uint16_t readGbaSoundBias() { return gbaSoundBias; }
    uint8_t readGbaWaveRam(int index);

    uint32_t readSoundCnt(int channel) { syncSamples(); return soundCnt[channel]; }
    uint16_t readMainSoundCnt() { return mainSoundCnt; }
    uint16_t readSoundBias() { return soundBias; }
    uint8_t readSndCapCnt(int channel) { syncSamples(); return sndCapCnt[channel]; }
    uint32_t readSndCapDad(int channel) { return sndCapDad[channel]; }

    void writeGbaSoundCntL(int channel, uint8_t value);
    void writeGbaSoundCntH(int channel, uint16_t mask, uint16_t value);
    void writeGbaSoundCntX(int channel, uint16_t mask, uint16_t value);
    void writeGbaMainSoundCntL(uint16_t mask, uint16_t value);
    void writeGbaMainSoundCntH(uint16_t mask, uint16_t value);
    void writeGbaMainSoundCntX(uint8_t value);
    void writeGbaSoundBias(uint16_t mask, uint16_t value);
    void writeGbaWaveRam(int index, uint8_t value);
    void writeGbaFifoA(uint32_t mask, uint32_t value);
    void writeGbaFifoB(uint32_t mask, uint32_t value);

    void writeSoundCnt(int channel, uint32_t mask, uint32_t value);
    void writeSoundSad(int channel, uint32_t mask, uint32_t value);
    void writeSoundTmr(int channel, uint16_t mask, uint16_t value);
    void writeSoundPnt(int channel, uint16_t mask, uint16_t value);
    void writeSoundLen(int channel, uint32_t mask, uint32_t value);
    void writeMainSoundCnt(uint16_t mask, uint16_t value);
    void writeSoundBias(uint16_t mask, uint16_t value);
    void writeSndCapCnt(int channel, uint8_t value);
    void writeSndCapDad(int channel, uint32_t mask, uint32_t value);
    void writeSndCapLen(int channel, uint16_t mask, uint16_t value);

private:
    Core *core;

    uint32_t *bufferIn = nullptr, *bufferOut = nullptr;
    uint32_t bufferSize = 0, bufferPointer = 0;

    std::condition_variable cond1, cond2;
    std::mutex mutex1, mutex2;
    std::atomic<bool> ready;

    // When syncing to the display, samples go through a ring buffer instead, which output reads at a rate adjusted to
    // keep it about 62 ms full, since the emulator then runs at the display's rate instead of the DS's
    static const uint32_t RING_SIZE = 16384, RING_TARGET = 2048;
    uint32_t ring[RING_SIZE] = {};
    std::atomic<uint32_t> ringHead{0}, ringTail{0};
    float ringPosition = 0;
    uint32_t ringLast = 0;
    bool ringCatchingUp = false;
    void readRing(uint32_t *out, int count);

    int16_t gbaFrameSequencer = 0;
    int32_t gbaSoundTimers[4] = {};
    int8_t gbaEnvelopes[3] = {};
    int8_t gbaEnvTimers[3] = {};
    int8_t gbaSweepTimer = 0;
    int8_t gbaWaveDigit = 0;
    uint16_t gbaNoiseValue = 0;

    uint8_t gbaWaveRam[2][16] = {};
    std::deque<int8_t> gbaFifos[2];
    int8_t gbaSampleA = 0, gbaSampleB = 0;

    uint16_t enabled = 0;

    // Samples are made in batches, since a scheduled task for each one keeps stopping the CPUs; a batch ends when its
    // task runs, and anything that can see or change the sound registers makes the samples due before it first
    uint32_t batchEnd = SPU_BATCH * 512 * 2;
    uint32_t batchDone = 0;
    void syncSamples();
    void mixSample();

    // Each channel fetches the data it will play ahead of time, like the hardware's FIFOs, so software refilling a
    // buffer just behind the playing position doesn't change what's still to be played in a batch made late; a
    // channel reads from where it is, and from its loop start if it gets there within the batch
    static const uint32_t FETCH_SIZE = 128;
    uint8_t fetchData[16][2][FETCH_SIZE];
    uint32_t fetchAddress[16][2] = {};
    uint32_t fetchCount[16][2] = {};
    void prefetch();
    template <typename T> T readData(int channel, uint32_t address);

    static const int indexTable[8];
    static const int16_t adpcmTable[89];

    int32_t adpcmValue[16] = {}, adpcmLoopValue[16] = {};
    int8_t adpcmIndex[16] = {}, adpcmLoopIndex[16] = {};
    bool adpcmToggle[16] = {};

    uint8_t dutyCycles[6] = {};
    uint16_t noiseValues[2] = {};
    uint32_t soundCurrent[16] = {};
    uint16_t soundTimers[16] = {};
    uint32_t sndCapCurrent[2] = {};
    uint16_t sndCapTimers[2] = {};

    uint8_t gbaSoundCntL[2] = {};
    uint16_t gbaSoundCntH[4] = {};
    uint16_t gbaSoundCntX[4] = {};
    uint16_t gbaMainSoundCntL = 0;
    uint16_t gbaMainSoundCntH = 0;
    uint8_t gbaMainSoundCntX = 0;
    uint16_t gbaSoundBias = 0;

    uint32_t soundCnt[16] = {};
    uint32_t soundSad[16] = {};
    uint16_t soundTmr[16] = {};
    uint16_t soundPnt[16] = {};
    uint32_t soundLen[16] = {};
    uint16_t mainSoundCnt = 0;
    uint16_t soundBias = 0;
    uint8_t sndCapCnt[2] = {};
    uint32_t sndCapDad[2] = {};
    uint16_t sndCapLen[2] = {};

    void pushSample(int16_t sampleLeft, int16_t sampleRight);
    void startChannel(int channel);
};
