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
#include <cstdint>
#include <cstdio>
#include <thread>

#include "../worker_thread.h"

class Core;

// A register write or a scanline to draw, for a drawing copy of an engine to replay in fast 2D mode
struct Gpu2DEntry {
    enum Type : uint8_t {
        DISPCNT, BGCNT, BGHOFS, BGVOFS, BGPA, BGPB, BGPC, BGPD, BGX, BGY, WINH, WINV,
        WININ, WINOUT, MOSAIC, BLDCNT, BLDALPHA, BLDY, MASTER_BRIGHT, LINE
    };

    Type type;
    uint8_t index; // The background or window, or for scanlines, whether the display capture takes it
    uint32_t mask; // For scanlines, the display capture settings at its start
    uint32_t value; // For scanlines, which one
};

// A queue of entries from the emulator thread to one drawing thread
class Gpu2DQueue {
public:
    void push(const Gpu2DEntry &entry) {
        // Wait for room if the drawing thread is far behind
        uint32_t end = tail.load(std::memory_order_relaxed);
        SpinWait spin;
        while (end - head.load(std::memory_order_acquire) >= SIZE) {
            EP_START_EMU(wait);
            spin.pause();
            EP_ADD_EMU(EP_QUEUE2D, wait);
        }
        entries[end % SIZE] = entry;
        tail.store(end + 1, std::memory_order_release);
    }

    bool pop(Gpu2DEntry &entry) {
        uint32_t start = head.load(std::memory_order_relaxed);
        if (start == tail.load(std::memory_order_acquire)) return false;
        entry = entries[start % SIZE];
        head.store(start + 1, std::memory_order_release);
        return true;
    }

    bool empty() { return head.load(std::memory_order_acquire) == tail.load(std::memory_order_acquire); }

private:
    static const uint32_t SIZE = 4096;
    Gpu2DEntry entries[SIZE];
    std::atomic<uint32_t> head{0}, tail{0};
};

class Gpu2D {
public:
    Gpu2D(Core *core, bool engine);
    void saveState(FILE *file);
    void loadState(FILE *file);

    void reloadRegisters();
    void updateWindows(int line);
    void drawGbaScanline(int line);
    void drawScanline(int line);
    void drawScanline(int line, uint32_t capCnt);

    // In fast 2D mode, register writes are also sent to a drawing copy, which is synced at the start of each frame
    void setQueue(Gpu2DQueue *queue) { this->queue = queue; }
    void copyRegisters(const Gpu2D &other);
    void replay(const Gpu2DEntry &entry);

    uint32_t *getFramebuffer() { return framebuffer; }
    void setFramebuffer(uint32_t *buffer) { framebuffer = buffer ? buffer : ownFramebuffer; }
    uint32_t *getRawLine() { return layers[0]; }

    uint32_t readDispCnt() { return dispCnt; }
    uint16_t readBgCnt(int bg) { return bgCnt[bg]; }
    uint16_t readWinIn() { return winIn; }
    uint16_t readWinOut() { return winOut; }
    uint16_t readBldCnt() { return bldCnt; }
    uint16_t readBldAlpha() { return bldAlpha; }
    uint16_t readMasterBright() { return masterBright; }

    void writeDispCnt(uint32_t mask, uint32_t value);
    void writeBgCnt(int bg, uint16_t mask, uint16_t value);
    void writeBgHOfs(int bg, uint16_t mask, uint16_t value);
    void writeBgVOfs(int bg, uint16_t mask, uint16_t value);
    void writeBgPA(int bg, uint16_t mask, uint16_t value);
    void writeBgPB(int bg, uint16_t mask, uint16_t value);
    void writeBgPC(int bg, uint16_t mask, uint16_t value);
    void writeBgPD(int bg, uint16_t mask, uint16_t value);
    void writeBgX(int bg, uint32_t mask, uint32_t value);
    void writeBgY(int bg, uint32_t mask, uint32_t value);
    void writeWinH(int win, uint16_t mask, uint16_t value);
    void writeWinV(int win, uint16_t mask, uint16_t value);
    void writeWinIn(uint16_t mask, uint16_t value);
    void writeWinOut(uint16_t mask, uint16_t value);
    void writeMosaic(uint16_t mask, uint16_t value);
    void writeBldCnt(uint16_t mask, uint16_t value);
    void writeBldAlpha(uint16_t mask, uint16_t value);
    void writeBldY(uint8_t value);
    void writeMasterBright(uint16_t mask, uint16_t value);

private:
    Core *core;
    bool engine;
    Gpu2DQueue *queue = nullptr;

    uint32_t bgVramAddr, objVramAddr;
    uint8_t *palette, *oam;
    uint8_t **extPalettes;

    uint32_t ownFramebuffer[256 * 192] = {};
    uint32_t *framebuffer = ownFramebuffer;
    bool objWindow[256] = {};
    uint32_t layers[2][256] = {};
    int8_t priorities[2][256] = {};
    int8_t blendBits[2][256] = {};
    uint8_t winMasks[256] = {};

    int internalX[2] = {};
    int internalY[2] = {};
    bool winHFlip[2] = {};
    bool winVFlag[2] = {};

    uint32_t dispCnt = 0;
    uint16_t bgCnt[4] = {};
    uint16_t bgHOfs[4] = {};
    uint16_t bgVOfs[4] = {};
    // The rotation and scaling parameters start as the identity like on hardware, which homebrew can rely on
    int16_t bgPA[2] = { 0x100, 0x100 };
    int16_t bgPB[2] = {};
    int16_t bgPC[2] = {};
    int16_t bgPD[2] = { 0x100, 0x100 };
    int32_t bgX[2] = {};
    int32_t bgY[2] = {};
    uint16_t winX1[2] = {};
    uint16_t winX2[2] = {};
    uint16_t winY1[2] = {};
    uint16_t winY2[2] = {};
    uint16_t winIn = 0;
    uint16_t winOut = 0;
    uint16_t bldCnt = 0;
    uint16_t mosaic = 0;
    uint16_t bldAlpha = 0;
    uint8_t bldY = 0;
    uint16_t masterBright = 0;

    static uint32_t rgb5ToRgb6(uint32_t color);

    void drawLayers(int line);
    void skipLayers();
    void calcWinMasks(int line, int width);
    void drawBgPixel(int bg, int line, int x, uint32_t pixel);
    void drawObjPixel(int line, int x, uint32_t pixel, int8_t priority);
    template <bool windows, bool is3D, bool topOnly> void insertBgLine(int bg, const uint32_t *pixels, int width, int shift);
    template <bool is3D> void drawBgLine(int bg, const uint32_t *pixels, int width, int shift);

    template <bool gbaMode> void drawText(int bg, int line);
    template <bool gbaMode> void drawAffine(int bg, int line);
    void drawExtended(int bg, int line);
    void drawExtendedGba(int bg, int line);
    void drawLarge(int bg, int line);
    template <bool gbaMode> void drawObjects(int line, bool window);
};
