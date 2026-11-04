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
#include <mutex>
#include <queue>
#include <vector>

#include "../defines.h"
#include "../worker_thread.h"

class Core;

enum GXState {
    GX_IDLE = 0,
    GX_RUNNING,
    GX_HALTED
};

// The command is kept in a full word since writing bytes makes the compiler assume anything could have changed, which
// costs a lot of reloading in loops that handle thousands of entries a frame
struct Entry {
    uint32_t command;
    uint32_t param;

    Entry() {}
    Entry(uint8_t command, uint32_t param): command(command), param(param) {}
};

// A ring buffer FIFO of geometry entries, which grows in the rare case that it fills up
// This avoids the overhead of a deque for every command that's sent
class EntryFifo {
    friend class Gpu3D;

public:
    typedef Entry value_type;

    size_t size() const { return count; }
    bool empty() const { return count == 0; }
    Entry &front() { return entries[head]; }
    Entry &operator[](size_t i) { return entries[(head + i) & mask]; }
    void pop_front() { head = (head + 1) & mask; count--; }

    void pop_params(uint32_t *params, size_t n) {
        // Take the parameters of the next n entries
        const Entry *from = entries;
        size_t start = head, wrap = mask;
        for (size_t i = 0; i < n; i++)
            params[i] = from[(start + i) & wrap].param;
        head = (start + n) & wrap;
        count -= n;
    }
    void clear() { head = count = 0; }

    void push_back(const Entry &entry) {
        if (count > mask) grow();
        entries[(head + count) & mask] = entry;
        count++;
    }

private:
    std::vector<Entry> data = std::vector<Entry>(512);
    Entry *entries = data.data();
    size_t mask = data.size() - 1, head = 0, count = 0;

    void grow() {
        // Double the size of the buffer, keeping entries in order
        std::vector<Entry> bigger(data.size() * 2);
        for (size_t i = 0; i < count; i++)
            bigger[i] = (*this)[i];
        data.swap(bigger);
        entries = data.data();
        mask = data.size() - 1;
        head = 0;
    }
};

struct Matrix {
    int32_t data[4 * 4] = {
        1 << 12, 0 << 12, 0 << 12, 0 << 12,
        0 << 12, 1 << 12, 0 << 12, 0 << 12,
        0 << 12, 0 << 12, 1 << 12, 0 << 12,
        0 << 12, 0 << 12, 0 << 12, 1 << 12
    };

    Matrix operator*(Matrix &mtx);
};

// The matrices and stacks used by the geometry engine
struct MatrixState {
    uint8_t mode = 0;
    bool clipDirty = false;
    Matrix projection, projectionStack;
    Matrix coordinate, coordinateStack[32];
    Matrix direction, directionStack[32];
    Matrix texture, textureStack;
    Matrix clip;

    void updateClip() {
        // Update the clip matrix if the matrices that make it up have changed
        if (!clipDirty) return;
        clip = coordinate * projection;
        clipDirty = false;
    }
};

struct Vector {
    int32_t x = 0, y = 0, z = 0;

    int32_t operator*(Vector &vtr);
    Vector operator*(Matrix &mtx);
};

struct Vertex: Vector {
    int32_t w = 0;
    int16_t s = 0, t = 0;
    uint32_t color = 0;

    Vertex operator*(Matrix &mtx);
};

struct _Polygon {
    uint32_t textureAddr = 0, paletteAddr = 0;
    uint16_t sizeS = 0, sizeT = 0;
    uint8_t textureFmt = 0;
    bool repeatS = false, repeatT = false;
    bool flipS = false, flipT = false;
    bool transparent0 = false;

    uint16_t vertices = 0;
    uint8_t size = 0;
    int8_t wShift = 0;
    bool wBuffer = false;
    bool crossed = false;
    bool clockwise = false;

    uint8_t mode = 0;
    uint8_t alpha = 0;
    uint8_t id = 0;
    bool transNewDepth = false;
    bool depthTestEqual = false;
    bool fog = false;
};

class Gpu3D {
    friend class Core;
public:
    _Polygon *polygonsOut = polygons2;
    Vertex *verticesOut = vertices2;
    uint16_t polygonCountOut = 0;
    uint16_t vertexCountOut = 0;

    Gpu3D(Core *core): core(core) {}
    ~Gpu3D();
    void saveState(FILE *file);
    void loadState(FILE *file);

