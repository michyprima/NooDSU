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

#include <cstdint>
#include <cstdio>

class Core;
class DspUcode;
namespace Teakra { class Teakra; }

// The DSi's DSP, a TeakLite II that runs programs the ARM9 loads into new WRAM, with the registers the ARM9 talks to it
// through; the processor itself is Teakra's, the registers follow melonDS, and Nintendo's own programs can be done in
// high level instead (dsi_dsp_hle)
class DsiDsp {
public:
    DsiDsp(Core *core): core(core) {}
    ~DsiDsp();
    void saveState(FILE *file);
    void loadState(FILE *file);

    void update();
    void finishHle();
    void resetCycles();
    void remap() { pagesDirty = true; }
    void clockChanged();
    bool takeSample(int16_t *sample);
    void replyIrq(int i);
    void semaphoreIrq();

    uint16_t readRst() { return rst; }
    uint16_t readPdata();
    uint16_t readPcfg() { return pcfg; }
    uint16_t readPsts();
    uint16_t readPsem() { return psem; }
    uint16_t readPmask() { return pmask; }
    uint16_t readSem();
    uint16_t readCmd(int i) { return cmd[i]; }
    uint16_t readRep(int i);

    void writeRst(uint16_t mask, uint16_t value);
    void writePdata(uint16_t mask, uint16_t value);
    void writePadr(uint16_t mask, uint16_t value);
    void writePcfg(uint16_t mask, uint16_t value);
    void writePsem(uint16_t mask, uint16_t value);
    void writePmask(uint16_t mask, uint16_t value);
    void writePclear(uint16_t mask, uint16_t value);
    void writeCmd(int i, uint16_t mask, uint16_t value);

private:
    Core *core;
    Teakra::Teakra *teakra = nullptr;
    DspUcode *ucode = nullptr; // The program done in high level, used in place of Teakra while set

    uint8_t *pages[16] = {}; // The new WRAM the DSP sees: 8 pages of block B as program memory, then 8 of C as data
    bool pagesDirty = true;
    uint32_t lastCycles = 0; // When the DSP was last brought up to date
    bool updatePending = false;

    uint16_t readFifo[16] = {}; // Words fetched ahead for PDATA reads
    uint8_t readHead = 0;
    uint8_t readCount = 0;
    int32_t readLength = 0; // Words left to fetch, or -1 for no limit

    int16_t samples[64][2] = {}; // Sound the DSP sent out, waiting to be mixed
    uint8_t sampleHead = 0;
    uint8_t sampleCount = 0;
    uint32_t samplePhase = 0; // How far the mixer is into the next sample, when the DSP's rate is higher than its own

    uint16_t rst = 0;
    uint16_t padr = 0;
    uint16_t pcfg = 0;
    uint16_t psts = 0;
    uint16_t psem = 0;
    uint16_t pmask = 0xFF;
    uint16_t cmd[3] = {};

    bool accessible();
    bool running();
    uint32_t frameCycles();
    void create();
    void startUcode();
    void stopUcode();
    void reset();
    void updatePages();
    void catchUp();
    void requestUpdate();
    uint32_t transferAddress();
    uint16_t transferRead();
    void fetch();
};
