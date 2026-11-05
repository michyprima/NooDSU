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
#include <string>
#include <thread>
#include <vector>

#include "../worker_thread.h"
#include "../atomic.h"

class Core;
class Gpu3DRenderer;
struct Vertex;
struct _Polygon;

// A 3D renderer that runs on the host's GPU, which a frontend can provide
// It renders a frame's polygons on its own thread, filling in each scanline and then marking it as done
class Gpu3DHardware {
public:
    virtual ~Gpu3DHardware() {}
    virtual void render(Gpu3DRenderer *renderer) = 0;
};

// The values for drawing one polygon's span on a scanline
struct Span {
    _Polygon *polygon;
    int line;
    uint32_t x1, x2, x3, x4, x1e, x4e;
    uint32_t x1a, x2a, x3a, x4a;
    bool horizontal;
    uint32_t ze[2], we[2], re[2], ge[2], be[2], se[2], te[2];
};

class Gpu3DRenderer {
public:
    Gpu3DRenderer(Core *core);
    ~Gpu3DRenderer();

    void saveState(FILE *file);
    void loadState(FILE *file);

    void drawScanline(int line);
    void finishHardwareFrame();
    uint32_t *getLine(int line);
    bool hasFrame() { return (hardwareFrame && hardwareBuffer) || framebuffer; }

    uint16_t readDisp3DCnt() { return disp3DCnt; }

    // For hardware renderers, which get the frame's state and textures here and write scanlines back
    static Gpu3DHardware *hardware;
    Core *getCore() { return core; }
    bool getResShift() { return resShift; }
    uint32_t getClearColor() { return clearColor; }
    uint16_t getClearDepth() { return clearDepth; }
    uint16_t getClearImageOffset() { return clearImageOffset; }
    uint16_t getToon(int index) { return toonTable[index]; }
    uint16_t getEdgeColor(int index) { return edgeColor[index]; }
    uint32_t getFogColor() { return fogColor; }
    uint16_t getFogOffset() { return fogOffset; }
    uint8_t getFogTable(int index) { return fogTable[index]; }
    uint8_t getAlphaTestRef() { return alphaTestRef; }
    uint64_t hashTexture(_Polygon *polygon);
    void decodeTexture(_Polygon *polygon, uint32_t *out);
    uint32_t *hardwareLine(int line) { return allocBuffers() ? &framebuffer[0][line * 256 * 2] : nullptr; }
    void setHardwareBuffer(uint32_t *buffer) { hardwareBuffer = buffer; }
    uint32_t *getHardwareCopy() { return hardwareCopy; }
    void hardwareCopyDone() { hardwareCopied = true; }
    uint32_t *takeHardwareCopy();
    uint32_t *reuseHardwareCopy();
    void forgetHardwareRows(int first, int count);
#ifdef FRAME_CAPTURE
    uint32_t captureId = 0; // The current 3D frame's ID for frame capture
#endif
    void hardwareDone(int line) { ready[line].store(3); }
#ifdef GPU_DUMP
    void softwareFrame(uint32_t *out);
    std::string dumpLayerPath; // Where to save the next finished 3D layer
#endif

    void writeDisp3DCnt(uint16_t mask, uint16_t value);
    void writeEdgeColor(int index, uint16_t mask, uint16_t value);
    void writeClearColor(uint32_t mask, uint32_t value);
    void writeClearDepth(uint16_t mask, uint16_t value);
    void writeClearImageOffset(uint16_t mask, uint16_t value);
    void writeFogColor(uint32_t mask, uint32_t value);
    void writeFogOffset(uint16_t mask, uint16_t value);
    void writeFogTable(int index, uint8_t value);
    void writeAlphaTestRef(uint8_t value);
    void writeToonTable(int index, uint16_t mask, uint16_t value);

private:
    friend struct LinearStepper;
    Core *core;