    void runCommands();
    void swapBuffers();
    bool shouldSwap() { return state == GX_HALTED; }

    uint32_t readGxStat();
    bool fifoHalfEmpty() { return gxStat & BIT(25); }
    void checkFifoIrq();
    uint32_t readPosResult(int index) { sync(); return posResult[index]; }
    uint32_t readVecResult(int index) { sync(); return vecResult[index]; }
    uint32_t readRamCount();
    uint32_t readClipMtxResult(int index);
    uint32_t readVecMtxResult(int index);

    void writeGxFifo(uint32_t mask, uint32_t value);
    void writeGxFifo(const uint8_t *data, int count);
    void writeMtxMode(uint32_t mask, uint32_t value);
    void writeMtxPush(uint32_t mask, uint32_t value);
    void writeMtxPop(uint32_t mask, uint32_t value);
    void writeMtxStore(uint32_t mask, uint32_t value);
    void writeMtxRestore(uint32_t mask, uint32_t value);
    void writeMtxIdentity(uint32_t mask, uint32_t value);
    void writeMtxLoad44(uint32_t mask, uint32_t value);
    void writeMtxLoad43(uint32_t mask, uint32_t value);
    void writeMtxMult44(uint32_t mask, uint32_t value);
    void writeMtxMult43(uint32_t mask, uint32_t value);
    void writeMtxMult33(uint32_t mask, uint32_t value);
    void writeMtxScale(uint32_t mask, uint32_t value);
    void writeMtxTrans(uint32_t mask, uint32_t value);
    void writeColor(uint32_t mask, uint32_t value);
    void writeNormal(uint32_t mask, uint32_t value);
    void writeTexCoord(uint32_t mask, uint32_t value);
    void writeVtx16(uint32_t mask, uint32_t value);
    void writeVtx10(uint32_t mask, uint32_t value);
    void writeVtxXY(uint32_t mask, uint32_t value);
    void writeVtxXZ(uint32_t mask, uint32_t value);
    void writeVtxYZ(uint32_t mask, uint32_t value);
    void writeVtxDiff(uint32_t mask, uint32_t value);
    void writePolygonAttr(uint32_t mask, uint32_t value);
    void writeTexImageParam(uint32_t mask, uint32_t value);
    void writePlttBase(uint32_t mask, uint32_t value);
    void writeDifAmb(uint32_t mask, uint32_t value);
    void writeSpeEmi(uint32_t mask, uint32_t value);
    void writeLightVector(uint32_t mask, uint32_t value);
    void writeLightColor(uint32_t mask, uint32_t value);
    void writeShininess(uint32_t mask, uint32_t value);
    void writeBeginVtxs(uint32_t mask, uint32_t value);
    void writeEndVtxs(uint32_t mask, uint32_t value);
    void writeSwapBuffers(uint32_t mask, uint32_t value);
    void writeViewport(uint32_t mask, uint32_t value);
    void writeBoxTest(uint32_t mask, uint32_t value);
    void writePosTest(uint32_t mask, uint32_t value);
    void writeVecTest(uint32_t mask, uint32_t value);
    void writeGxStat(uint32_t mask, uint32_t value);

private:
    Core *core;

    GXState state = GX_IDLE;

    EntryFifo fifo;
    uint32_t pipeSize = 0;
    uint32_t testQueue = 0;
    uint32_t matrixQueue = 0;

    static const uint8_t paramCounts[0x100];

    // The execution state, and the FIFO's copy that's kept when there's a worker
    MatrixState mtx, fifoMtx;

    Vertex vertices1[6144], vertices2[6144];
    Vertex *verticesIn = vertices1;
    uint16_t vertexCountIn = 0;
    uint16_t processCount = 0;

    _Polygon polygons1[2048], polygons2[2048];
    _Polygon *polygonsIn = polygons1;
    uint16_t polygonCountIn = 0;

    Vertex savedVertex;
    _Polygon savedPolygon;
    int16_t s, t;

    int vertexCount = 0;
    bool clockwise = false;
    int polygonType = 0;
    int textureCoordMode = 0;

    uint32_t polygonAttr = 0;
    uint8_t enabledLights = 0;
    bool renderBack = false, renderFront = false;

    uint32_t diffuseColor = 0, ambientColor = 0;
    uint32_t specularColor = 0, emissionColor = 0;
    bool shininessEnabled = false;
    Vector lightVector[4], halfVector[4];
    uint32_t lightColor[4] = {};
    uint8_t shininess[128] = {};

