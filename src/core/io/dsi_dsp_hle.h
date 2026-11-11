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
#include <vector>

class Core;
class DsiDsp;

// The DSP programs Nintendo shipped, done in C++ instead of running them, after melonDS's DSP HLE; a program is known by
// the CRC32 of the program memory it was loaded into, and anything else is left to Teakra
class DspUcode {
public:
    enum Kind { AAC, GRAPHICS, G711 };

    static DspUcode *create(Core *core, DsiDsp *dsp, uint8_t **pages);
    static DspUcode *create(Core *core, DsiDsp *dsp, uint8_t **pages, uint32_t id);
    virtual ~DspUcode() {}
    virtual void saveState(FILE *file);
    virtual void loadState(FILE *file);

    uint32_t getId() { return (kind << 16) | (uint16_t)version; }
    void start();
    void finish();
    void resetCycles();
    void sampleClock(int16_t *output, int16_t input);

    bool replyReady(int i) { return replyWritten[i]; }
    bool cmdEmpty(int i) { return !cmdWritten[i]; }
    uint16_t readReply(int i);
    virtual void writeCmd(int i, uint16_t value);

    uint16_t getSemaphore() { return semOut; }
    void setSemaphore(uint16_t value) { semIn |= value; }
    void clearSemaphore(uint16_t value) { semOut &= ~value; }
    void maskSemaphore(uint16_t value) { semMask = value; }

    uint16_t readData(uint32_t address);
    void writeData(uint32_t address, uint16_t value);
    uint16_t readProgram(uint32_t address);
    void writeProgram(uint32_t address, uint16_t value);

protected:
    static const uint16_t PIPE_MONITOR = 0x0800;
    static const uint16_t PIPE_BUFFERS = 0x1000;
    static const uint16_t MIC_BUFFER = 0x2000;

    Core *core;
    DsiDsp *dsp;
    uint8_t **pages;
    int kind;
    int version; // The SDK version times 16, plus 1 for a patch; -1 is the AAC program in DSi Sound

    bool exited = false;
    uint16_t cmd[3] = {};
    bool cmdWritten[3] = {};
    uint16_t reply[3] = {};
    bool replyWritten[3] = {};
    uint8_t replyAction = 0; // What to do once the ARM9 reads REP2, with its parameter
    uint32_t replyParam = 0;

    uint16_t semIn = 0;
    uint16_t semOut = 0;
    uint16_t semMask = 0;

    bool playing = false;
    bool playHalved = false;
    uint32_t playAddress = 0;
    uint32_t playLength = 0;
    int16_t playFifo[8] = {}; // Samples fetched ahead, each played on both sides
    uint8_t playHead = 0;
    uint8_t playCount = 0;

    bool micSampling = false;
    int16_t micFifo[8] = {};
    uint8_t micCount = 0;

    bool finishPending = false; // A command running, finished when the system reaches finishCycles
    uint32_t finishCycles = 0;
    uint16_t finishParam = 0;

    DspUcode(Core *core, DsiDsp *dsp, uint8_t **pages, int kind, int version):
        core(core), dsp(dsp), pages(pages), kind(kind), version(version) {}
    virtual void finishCmd(uint16_t param) = 0;
    void scheduleFinish(uint32_t cycles, uint16_t param);

    void sendReply(int i, uint16_t value);
    void setReplyAction(uint8_t action, uint32_t param);
    void replyRead();
    void setSemaphoreOut(uint16_t value);

    uint32_t pipeLength(int pipe);
    uint32_t readPipe(int pipe, uint16_t *data, uint32_t length);
    uint32_t writePipe(int pipe, const uint16_t *data, uint32_t length);
    void readArm9(uint16_t *data, uint32_t address, uint32_t size);
    void writeArm9(const uint16_t *data, uint32_t address, uint32_t size);

    void startAudioCmd();
    void advancePlay();
    void advanceMic();
};

// AAC decoding, of a frame at a time, which DSi Sound uses for its music player
class AacUcode: public DspUcode {
public:
    AacUcode(Core *core, DsiDsp *dsp, uint8_t **pages, int version): DspUcode(core, dsp, pages, AAC, version) {}
    ~AacUcode();
    void saveState(FILE *file);
    void loadState(FILE *file);
    void writeCmd(int i, uint16_t value);

private:
    void *decoder = nullptr;
    int lastFrequency = -1;
    int lastChannels = -1;

    uint8_t cmdState = 0;
    uint8_t paramCount = 0;
    uint16_t params[10] = {};

    void receiveCmd();
    void decodeFrame();
    bool configure(uint32_t frequency, int channels);
    void finishCmd(uint16_t param);
};

// G.711 A-law and u-law encoding and decoding
class G711Ucode: public DspUcode {
public:
    G711Ucode(Core *core, DsiDsp *dsp, uint8_t **pages, int version): DspUcode(core, dsp, pages, G711, version) {}
    void saveState(FILE *file);
    void loadState(FILE *file);
    void writeCmd(int i, uint16_t value);

private:
    uint8_t cmdState = 0;
    uint16_t params[8] = {};

    void tryStartCmd();
    void finishCmd(uint16_t param);
    void encodeALaw();
    void encodeULaw();
    void decodeALaw();
    void decodeULaw();
};

// Scaling pictures and converting YUV to RGB, for software built with Nintendo's DSP graphics library
class GraphicsUcode: public DspUcode {
public:
    GraphicsUcode(Core *core, DsiDsp *dsp, uint8_t **pages, int version): DspUcode(core, dsp, pages, GRAPHICS, version) {}
    void saveState(FILE *file);
    void loadState(FILE *file);
    void writeCmd(int i, uint16_t value);

private:
    uint8_t cmdState = 0;
    uint16_t cmdIndex = 0;
    uint16_t params[14] = {};
    std::vector<uint16_t> src, dst; // Lines being worked on, which the program keeps in its data memory

    void tryStartCmd();
    void finishCmd(uint16_t param);
    void scaleNearest();
    void scaleBilinear();
    void scaleBicubic();
    void scaleThird();
    void yuvToRgb();
};