    bool resShift = false;
    bool hardwareFrame = false; // Whether the current frame is being rendered by the hardware renderer
    int hardwareLines = 192; // How many scanlines the hardware frame has, which doubles in high-res
    uint32_t *hardwareBuffer = nullptr; // Where a hardware renderer leaves its frames instead, with lines 512 apart
    uint32_t *hardwareCopy = nullptr; // Where a hardware renderer also copies high-res frames for the frontend
    bool hardwareCopied = false; // Whether the current frame was copied there
    uint32_t *lastCopy = nullptr; // The last copy handed over, held while no frame has been drawn since, to hand it again

    // Get where a finished scanline is, which can be in the hardware renderer's own buffer
    uint32_t *lineData(int line) {
        if (hardwareFrame && hardwareBuffer) return &hardwareBuffer[line * 256 * 2];
        return framebuffer ? &framebuffer[0][line * 256 * 2] : blankLine;
    }
    // Each layer is padded by 512 bytes, since the layers' sizes are a multiple of the cache's way size, which would
    // make the same pixel in all of them compete for the same cache set
    static const int LAYER_SIZE = 256 * 192 * 4 + 128;
    // The software renderer's buffers are only allocated once it's used, since the hardware renderer doesn't need them
    struct SoftwareBuffers {
        uint32_t framebuffer[2][LAYER_SIZE];
        int32_t depthBuffer[2][LAYER_SIZE];
        uint32_t attribBuffer[2][LAYER_SIZE];
        uint8_t stencilBuffer[256 * 192 * 4];
    };
    SoftwareBuffers *softwareBuffers = nullptr;
    uint32_t (*framebuffer)[LAYER_SIZE] = nullptr;
    int32_t (*depthBuffer)[LAYER_SIZE] = nullptr;
    uint32_t (*attribBuffer)[LAYER_SIZE] = nullptr;
    uint8_t *stencilBuffer = nullptr;
    static uint32_t blankLine[256 * 2]; // Returned for scanlines before any buffers exist, as transparent pixels
    bool allocBuffers();
    bool stencilClear[256 * 2] = {};

    int polygonTop[2048] = {};
    int polygonBot[2048] = {};
    std::vector<uint16_t> linePolygons;
    int lineStarts[192 * 2 + 1] = {};

    uint8_t activeThreads = 0;
    std::vector<WorkerThread*> threads;
    Atomic32<int> ready[192 * 2];

    uint16_t disp3DCnt = 0;
    uint16_t edgeColor[8] = {};
    uint32_t clearColor = 0;
    uint16_t clearDepth = 0;
    uint16_t clearImageOffset = 0; // Where the clear images start, X in the low byte and Y in the high byte
    uint32_t fogColor = 0;
    uint16_t fogOffset = 0;
    uint8_t fogTable[32] = {};
    uint8_t alphaTestRef = 0;
    uint16_t toonTable[32] = {};

    static uint32_t rgba5ToRgba6(uint32_t color);

    uint32_t *getLine1(int line);

    void drawThreaded(int thread);
    void drawScanline1(int line);
    void finishScanline(int line);

    uint8_t *getTexture(uint32_t address);
    uint8_t *getPalette(uint32_t address);

    static uint32_t interpolateLinear(uint32_t v1, uint32_t v2, uint32_t x1, uint32_t x, uint32_t x2);
    static uint32_t interpolateLinRev(uint32_t v1, uint32_t v2, uint32_t x1, uint32_t x, uint32_t x2);
    static uint32_t interpolateFactor(uint32_t factor, uint32_t shift, uint32_t v1, uint32_t v2);
    static uint32_t interpolateColor(uint32_t c1, uint32_t c2, uint32_t x1, uint32_t x, uint32_t x2);
    static uint32_t blendColor(uint32_t c1, uint32_t c2, uint32_t a);

    uint32_t readTexture(_Polygon *polygon, int s, int t);
    void drawPolygon(int line, int polygonIndex);
    template <bool wBuffer, bool antiAlias, bool textured> void drawSpan(const Span &span);
};