    uint16_t viewport[4] = { 0, 0, 256, 192 };
    uint16_t viewportNext[4] = { 0, 0, 256, 192 };

    uint32_t gxFifo = 0x00000000;
    uint32_t gxStat = 0x04000000;
    uint32_t gxStatExec = 0;
    int32_t posResult[4] = {};
    int16_t vecResult[3] = {};

    uint8_t gxFifoCount = 0;

    // Commands can be executed on a worker thread, while the FIFO is still handled on the emulator thread
    // Commands are passed through a ring of words, and anything that reads their results waits for them first
    static const uint32_t QUEUE_SIZE = 0x10000;
    static const uint32_t EXEC_STAT_BITS = BIT(1);
    // Each side only publishes its position now and then, and shared values are padded onto their own cache lines,
    // since sharing cache lines between cores is slow
    WorkerThread *worker = nullptr;
    uint32_t *queue = nullptr, *queueAlloc = nullptr;
    uint32_t queueEnd = 0, queueStart = 0;
    uint32_t statSent = 0;
    uint8_t padding1[64];
    std::atomic<uint32_t> queueHead, statDone;
    std::atomic<bool> workerSleeping;
    uint8_t padding2[64];
    std::atomic<uint32_t> queueTail;
    std::atomic<bool> workerStop;
    uint8_t padding3[64];
    std::mutex workerMutex;
    std::condition_variable workerCond;

    void startWorker();
    void stopWorker();
    void runWorker();
    void queueCommand(uint8_t command, uint32_t *params, int count);
    void publishQueue(bool force);
    void sync();
    void syncStat();

    int stackSlot(uint8_t mode, uint8_t command, uint32_t param);
    int takeCommand(uint8_t command, uint32_t *params, int count);
    void execute(uint8_t command, uint32_t *params);

    static uint32_t rgb5ToRgb6(uint16_t color);
    static Vertex intersection(Vertex *vtx1, Vertex *vtx2, int32_t val1, int32_t val2);
    static bool clipPolygon(Vertex *unclipped, Vertex *clipped, uint8_t *size);

    void processVertices();
    void addVertex();
    void addPolygon();

    void mtxModeCmd(MatrixState &m, uint32_t param);
    void mtxPushCmd(MatrixState &m, int slot);
    void mtxPopCmd(MatrixState &m, int slot);
    void mtxStoreCmd(MatrixState &m, int slot);
    void mtxRestoreCmd(MatrixState &m, int slot);
    void mtxIdentityCmd(MatrixState &m);
    void mtxLoad44Cmd(MatrixState &m, uint32_t *params);
    void mtxLoad43Cmd(MatrixState &m, uint32_t *params);
    void mtxMult44Cmd(MatrixState &m, uint32_t *params);
    void mtxMult43Cmd(MatrixState &m, uint32_t *params);
    void mtxMult33Cmd(MatrixState &m, uint32_t *params);
    void mtxScaleCmd(MatrixState &m, uint32_t *params);
    void mtxTransCmd(MatrixState &m, uint32_t *params);
    void colorCmd(uint32_t param);
    void normalCmd(uint32_t param);
    void texCoordCmd(uint32_t param);
    void vtx16Cmd(uint32_t *params);
    void vtx10Cmd(uint32_t param);
    void vtxXYCmd(uint32_t param);
    void vtxXZCmd(uint32_t param);
    void vtxYZCmd(uint32_t param);
    void vtxDiffCmd(uint32_t param);
    void polygonAttrCmd(uint32_t param);
    void texImageParamCmd(uint32_t param);
    void plttBaseCmd(uint32_t param);
    void difAmbCmd(uint32_t param);
    void speEmiCmd(uint32_t param);
    void lightVectorCmd(uint32_t param);
    void lightColorCmd(uint32_t param);
    void shininessCmd(uint32_t *params);
    void beginVtxsCmd(uint32_t param);
    void swapBuffersCmd(uint32_t param);
    void viewportCmd(uint32_t param);
    void boxTestCmd(uint32_t *params);
    void posTestCmd(uint32_t *params);
    void vecTestCmd(uint32_t param);

    void trackEntry(uint8_t command);
    void queueEntry(Entry entry);
    void finishEntries(size_t oldSize);
    void addEntry(Entry entry);
    void queueGxFifo(const uint8_t *data, int count);
};
