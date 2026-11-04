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

#include <algorithm>
#include <cstring>
#include "../core.h"
#include "../emu_profile.h"

Matrix Matrix::operator*(Matrix &mtx) {
    // Multiply 2 matrices
    Matrix result;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            result.data[y * 4 + x] = ((int64_t)data[y * 4 + 0] * mtx.data[0 + x] + (int64_t)data[y * 4 + 1] * mtx.data[4 + x] +
                (int64_t)data[y * 4 + 2] * mtx.data[8 + x] + (int64_t)data[y * 4 + 3] * mtx.data[12 + x]) >> 12;
    return result;
}

int32_t Vector::operator*(Vector &vtr) {
    // Multiply 2 vectors
    return ((int64_t)x * vtr.x + (int64_t)y * vtr.y + (int64_t)z * vtr.z) >> 12;
}

Vector Vector::operator*(Matrix &mtx) {
    // Multiply a vector with a matrix
    Vector result;
    result.x = ((int64_t)x * mtx.data[0] + (int64_t)y * mtx.data[4] + (int64_t)z * mtx.data[8]) >> 12;
    result.y = ((int64_t)x * mtx.data[1] + (int64_t)y * mtx.data[5] + (int64_t)z * mtx.data[9]) >> 12;
    result.z = ((int64_t)x * mtx.data[2] + (int64_t)y * mtx.data[6] + (int64_t)z * mtx.data[10]) >> 12;
    return result;
}

Vertex Vertex::operator*(Matrix &mtx) {
    // Multiply a vertex with a matrix
    Vertex result = *this;
    result.x = ((int64_t)x * mtx.data[0] + (int64_t)y * mtx.data[4] + (int64_t)z * mtx.data[8] + (int64_t)w * mtx.data[12]) >> 12;
    result.y = ((int64_t)x * mtx.data[1] + (int64_t)y * mtx.data[5] + (int64_t)z * mtx.data[9] + (int64_t)w * mtx.data[13]) >> 12;
    result.z = ((int64_t)x * mtx.data[2] + (int64_t)y * mtx.data[6] + (int64_t)z * mtx.data[10] + (int64_t)w * mtx.data[14]) >> 12;
    result.w = ((int64_t)x * mtx.data[3] + (int64_t)y * mtx.data[7] + (int64_t)z * mtx.data[11] + (int64_t)w * mtx.data[15]) >> 12;
    return result;
}

const uint8_t Gpu3D::paramCounts[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 0x00-0x0F
    1, 0, 1, 1, 1, 0, 16, 12, 16, 12, 9, 3, 3, 0, 0, 0, // 0x10-0x1F
    1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, // 0x20-0x2F
    1, 1, 1, 1, 32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 0x30-0x3F
    1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 0x40-0x4F
    1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 0x50-0x5F
    1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 0x60-0x6F
    3, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 0x70-0x7F
};

Gpu3D::~Gpu3D() {
    // Finish any commands on the worker before cleaning it up
    stopWorker();
}

void Gpu3D::saveState(FILE *file) {
    // Write state data to the file, after any queued commands have run
    sync();
    uint32_t stat = gxStat | gxStatExec;
    fwrite(&state, sizeof(state), 1, file);
    fwrite(&pipeSize, sizeof(pipeSize), 1, file);
    fwrite(&testQueue, sizeof(testQueue), 1, file);
    fwrite(&matrixQueue, sizeof(matrixQueue), 1, file);
    fwrite(&mtx.mode, sizeof(mtx.mode), 1, file);
    fwrite(&mtx.clipDirty, sizeof(mtx.clipDirty), 1, file);
    fwrite(&mtx.projection, sizeof(mtx.projection), 1, file);
    fwrite(&mtx.projectionStack, sizeof(mtx.projectionStack), 1, file);
    fwrite(&mtx.coordinate, sizeof(mtx.coordinate), 1, file);
    fwrite(mtx.coordinateStack, sizeof(Matrix), sizeof(mtx.coordinateStack) / sizeof(Matrix), file);
    fwrite(&mtx.direction, sizeof(mtx.direction), 1, file);
    fwrite(mtx.directionStack, sizeof(Matrix), sizeof(mtx.coordinateStack) / sizeof(Matrix), file);
    fwrite(&mtx.texture, sizeof(mtx.texture), 1, file);
    fwrite(&mtx.textureStack, sizeof(mtx.textureStack), 1, file);
    fwrite(&mtx.clip, sizeof(mtx.clip), 1, file);
    fwrite(verticesIn, sizeof(Vertex), sizeof(vertices1) / sizeof(Vertex), file);
    fwrite(verticesOut, sizeof(Vertex), sizeof(vertices2) / sizeof(Vertex), file);
    fwrite(&vertexCountIn, sizeof(vertexCountIn), 1, file);
    fwrite(&vertexCountOut, sizeof(vertexCountOut), 1, file);
    fwrite(&processCount, sizeof(processCount), 1, file);
    fwrite(polygonsIn, sizeof(_Polygon), sizeof(polygons1) / sizeof(_Polygon), file);
    fwrite(polygonsOut, sizeof(_Polygon), sizeof(polygons2) / sizeof(_Polygon), file);
    fwrite(&polygonCountIn, sizeof(polygonCountIn), 1, file);
    fwrite(&polygonCountOut, sizeof(polygonCountOut), 1, file);
    fwrite(&savedVertex, sizeof(savedVertex), 1, file);
    fwrite(&savedPolygon, sizeof(savedPolygon), 1, file);
    fwrite(&s, sizeof(s), 1, file);
    fwrite(&t, sizeof(t), 1, file);
    fwrite(&vertexCount, sizeof(vertexCount), 1, file);
    fwrite(&clockwise, sizeof(clockwise), 1, file);
    fwrite(&polygonType, sizeof(polygonType), 1, file);
    fwrite(&textureCoordMode, sizeof(textureCoordMode), 1, file);
    fwrite(&polygonAttr, sizeof(polygonAttr), 1, file);
    fwrite(&enabledLights, sizeof(enabledLights), 1, file);
    fwrite(&renderBack, sizeof(renderBack), 1, file);
    fwrite(&renderFront, sizeof(renderFront), 1, file);
    fwrite(&diffuseColor, sizeof(diffuseColor), 1, file);
    fwrite(&ambientColor, sizeof(ambientColor), 1, file);
    fwrite(&specularColor, sizeof(specularColor), 1, file);
    fwrite(&emissionColor, sizeof(emissionColor), 1, file);
    fwrite(&shininessEnabled, sizeof(shininessEnabled), 1, file);
    fwrite(lightVector, sizeof(Vector), sizeof(lightVector) / sizeof(Vector), file);
    fwrite(halfVector, sizeof(Vector), sizeof(halfVector) / sizeof(Vector), file);
    fwrite(lightColor, 4, sizeof(lightColor) / 4, file);
    fwrite(shininess, 1, sizeof(shininess), file);
    fwrite(viewport, 2, sizeof(viewport) / 2, file);
    fwrite(viewportNext, 2, sizeof(viewportNext) / 2, file);
    fwrite(&gxFifo, sizeof(gxFifo), 1, file);
    fwrite(&stat, sizeof(stat), 1, file);
    fwrite(posResult, 4, sizeof(posResult) / 4, file);
    fwrite(vecResult, 2, sizeof(vecResult) / 2, file);
    fwrite(&gxFifoCount, sizeof(gxFifoCount), 1, file);
    SaveStates::writeFifo(fifo, file);
}

void Gpu3D::loadState(FILE *file) {
    // Read state data from the file, after any queued commands have run
    sync();
    fread(&state, sizeof(state), 1, file);
    fread(&pipeSize, sizeof(pipeSize), 1, file);
    fread(&testQueue, sizeof(testQueue), 1, file);
    fread(&matrixQueue, sizeof(matrixQueue), 1, file);
    fread(&mtx.mode, sizeof(mtx.mode), 1, file);
    fread(&mtx.clipDirty, sizeof(mtx.clipDirty), 1, file);
    fread(&mtx.projection, sizeof(mtx.projection), 1, file);
    fread(&mtx.projectionStack, sizeof(mtx.projectionStack), 1, file);
    fread(&mtx.coordinate, sizeof(mtx.coordinate), 1, file);
    fread(mtx.coordinateStack, sizeof(Matrix), sizeof(mtx.coordinateStack) / sizeof(Matrix), file);
    fread(&mtx.direction, sizeof(mtx.direction), 1, file);
    fread(mtx.directionStack, sizeof(Matrix), sizeof(mtx.coordinateStack) / sizeof(Matrix), file);
    fread(&mtx.texture, sizeof(mtx.texture), 1, file);
    fread(&mtx.textureStack, sizeof(mtx.textureStack), 1, file);
    fread(&mtx.clip, sizeof(mtx.clip), 1, file);
    fread(vertices1, sizeof(Vertex), sizeof(vertices1) / sizeof(Vertex), file);
    fread(vertices2, sizeof(Vertex), sizeof(vertices2) / sizeof(Vertex), file);
    fread(&vertexCountIn, sizeof(vertexCountIn), 1, file);
    fread(&vertexCountOut, sizeof(vertexCountOut), 1, file);
    fread(&processCount, sizeof(processCount), 1, file);
    fread(polygons1, sizeof(_Polygon), sizeof(polygons1) / sizeof(_Polygon), file);
    fread(polygons2, sizeof(_Polygon), sizeof(polygons2) / sizeof(_Polygon), file);
    fread(&polygonCountIn, sizeof(polygonCountIn), 1, file);
    fread(&polygonCountOut, sizeof(polygonCountOut), 1, file);
    fread(&savedVertex, sizeof(savedVertex), 1, file);
    fread(&savedPolygon, sizeof(savedPolygon), 1, file);
    fread(&s, sizeof(s), 1, file);
    fread(&t, sizeof(t), 1, file);
    fread(&vertexCount, sizeof(vertexCount), 1, file);
    fread(&clockwise, sizeof(clockwise), 1, file);
    fread(&polygonType, sizeof(polygonType), 1, file);
    fread(&textureCoordMode, sizeof(textureCoordMode), 1, file);
    fread(&polygonAttr, sizeof(polygonAttr), 1, file);
    fread(&enabledLights, sizeof(enabledLights), 1, file);
    fread(&renderBack, sizeof(renderBack), 1, file);
    fread(&renderFront, sizeof(renderFront), 1, file);
    fread(&diffuseColor, sizeof(diffuseColor), 1, file);
    fread(&ambientColor, sizeof(ambientColor), 1, file);
    fread(&specularColor, sizeof(specularColor), 1, file);
    fread(&emissionColor, sizeof(emissionColor), 1, file);
    fread(&shininessEnabled, sizeof(shininessEnabled), 1, file);
    fread(lightVector, sizeof(Vector), sizeof(lightVector) / sizeof(Vector), file);
    fread(halfVector, sizeof(Vector), sizeof(halfVector) / sizeof(Vector), file);
    fread(lightColor, 4, sizeof(lightColor) / 4, file);
    fread(shininess, 1, sizeof(shininess), file);
    fread(viewport, 2, sizeof(viewport) / 2, file);
    fread(viewportNext, 2, sizeof(viewportNext) / 2, file);
    fread(&gxFifo, sizeof(gxFifo), 1, file);
    fread(&gxStat, sizeof(gxStat), 1, file);
    gxStatExec = gxStat & EXEC_STAT_BITS;
    gxStat &= ~EXEC_STAT_BITS;
    fifoMtx = mtx;
    fread(posResult, 4, sizeof(posResult) / 4, file);
    fread(vecResult, 2, sizeof(vecResult) / 2, file);
    fread(&gxFifoCount, sizeof(gxFifoCount), 1, file);
    SaveStates::readFifo(fifo, file);

    // Reset vertex and polygon buffers
    verticesIn = vertices1;
    verticesOut = vertices2;
    polygonsIn = polygons1;
    polygonsOut = polygons2;
}

uint32_t Gpu3D::rgb5ToRgb6(uint16_t color) {
    // Convert an RGB5 value to an RGB6 value (the way the 3D engine does it)
    uint8_t r = ((color >> 0) & 0x1F) * 2; if (r > 0) r++;
    uint8_t g = ((color >> 5) & 0x1F) * 2; if (g > 0) g++;
    uint8_t b = ((color >> 10) & 0x1F) * 2; if (b > 0) b++;
    return (b << 12) | (g << 6) | r;
}

Vertex Gpu3D::intersection(Vertex *vtx1, Vertex *vtx2, int32_t val1, int32_t val2) {
    // Calculate the interpolation coefficients
    int64_t d1 = val1 + vtx1->w;
    int64_t d2 = val2 + vtx2->w;
    if (d2 == d1) return *vtx1;

    // Interpolate the vertex coordinates
    Vertex vertex;
    vertex.x = ((d2 * vtx1->x) - (d1 * vtx2->x)) / (d2 - d1);
    vertex.y = ((d2 * vtx1->y) - (d1 * vtx2->y)) / (d2 - d1);
    vertex.z = ((d2 * vtx1->z) - (d1 * vtx2->z)) / (d2 - d1);
    vertex.w = ((d2 * vtx1->w) - (d1 * vtx2->w)) / (d2 - d1);
    vertex.s = ((d2 * vtx1->s) - (d1 * vtx2->s)) / (d2 - d1);
    vertex.t = ((d2 * vtx1->t) - (d1 * vtx2->t)) / (d2 - d1);

    // Interpolate the vertex color
    uint8_t r = ((d2 * ((vtx1->color >> 0) & 0x3F)) - (d1 * ((vtx2->color >> 0) & 0x3F))) / (d2 - d1);
    uint8_t g = ((d2 * ((vtx1->color >> 6) & 0x3F)) - (d1 * ((vtx2->color >> 6) & 0x3F))) / (d2 - d1);
    uint8_t b = ((d2 * ((vtx1->color >> 12) & 0x3F)) - (d1 * ((vtx2->color >> 12) & 0x3F))) / (d2 - d1);
    vertex.color = (vtx1->color & 0xFC0000) | (b << 12) | (g << 6) | r;
    return vertex;
}

bool Gpu3D::clipPolygon(Vertex *unclipped, Vertex *clipped, uint8_t *size) {
    // Clip a polygon using the Sutherland-Hodgman algorithm, alternating between two vertex buffers
    // The extra buffer is left uninitialized, since clearing it for every polygon is costly
    bool clip = false;
    alignas(Vertex) uint8_t bufferData[10 * sizeof(Vertex)];
    Vertex *buffer = (Vertex*)bufferData;
    Vertex *vertices = unclipped;
    for (int i = 0; i < 6; i++) {
        // Choose which coordinate to check based on the current side being clipped against
        int axis = i >> 1;
        bool negate = i & 1;
        auto value = [&](Vertex *v) -> int32_t {
            int32_t val = (axis == 0) ? v->x : (axis == 1) ? v->y : v->z;
            return negate ? -val : val;
        };

        // Skip sides that all vertices are in bounds of, since clipping against them changes nothing
        // If all vertices are out of bounds, nothing is left of the polygon
        int inside = 0;
        for (int j = 0; j < *size; j++)
            inside += (value(&vertices[j]) >= -vertices[j].w);
        if (inside == *size) continue;
        if (inside == 0) {
            *size = 0;
            return clip;
        }

        // Clip into whichever buffer isn't holding the current vertices
        Vertex *output = (vertices == clipped) ? buffer : clipped;
        int oldSize = *size;
        *size = 0;
        for (int j = 0; j < oldSize; j++) {
            // Get the unclipped vertices
            Vertex *current = &vertices[j];
            Vertex *previous = &vertices[(j - 1 + oldSize) % oldSize];
            int32_t currentVal = value(current), previousVal = value(previous);

            // Add the clipped vertices
            if (currentVal >= -current->w) { // Current vertex in bounds
                if (previousVal < -previous->w) // Previous vertex not in bounds
                    output[(*size)++] = intersection(current, previous, currentVal, previousVal);
                output[(*size)++] = *current;
            }
            else if (previousVal >= -previous->w) { // Previous vertex in bounds
                output[(*size)++] = intersection(current, previous, currentVal, previousVal);
            }
        }
        vertices = output;
        clip = true;
    }

    // Make sure the final vertices end up in the clipped buffer
    if (vertices == buffer)
        memcpy(clipped, buffer, *size * sizeof(Vertex));
    return clip;
}

static bool changesExecStat(uint8_t command) {
    // Check if a command changes the GXSTAT bits that are set by execution, which is only the box test result
    return command == 0x70;
}

void Gpu3D::startWorker() {
    // Start executing commands on a worker thread, above the 3D renderer's priority so it doesn't fall behind
    if (!queue) {
        // Align the queue to cache blocks, so they can be claimed before being written
        queueAlloc = new uint32_t[QUEUE_SIZE + 8];
        queue = (uint32_t*)(((uintptr_t)queueAlloc + 31) & ~31);
    }
    fifoMtx = mtx;
    queueEnd = queueStart = 0;
    queueHead.store(0);
    queueTail.store(0);
    statDone.store(statSent);
    workerSleeping.store(false);
    workerStop.store(false);
    // Keep it on core 0 where threads can be placed, where it outranks the 2D thread, since core 2 also has the frontend
    // and a hardware 3D renderer at the same priority, which can hold up the emulator waiting on it
    worker = new WorkerThread(WorkerThread::CORE_0, &Gpu3D::runWorker, this);
#ifndef GEOM_WORKER_PRIORITY
#define GEOM_WORKER_PRIORITY 15
#endif
    worker->setPriority(GEOM_WORKER_PRIORITY);
}

void Gpu3D::stopWorker() {
    // Let the worker finish its commands and then stop it
    if (!worker) return;
    sync();
    workerStop.store(true);
    {
        std::lock_guard<std::mutex> guard(workerMutex);
        workerCond.notify_one();
    }
    worker->join();
    delete worker;
    worker = nullptr;
    delete[] queueAlloc;
    queueAlloc = queue = nullptr;
}

void Gpu3D::runWorker() {
    HB_SET_CORE(HB_GEOM_CORE);
    uint32_t head = queueHead.load(), tail = head, stat = statDone.load(), published = head;
    while (true) {
        if (head == tail && (tail = queueTail.load()) == head) {
            // Publish progress once caught up, so anything waiting on the worker can continue
            statDone.store(stat);
            queueHead.store(head);
            published = head;

            // Sleep until more commands are published; it doesn't spin, since it would hold up the 3D renderer
            std::unique_lock<std::mutex> lock(workerMutex);
            workerSleeping.store(true);
            while ((tail = queueTail.load()) == head) {
                if (workerStop.load()) return;
                workerCond.wait(lock);
            }
            workerSleeping.store(false);
        }

        // Execute the next command
        uint32_t header = queue[head & (QUEUE_SIZE - 1)];
        uint8_t command = header;
        int count = header >> 8;
        uint32_t params[32];
        for (int i = 0; i < count; i++)
            params[i] = queue[(head + 1 + i) & (QUEUE_SIZE - 1)];
        execute(command, params);
        if (changesExecStat(command)) stat++;
        head += count + 1;

        // Also publish progress regularly, so the emulator thread can reuse space if the queue fills up
        if (head - published >= 0x400) {
            queueHead.store(head);
            published = head;
        }
    }
}

void Gpu3D::queueCommand(uint8_t command, uint32_t *params, int count) {
    // Wait for room in the queue, which only happens if the worker falls far behind
    // A block's worth of extra room is kept, since whole cache blocks are claimed when they start being written
    SpinWait spin;
    while (queueEnd + count + 1 + 8 - queueStart > QUEUE_SIZE) {
        publishQueue(true);
        spin.pause();
        queueStart = queueHead.load();
    }

    // Add the command and its parameters; they're published at the end of the batch
    // Each cache block is claimed instead of being read from memory, since everything in it is new
    uint32_t *words = queue, end = queueEnd;
    uint32_t index = end & (QUEUE_SIZE - 1);
    if (!(index & 7)) claimBlock(&words[index]);
    words[index] = command | (count << 8);
    for (int i = 0; i < count; i++) {
        index = (end + 1 + i) & (QUEUE_SIZE - 1);
        if (!(index & 7)) claimBlock(&words[index]);
        words[index] = params[i];
    }
    queueEnd = end + count + 1;
    if (changesExecStat(command)) statSent++;
}

void Gpu3D::publishQueue(bool force) {
    // Let the worker see newly queued commands, waking it if it's sleeping
    // A sleeping worker is only woken once enough commands build up, unless something needs their results
    // The sleeping flag is checked again after publishing, since the worker might have just gone to sleep
    uint32_t tail = queueTail.load(std::memory_order_relaxed);
    if (tail == queueEnd || (!force && queueEnd - tail < 0x800 && workerSleeping.load())) return;
    queueTail.store(queueEnd);
    if (workerSleeping.load()) {
        std::lock_guard<std::mutex> guard(workerMutex);
        workerCond.notify_one();
    }
}

void Gpu3D::sync() {
    // Wait for the worker to execute all queued commands, before something reads their results
    if (!worker) return;
    publishQueue(true);
    if ((queueStart = queueHead.load()) == queueEnd) return;
    EP_START(wait);
    SpinWait spin;
    while ((queueStart = queueHead.load()) != queueEnd)
        spin.pause();
    EP_ADD(EP_GEOM_SYNC, wait);
}

void Gpu3D::syncStat() {
    // Wait for the worker to execute any queued commands that change the GXSTAT bits set by execution
    if (!worker || statDone.load() == statSent) return;
    publishQueue(true);
    EP_START(wait);
    SpinWait spin;
    while (statDone.load() != statSent)
        spin.pause();
    EP_ADD(EP_GEOM_SYNC, wait);
}

int Gpu3D::stackSlot(uint8_t mode, uint8_t command, uint32_t param) {
    // Update GXSTAT's matrix stack bits for a stack command, and get the stack slot it uses, or -1 if none
    switch (command) {
    case 0x11: // MTX_PUSH
        switch (mode) {
        case 0: // Projection stack
            // Push to the single projection stack slot and increment the pointer, or indicate an overflow error
            if (gxStat & BIT(13)) {
                gxStat |= BIT(15);
                return -1;
            }
            gxStat |= BIT(13);
            return 0;

        case 1: case 2: { // Coordinate and directional stacks
            // Indicate an overflow error; even though the 31st slot exists, it still causes one
            uint8_t pointer = (gxStat >> 8) & 0x1F;
            if (pointer >= 30)
                gxStat |= BIT(15);

            // Push to the current slot and increment the pointer
            if (pointer >= 31) return -1;
            gxStat += BIT(8);
            return pointer;
        }

        default: // Texture stack
            return 0;
        }

    case 0x12: // MTX_POP
        switch (mode) {
        case 0: // Projection stack
            // Pop from the single projection stack slot and decrement the pointer, or indicate an underflow error
            if (!(gxStat & BIT(13))) {
                gxStat |= BIT(15);
                return -1;
            }
            gxStat &= ~BIT(13);
            return 0;

        case 1: case 2: { // Coordinate and directional stacks
            // Indicate an underflow or overflow error; even though the 31st slot exists, it still causes one
            uint8_t pointer = ((gxStat >> 8) & 0x1F) - ((int8_t)(param << 2) >> 2);
            if (pointer >= 30)
                gxStat |= BIT(15);

            // Pop from the new slot and update the pointer
            if (pointer >= 31) return -1;
            gxStat = (gxStat & ~0x1F00) | (pointer << 8);
            return pointer;
        }

        default: // Texture stack
            return 0;
        }

    default: // MTX_STORE, MTX_RESTORE
        if (mode == 1 || mode == 2) {
            // Indicate an overflow error for the 31st slot, which still exists
            int address = param & 0x1F;
            if (address == 31) gxStat |= BIT(15);
            return address;
        }
        return 0;
    }
}

int Gpu3D::takeCommand(uint8_t command, uint32_t *params, int count) {
    // Update the FIFO's view of a command as it's taken for execution, and return how many parameters to pass on
    switch (command) {
    case 0x11: case 0x12: // MTX_PUSH, MTX_POP
        // Clear the busy bit if no more matrix commands are queued, and then fall through
        if (--matrixQueue == 0)
            gxStat &= ~BIT(14);

    case 0x13: case 0x14: // MTX_STORE, MTX_RESTORE
        // Pass the stack slot as an extra parameter, since the FIFO keeps track of the stack pointers
        params[1] = stackSlot(worker ? fifoMtx.mode : mtx.mode, command, params[0]);
        count = 2;
        break;

    case 0x50: // SWAP_BUFFERS
        // Halt the geometry engine; the buffers will be swapped and the engine unhalted on next V-blank
        state = GX_HALTED;
        break;

    case 0x70: case 0x71: case 0x72: // BOX_TEST, POS_TEST, VEC_TEST
        // Clear the busy bit if no more test commands are queued
        if ((testQueue -= paramCounts[command]) == 0)
            gxStat &= ~BIT(0);
        break;
    }

    // With a worker, also apply matrix commands to a copy of the matrices, so they can be read without waiting
    if (worker) {
        switch (command) {
            case 0x10: mtxModeCmd(fifoMtx, params[0]); break; // MTX_MODE
            case 0x11: mtxPushCmd(fifoMtx, params[1]); break; // MTX_PUSH
            case 0x12: mtxPopCmd(fifoMtx, params[1]); break; // MTX_POP
            case 0x13: mtxStoreCmd(fifoMtx, params[1]); break; // MTX_STORE
            case 0x14: mtxRestoreCmd(fifoMtx, params[1]); break; // MTX_RESTORE
            case 0x15: mtxIdentityCmd(fifoMtx); break; // MTX_IDENTITY
            case 0x16: mtxLoad44Cmd(fifoMtx, params); break; // MTX_LOAD_4x4
            case 0x17: mtxLoad43Cmd(fifoMtx, params); break; // MTX_LOAD_4x3
            case 0x18: mtxMult44Cmd(fifoMtx, params); break; // MTX_MULT_4x4
            case 0x19: mtxMult43Cmd(fifoMtx, params); break; // MTX_MULT_4x3
            case 0x1A: mtxMult33Cmd(fifoMtx, params); break; // MTX_MULT_3x3
            case 0x1B: mtxScaleCmd(fifoMtx, params); break; // MTX_SCALE
            case 0x1C: mtxTransCmd(fifoMtx, params); break; // MTX_TRANS
        }
    }
    return count;
}

void Gpu3D::execute(uint8_t command, uint32_t *params) {
    // Execute a geometry command
    EP_START_EMU(cmdStart);
    switch (command) {
        case 0x10: mtxModeCmd(mtx, params[0]); break; // MTX_MODE
        case 0x11: mtxPushCmd(mtx, params[1]); break; // MTX_PUSH
        case 0x12: mtxPopCmd(mtx, params[1]); break; // MTX_POP
        case 0x13: mtxStoreCmd(mtx, params[1]); break; // MTX_STORE
        case 0x14: mtxRestoreCmd(mtx, params[1]); break; // MTX_RESTORE
        case 0x15: mtxIdentityCmd(mtx); break; // MTX_IDENTITY
        case 0x16: mtxLoad44Cmd(mtx, params); break; // MTX_LOAD_4x4
        case 0x17: mtxLoad43Cmd(mtx, params); break; // MTX_LOAD_4x3
        case 0x18: mtxMult44Cmd(mtx, params); break; // MTX_MULT_4x4
        case 0x19: mtxMult43Cmd(mtx, params); break; // MTX_MULT_4x3
        case 0x1A: mtxMult33Cmd(mtx, params); break; // MTX_MULT_3x3
        case 0x1B: mtxScaleCmd(mtx, params); break; // MTX_SCALE
        case 0x1C: mtxTransCmd(mtx, params); break; // MTX_TRANS
        case 0x20: colorCmd(params[0]); break; // COLOR
        case 0x21: normalCmd(params[0]); break; // NORMAL
        case 0x22: texCoordCmd(params[0]); break; // TEXCOORD
        case 0x23: vtx16Cmd(params); break; // VTX_16
        case 0x24: vtx10Cmd(params[0]); break; // VTX_10
        case 0x25: vtxXYCmd(params[0]); break; // VTX_XY
        case 0x26: vtxXZCmd(params[0]); break; // VTX_XZ
        case 0x27: vtxYZCmd(params[0]); break; // VTX_YZ
        case 0x28: vtxDiffCmd(params[0]); break; // VTX_DIFF
        case 0x29: polygonAttrCmd(params[0]); break; // POLYGON_ATTR
        case 0x2A: texImageParamCmd(params[0]); break; // TEXIMAGE_PARAM
        case 0x2B: plttBaseCmd(params[0]); break; // PLTT_BASE
        case 0x30: difAmbCmd(params[0]); break; // DIF_AMB
        case 0x31: speEmiCmd(params[0]); break; // SPE_EMI
        case 0x32: lightVectorCmd(params[0]); break; // LIGHT_VECTOR
        case 0x33: lightColorCmd(params[0]); break; // LIGHT_COLOR
        case 0x34: shininessCmd(params); break; // SHININESS
        case 0x40: beginVtxsCmd(params[0]); break; // BEGIN_VTXS
        case 0x41: break; // END_VTXS
        case 0x50: swapBuffersCmd(params[0]); break; // SWAP_BUFFERS
        case 0x60: viewportCmd(params[0]); break; // VIEWPORT
        case 0x70: boxTestCmd(params); break; // BOX_TEST
        case 0x71: posTestCmd(params); break; // POS_TEST
        case 0x72: vecTestCmd(params[0]); break; // VEC_TEST

    default:
        LOG_CRIT("Unknown GXFIFO command: 0x%X\n", command);
        break;
    }
    EP_ADD_EMU((command >= 0x23 && command <= 0x28) ? EP_GEOM_VTX : (command == 0x22) ? EP_GEOM_TEX :
        (command < 0x20) ? EP_GEOM_MTX : EP_GEOM_OTHER, cmdStart);
}

void Gpu3D::runCommands() {
#ifdef PROFILE_EMU
    EP_START(geom);
    struct GeomTimer { uint32_t start; ~GeomTimer() { EP_ADD(EP_GEOM, start); } } geomTimer = { geom };
#endif
    // Execute commands on a worker thread if threaded 3D is enabled
    if (!worker != !Settings::worker3D())
        worker ? stopWorker() : startWorker();

    // Run a batch of geometry commands
    // The FIFO's state is kept in local variables, since otherwise it's read again after every store
    Entry *entries = fifo.entries;
    size_t mask = fifo.mask, head = fifo.head, size = fifo.count;
    uint32_t pipe = pipeSize, cycles = 0;
    while (cycles < GPU3D_BATCH) {
        // Fetch the next geometry command
        Entry entry = entries[head];
        int count = paramCounts[entry.command];
        uint32_t params[32];

        // Fetch the command's parameters; commands without any still take up an entry
        if (count > 1) {
            for (int i = 0; i < count; i++)
                params[i] = entries[(head + i) & mask].param;
        }
        else {
            count = 1;
            params[0] = entry.param;
        }
        head = (head + count) & mask;
        size -= count;

        // Apply the parts of the command that the FIFO handles, and then execute it, on the worker if running
        // Most commands don't involve the FIFO's state, so they skip checking for the ones that do
        int passed = (entry.command >= 0x20 && entry.command < 0x50) ? count : takeCommand(entry.command, params, count);
        if (worker) queueCommand(entry.command, params, passed);
        else execute(entry.command, params);

        // On hardware, FIFO entries are moved into a pipe before being executed
        // The pipe can hold 4 entries, and is refilled when it runs half empty (2 entries)
        // As long as there are enough entries, set the pipe size to 3 or 4 depending on when it would refill
        pipe = 4 - ((pipe + count) & 1);
        if (pipe > size) pipe = size;

        // End a batch early if the engine halted or there aren't enough parameters
        cycles += 2;
        if (state == GX_HALTED || size == 0 || size < paramCounts[entries[head].command])
            break;
    }
    fifo.head = head;
    fifo.count = size;
    pipeSize = pipe;

    // Let the worker see the batch's commands
    if (worker) publishQueue(false);

    // Update the FIFO status
    gxStat = (gxStat & ~0x01FF0000) | ((fifo.size() - pipeSize) << 16); // FIFO entries
    if (fifo.size() - pipeSize == 0) gxStat |= BIT(26); // FIFO empty
    if (fifo.size() == 0) gxStat &= ~BIT(27); // Commands not executing

    // If the FIFO becomes less than half full, trigger GXFIFO DMA transfers
    // If the FIFO is already less than half full when a DMA starts, it will automatically activate
    if (fifo.size() - pipeSize < 128 && !(gxStat & BIT(25))) {
        gxStat |= BIT(25);
        core->dma[0].trigger(7);
    }

    // Send a GXFIFO interrupt if enabled
    checkFifoIrq();

    // Unhalt the CPU if the FIFO was full but now has space free
    if (fifo.size() - pipeSize <= 256)
        core->interpreter[0].unhalt(1);

    // Keep executing commands as long as they're ready
    if (state != GX_HALTED) {
        if (!fifo.empty() && fifo.size() >= paramCounts[fifo.front().command])
            core->schedule(GPU3D_COMMANDS, cycles);
        else
            state = GX_IDLE;
    }
}

void Gpu3D::processVertices() {
    // Scale the viewport based on the high-res 3D setting
    bool resShift = Settings::highRes3D;
    uint16_t x = viewport[0] << resShift;
    uint16_t y = viewport[1] << resShift;
    uint16_t w = viewport[2] << resShift;
    uint16_t h = viewport[3] << resShift;
    uint16_t xMask = (0x200 << resShift) - 1;
    uint16_t yMask = (0x100 << resShift) - 1;

    // Normalize and scale new vertices to the viewport
    // X coordinates are 9-bit and Y coordinates are 8-bit; invalid viewports can cause wraparound
    // Z coordinates (and depth values in general) are 24-bit
    for (int i = processCount; i < vertexCountIn; i++) {
        if (verticesIn[i].w != 0) {
            verticesIn[i].x = (( (int64_t)verticesIn[i].x + verticesIn[i].w) * w / (verticesIn[i].w * 2) + x) & xMask;
            verticesIn[i].y = ((-(int64_t)verticesIn[i].y + verticesIn[i].w) * h / (verticesIn[i].w * 2) + y) & yMask;
            verticesIn[i].z = (((((int64_t)verticesIn[i].z << 14) / verticesIn[i].w) + 0x3FFF) << 9);
        }
    }

    // Count vertices as processed and update the viewport
    processCount = vertexCountIn;
    memcpy(viewport, viewportNext, sizeof(viewport));
}

void Gpu3D::swapBuffers() {
    // The hardware renderer may still read the output geometry even if the 2D engine didn't use its scanlines
    // Finish it before the swap makes those buffers available for new geometry and V-blank uploads
    core->gpu3DRenderer.finishHardwareFrame();
    // Process final vertices and reset the count, after any queued commands have run
    sync();
    processVertices();
    processCount = 0;

    // Determine each polygon's W-shift value to be used for reducing (or expanding) W values to 16 bits
    for (int i = 0; i < polygonCountIn; i++) {
        _Polygon *p = &polygonsIn[i];

        // Find the polygon's greatest W value
        uint32_t value = verticesIn[p->vertices].w;
        for (int j = 1; j < p->size; j++)
            if ((uint32_t)verticesIn[p->vertices + j].w > value)
                value = verticesIn[p->vertices + j].w;

        // Reduce precision in 4-bit increments until the value fits in a 16-bit range
        while ((value >> p->wShift) > 0xFFFF)
            p->wShift += 4;

        // If precision wasn't reduced, increase it until the value doesn't fit in a 16-bit range
        if (p->wShift == 0 && value != 0)
            while ((value << -(p->wShift - 4)) <= 0xFFFF)
                p->wShift -= 4;
    }

    // Swap the vertex buffers
    SWAP(verticesOut, verticesIn);
    vertexCountOut = vertexCountIn;
    vertexCountIn = 0;
    vertexCount = 0;

    // Swap the polygon buffers
    SWAP(polygonsOut, polygonsIn);
    polygonCountOut = polygonCountIn;
    polygonCountIn = 0;

    // Invalidate the 3D so a new frame is drawn
    core->gpu.invalidate3D();

    // Unhalt the GXFIFO, and start executing commands if one is ready
    if (!fifo.empty() && fifo.size() >= paramCounts[fifo.front().command]) {
        core->schedule(GPU3D_COMMANDS, 2);
        state = GX_RUNNING;
    }
    else {
        state = GX_IDLE;
    }
}

void Gpu3D::addVertex() {
    // Set the new vertex
    if (vertexCountIn >= 6144) return;
    verticesIn[vertexCountIn] = savedVertex;
    verticesIn[vertexCountIn].w = 1 << 12;

    // Transform the texture coordinates
    if (textureCoordMode == 3) {
        // Get the texture matrix with the texture coordinates
        Matrix matrix = mtx.texture;
        matrix.data[12] = (int32_t)s << 12;
        matrix.data[13] = (int32_t)t << 12;

        // Multiply the vertex with the texture matrix
        Vertex vertex = verticesIn[vertexCountIn] * matrix;

        // Save the transformed coordinates
        verticesIn[vertexCountIn].s = vertex.x >> 12;
        verticesIn[vertexCountIn].t = vertex.y >> 12;
    }

    // Update the clip matrix if necessary
    mtx.updateClip();

    // Transform the vertex
    verticesIn[vertexCountIn] = verticesIn[vertexCountIn] * mtx.clip;

    // Move to the next vertex
    vertexCountIn++;
    vertexCount++;

    // Move to the next polygon if one has been completed
    switch (polygonType) {
        case 0: if (vertexCount % 3 == 0) addPolygon(); break; // Separate triangles
        case 1: if (vertexCount % 4 == 0) addPolygon(); break; // Separate quads
        case 2: if (vertexCount >= 3) addPolygon(); break; // Triangle strips
        case 3: if (vertexCount >= 4 && vertexCount % 2 == 0) addPolygon(); break; // Quad strips
    }
}

void Gpu3D::addPolygon() {
    // Set the polygon vertex information
    if (polygonCountIn >= 2048) return;
    int size = 3 + (polygonType & 1);
    savedPolygon.size = size;
    savedPolygon.vertices = vertexCountIn - size;

    // Save a copy of the unclipped vertices
    // Temporary vertices are left uninitialized, since clearing them for every polygon is costly
    alignas(Vertex) uint8_t unclippedData[4 * sizeof(Vertex)], clippedData[10 * sizeof(Vertex)];
    Vertex *unclipped = (Vertex*)unclippedData, *clipped = (Vertex*)clippedData;
    memcpy(unclipped, &verticesIn[savedPolygon.vertices], size * sizeof(Vertex));

    // Rearrange quad strip vertices to be counter-clockwise
    if (polygonType == 3)
        SWAP(unclipped[2], unclipped[3]);

    // Get the components of the second and third vertices, relative to the first
    int64_t x1 = unclipped[1].x - unclipped[0].x;
    int64_t y1 = unclipped[1].y - unclipped[0].y;
    int64_t w1 = unclipped[1].w - unclipped[0].w;
    int64_t x2 = unclipped[2].x - unclipped[0].x;
    int64_t y2 = unclipped[2].y - unclipped[0].y;
    int64_t w2 = unclipped[2].w - unclipped[0].w;

    // Calculate the 3D cross product of the vertices
    int64_t xc = y1 * w2 - w1 * y2;
    int64_t yc = w1 * x2 - x1 * w2;
    int64_t wc = x1 * y2 - y1 * x2;

    // Reduce the result to 32 bits to avoid overflow
    while (xc != (int32_t)xc || yc != (int32_t)yc || wc != (int32_t)wc) {
        xc >>= 4;
        yc >>= 4;
        wc >>= 4;
    }

    // Calculate the dot product of the cross product and the first vertex to determine 2D orientation
    int64_t dot = xc * unclipped[0].x + yc * unclipped[0].y + wc * unclipped[0].w;

    // Separate from vertex store order, the renderer needs to know the raw polygon orientation to calculate edges
    savedPolygon.clockwise = (dot < 0);

    // Every other triangle strip polygon is stored clockwise instead of counter-clockwise
    // Keep track of this, and reverse the dot product of clockwise polygons to accomodate
    if (polygonType == 2) {
        if (clockwise) dot = -dot;
        clockwise = !clockwise;
    }

    // Check if the polygon should be culled, and clip it if not
    bool cull = (!renderFront && dot > 0) || (!renderBack && dot < 0);
    EP_START_EMU(clipStart);
    bool clip = cull ? false : clipPolygon(unclipped, clipped, &savedPolygon.size);
    EP_ADD_EMU(EP_GEOM_CLIP, clipStart);

    // Discard polygons that should be culled or are outside of the view area
    if (cull || savedPolygon.size == 0) {
        switch (polygonType) {
        case 0: case 1: // Separate polygons
            // Discard the vertices
            vertexCountIn -= size;
            return;

        case 2: // Triangle strips
            if (vertexCount == 3) { // First triangle in the strip
                // Discard the first vertex, but keep the other 2 for the next triangle
                verticesIn[vertexCountIn - 3] = verticesIn[vertexCountIn - 2];
                verticesIn[vertexCountIn - 2] = verticesIn[vertexCountIn - 1];
                vertexCountIn--;
                vertexCount--;
            }
            else if (vertexCountIn < 6144) {
                // End the previous strip, and start a new one with the last 2 vertices
                verticesIn[vertexCountIn - 0] = verticesIn[vertexCountIn - 1];
                verticesIn[vertexCountIn - 1] = verticesIn[vertexCountIn - 2];
                vertexCountIn++;
                vertexCount = 2;
            }
            return;

        case 3: // Quad strips
            if (vertexCount == 4) { // First quad in the strip
                // Discard the first 2 vertices, but keep the other 2 for the next quad
                verticesIn[vertexCountIn - 4] = verticesIn[vertexCountIn - 2];
                verticesIn[vertexCountIn - 3] = verticesIn[vertexCountIn - 1];
                vertexCountIn -= 2;
                vertexCount -= 2;
            }
            else {
                // End the previous strip, and start a new one with the last 2 vertices
                vertexCount = 2;
            }
            return;
        }
    }

    // Update the vertices of clipped polygons
    if (clip) {
        switch (polygonType) {
        case 0: case 1: // Separate polygons
            // Remove the unclipped vertices
            vertexCountIn -= size;

            // Add the clipped vertices
            for (int i = 0; i < savedPolygon.size; i++) {
                if (vertexCountIn >= 6144) return;
                verticesIn[vertexCountIn] = clipped[i];
                vertexCountIn++;
            }
            break;

        case 2: // Triangle strips
            // Remove the unclipped vertices
            vertexCountIn -= (vertexCount == 3) ? 3 : 1;
            savedPolygon.vertices = vertexCountIn;

            // Add the clipped vertices
            for (int i = 0; i < savedPolygon.size; i++) {
                if (vertexCountIn >= 6144) return;
                verticesIn[vertexCountIn] = clipped[i];
                vertexCountIn++;
            }

            // End the previous strip, and start a new one with the last 2 vertices
            for (int i = 0; i < 2; i++) {
                if (vertexCountIn >= 6144) return;
                verticesIn[vertexCountIn] = unclipped[1 + i];
                vertexCountIn++;
            }
            vertexCount = 2;
            break;

        case 3: // Quad strips
            // Remove the unclipped vertices
            vertexCountIn -= (vertexCount == 4) ? 4 : 2;
            savedPolygon.vertices = vertexCountIn;

            // Add the clipped vertices
            for (int i = 0; i < savedPolygon.size; i++) {
                if (vertexCountIn >= 6144) return;
                verticesIn[vertexCountIn] = clipped[i];
                vertexCountIn++;
            }

            // End the previous strip, and start a new one with the last 2 vertices
            for (int i = 0; i < 2; i++) {
                if (vertexCountIn >= 6144) return;
                verticesIn[vertexCountIn] = unclipped[3 - i];
                vertexCountIn++;
            }
            vertexCount = 2;
            break;
        }
    }

    // Set the new polygon
    polygonsIn[polygonCountIn] = savedPolygon;
    polygonsIn[polygonCountIn].crossed = (polygonType == 3 && !clip);
    polygonsIn[polygonCountIn].paletteAddr <<= 4 - (savedPolygon.textureFmt == 2);

    // Move to the next polygon
    polygonCountIn++;
}

void Gpu3D::mtxModeCmd(MatrixState &m, uint32_t param) {
    // Set the matrix mode
    m.mode = param & 0x00000003;
}

void Gpu3D::mtxPushCmd(MatrixState &m, int slot) {
    // Push the current matrix onto a stack, if the FIFO found room
    switch (m.mode) {
    case 0: // Projection stack
        if (slot >= 0) m.projectionStack = m.projection;
        break;

    case 1: case 2: // Coordinate and directional stacks
        if (slot >= 0) {
            m.coordinateStack[slot] = m.coordinate;
            m.directionStack[slot] = m.direction;
        }
        break;

    case 3: // Texture stack
        m.textureStack = m.texture;
        break;
    }
}

void Gpu3D::mtxPopCmd(MatrixState &m, int slot) {
    // Pop a matrix from a stack, if the FIFO found one
    switch (m.mode) {
    case 0: // Projection stack
        if (slot >= 0) {
            m.projection = m.projectionStack;
            m.clipDirty = true;
        }
        break;

    case 1: case 2: // Coordinate and directional stacks
        if (slot >= 0) {
            m.coordinate = m.coordinateStack[slot];
            m.direction = m.directionStack[slot];
            m.clipDirty = true;
        }
        break;

    case 3: // Texture stack
        m.texture = m.textureStack;
        break;
    }
}

void Gpu3D::mtxStoreCmd(MatrixState &m, int slot) {
    // Store a matrix to the stack
    switch (m.mode) {
    case 0: // Projection stack
        m.projectionStack = m.projection;
        break;

    case 1: case 2: // Coordinate and directional stacks
        m.coordinateStack[slot] = m.coordinate;
        m.directionStack[slot] = m.direction;
        break;

    case 3: // Texture stack
        m.textureStack = m.texture;
        break;
    }
}

void Gpu3D::mtxRestoreCmd(MatrixState &m, int slot) {
    // Restore a matrix from the stack
    switch (m.mode) {
    case 0: // Projection stack
        m.projection = m.projectionStack;
        m.clipDirty = true;
        break;

    case 1: case 2: // Coordinate and directional stacks
        m.coordinate = m.coordinateStack[slot];
        m.direction = m.directionStack[slot];
        m.clipDirty = true;
        break;

    case 3: // Texture stack
        m.texture = m.textureStack;
        break;
    }
}

void Gpu3D::mtxIdentityCmd(MatrixState &m) {
    // Set a matrix to the identity matrix
    switch (m.mode) {
    case 0: // Projection stack
        m.projection = Matrix();
        m.clipDirty = true;
        break;

    case 1: // Coordinate stack
        m.coordinate = Matrix();
        m.clipDirty = true;
        break;

    case 2: // Coordinate and directional stacks
        m.coordinate = Matrix();
        m.direction = Matrix();
        m.clipDirty = true;
        break;

    case 3: // Texture stack
        m.texture = Matrix();
        break;
    }
}

void Gpu3D::mtxLoad44Cmd(MatrixState &m, uint32_t *params) {
    // Convert the parameters to a 4x4 matrix
    Matrix matrix = *(Matrix*)&params[0];

    // Set a matrix to the 4x4 matrix
    switch (m.mode) {
    case 0: // Projection stack
        m.projection = matrix;
        m.clipDirty = true;
        break;

    case 1: // Coordinate stack
        m.coordinate = matrix;
        m.clipDirty = true;
        break;

    case 2: // Coordinate and directional stacks
        m.coordinate = matrix;
        m.direction = matrix;
        m.clipDirty = true;
        break;

    case 3: // Texture stack
        m.texture = matrix;
        break;
    }
}

void Gpu3D::mtxLoad43Cmd(MatrixState &m, uint32_t *params) {
    // Convert the parameters to a 4x3 matrix
    Matrix matrix;
    for (int i = 0; i < 4; i++)
        memcpy(&matrix.data[i * 4], &params[i * 3], 3 * sizeof(int32_t));

    // Set a matrix to the 4x3 matrix
    switch (m.mode) {
    case 0: // Projection stack
        m.projection = matrix;
        m.clipDirty = true;
        break;

    case 1: // Coordinate stack
        m.coordinate = matrix;
        m.clipDirty = true;
        break;

    case 2: // Coordinate and directional stacks
        m.coordinate = matrix;
        m.direction = matrix;
        m.clipDirty = true;
        break;

    case 3: // Texture stack
        m.texture = matrix;
        break;
    }
}

void Gpu3D::mtxMult44Cmd(MatrixState &m, uint32_t *params) {
    // Convert the parameters to a 4x4 matrix
    Matrix matrix = *(Matrix*)&params[0];

    // Multiply a matrix by the 4x4 matrix
    switch (m.mode) {
    case 0: // Projection stack
        m.projection = matrix * m.projection;
        m.clipDirty = true;
        break;

    case 1: // Coordinate stack
        m.coordinate = matrix * m.coordinate;
        m.clipDirty = true;
        break;

    case 2: // Coordinate and directional stacks
        m.coordinate = matrix * m.coordinate;
        m.direction = matrix * m.direction;
        m.clipDirty = true;
        break;

    case 3: // Texture stack
        m.texture = matrix * m.texture;
        break;
    }
}

void Gpu3D::mtxMult43Cmd(MatrixState &m, uint32_t *params) {
    // Convert the parameters to a 4x3 matrix
    Matrix matrix;
    for (int i = 0; i < 4; i++)
        memcpy(&matrix.data[i * 4], &params[i * 3], 3 * sizeof(int32_t));

    // Multiply a matrix by the 4x3 matrix
    switch (m.mode) {
    case 0: // Projection stack
        m.projection = matrix * m.projection;
        m.clipDirty = true;
        break;

    case 1: // Coordinate stack
        m.coordinate = matrix * m.coordinate;
        m.clipDirty = true;
        break;

    case 2: // Coordinate and directional stacks
        m.coordinate = matrix * m.coordinate;
        m.direction = matrix * m.direction;
        m.clipDirty = true;
        break;

    case 3: // Texture stack
        m.texture = matrix * m.texture;
        break;
    }
}

void Gpu3D::mtxMult33Cmd(MatrixState &m, uint32_t *params) {
    // Convert the parameters to a 3x3 matrix
    Matrix matrix;
    for (int i = 0; i < 3; i++)
        memcpy(&matrix.data[i * 4], &params[i * 3], 3 * sizeof(int32_t));

    // Multiply a matrix by the 3x3 matrix
    switch (m.mode) {
    case 0: // Projection stack
        m.projection = matrix * m.projection;
        m.clipDirty = true;
        break;

    case 1: // Coordinate stack
        m.coordinate = matrix * m.coordinate;
        m.clipDirty = true;
        break;

    case 2: // Coordinate and directional stacks
        m.coordinate = matrix * m.coordinate;
        m.direction = matrix * m.direction;
        m.clipDirty = true;
        break;

    case 3: // Texture stack
        m.texture = matrix * m.texture;
        break;
    }
}

// Multiply a matrix by a scale matrix, which only scales its first 3 rows
static void scale(Matrix &mtx, uint32_t *params) {
    for (int y = 0; y < 3; y++)
        for (int x = 0; x < 4; x++)
            mtx.data[y * 4 + x] = ((int64_t)(int32_t)params[y] * mtx.data[y * 4 + x]) >> 12;
}

// Multiply a matrix by a translation matrix, which only changes its last row
static void translate(Matrix &mtx, uint32_t *params) {
    for (int x = 0; x < 4; x++)
        mtx.data[12 + x] = ((int64_t)(int32_t)params[0] * mtx.data[0 + x] + (int64_t)(int32_t)params[1] * mtx.data[4 + x] +
            (int64_t)(int32_t)params[2] * mtx.data[8 + x] + ((int64_t)mtx.data[12 + x] << 12)) >> 12;
}

void Gpu3D::mtxScaleCmd(MatrixState &m, uint32_t *params) {
    // Multiply a matrix by the scale matrix
    switch (m.mode) {
    case 0: // Projection stack
        scale(m.projection, params);
        m.clipDirty = true;
        break;

    case 1: case 2: // Coordinate stack
        scale(m.coordinate, params);
        m.clipDirty = true;
        break;

    case 3: // Texture stack
        scale(m.texture, params);
        break;
    }
}

void Gpu3D::mtxTransCmd(MatrixState &m, uint32_t *params) {
    // Multiply a matrix by the translation matrix
    switch (m.mode) {
    case 0: // Projection stack
        translate(m.projection, params);
        m.clipDirty = true;
        break;

    case 1: // Coordinate stack
        translate(m.coordinate, params);
        m.clipDirty = true;
        break;

    case 2: // Coordinate and directional stacks
        translate(m.coordinate, params);
        translate(m.direction, params);
        m.clipDirty = true;
        break;

    case 3: // Texture stack
        translate(m.texture, params);
        break;
    }
}

void Gpu3D::colorCmd(uint32_t param) {
    // Set the vertex color
    savedVertex.color = rgb5ToRgb6(param);
}

void Gpu3D::normalCmd(uint32_t param) {
    // Get the normal vector
    Vertex normalVector;
    normalVector.x = ((int16_t)((param & 0x000003FF) << 6)) >> 3;
    normalVector.y = ((int16_t)((param & 0x000FFC00) >> 4)) >> 3;
    normalVector.z = ((int16_t)((param & 0x3FF00000) >> 14)) >> 3;

    // Transform the texture coordinates
    if (textureCoordMode == 2) {
        // Get the normal vector as a vertex
        Vertex vertex = normalVector;
        vertex.w = 1 << 12;

        // Get the texture matrix with the texture coordinates
        Matrix matrix = mtx.texture;
        matrix.data[12] = s << 12;
        matrix.data[13] = t << 12;

        // Multiply the vertex with the matrix
        vertex = vertex * matrix;

        // Save the transformed coordinates
        savedVertex.s = vertex.x >> 12;
        savedVertex.t = vertex.y >> 12;
    }

    // Multiply the normal vector with the directional matrix
    normalVector = normalVector * mtx.direction;

    // Set the base vertex color
    savedVertex.color = emissionColor;

    // Calculate the vertex color
    // This is a translation of the pseudocode from GBATEK to C++
    for (int i = 0; i < 4; i++) {
        if (enabledLights & BIT(i)) {
            int diffuseLevel = -(lightVector[i] * normalVector);
            if (diffuseLevel < (0 << 12)) diffuseLevel = (0 << 12);
            if (diffuseLevel > (1 << 12)) diffuseLevel = (1 << 12);

            int shininessLevel = -(halfVector[i] * normalVector);
            if (shininessLevel < (0 << 12)) shininessLevel = (0 << 12);
            if (shininessLevel > (1 << 12)) shininessLevel = (1 << 12);
            shininessLevel = (shininessLevel * shininessLevel) >> 12;

            if (shininessEnabled) shininessLevel = shininess[shininessLevel >> 5] << 4;

            int r = (savedVertex.color >> 0) & 0x3F;
            int g = (savedVertex.color >> 6) & 0x3F;
            int b = (savedVertex.color >> 12) & 0x3F;

            r += (((specularColor >> 0) & 0x3F) * ((lightColor[i] >> 0) & 0x3F) * shininessLevel) >> 18;
            g += (((specularColor >> 6) & 0x3F) * ((lightColor[i] >> 6) & 0x3F) * shininessLevel) >> 18;
            b += (((specularColor >> 12) & 0x3F) * ((lightColor[i] >> 12) & 0x3F) * shininessLevel) >> 18;

            r += (((diffuseColor >> 0) & 0x3F) * ((lightColor[i] >> 0) & 0x3F) * diffuseLevel) >> 18;
            g += (((diffuseColor >> 6) & 0x3F) * ((lightColor[i] >> 6) & 0x3F) * diffuseLevel) >> 18;
            b += (((diffuseColor >> 12) & 0x3F) * ((lightColor[i] >> 12) & 0x3F) * diffuseLevel) >> 18;

            r += ((ambientColor >> 0) & 0x3F) * ((lightColor[i] >> 0) & 0x3F) >> 6;
            g += ((ambientColor >> 6) & 0x3F) * ((lightColor[i] >> 6) & 0x3F) >> 6;
            b += ((ambientColor >> 12) & 0x3F) * ((lightColor[i] >> 12) & 0x3F) >> 6;

            if (r < 0) r = 0; if (r > 0x3F) r = 0x3F;
            if (g < 0) g = 0; if (g > 0x3F) g = 0x3F;
            if (b < 0) b = 0; if (b > 0x3F) b = 0x3F;

            savedVertex.color = (b << 12) | (g << 6) | r;
        }
    }
}

void Gpu3D::texCoordCmd(uint32_t param) {
    // Set the untransformed texture coordinates
    s = (int16_t)(param >> 0);
    t = (int16_t)(param >> 16);

    // Transform the texture coordinates
    if (textureCoordMode == 1) {
        // Create a vertex with the texture coordinates
        Vertex vertex;
        vertex.x = s << 8;
        vertex.y = t << 8;
        vertex.z = 1 << 8;
        vertex.w = 1 << 8;

        // Multiply the vertex with the texture matrix
        vertex = vertex * mtx.texture;

        // Save the transformed coordinates
        savedVertex.s = vertex.x >> 8;
        savedVertex.t = vertex.y >> 8;
    }
    else {
        savedVertex.s = s;
        savedVertex.t = t;
    }
}

void Gpu3D::vtx16Cmd(uint32_t *params) {
    // Set the X, Y, and Z coordinates
    savedVertex.x = (int16_t)(params[0] >> 0);
    savedVertex.y = (int16_t)(params[0] >> 16);
    savedVertex.z = (int16_t)(params[1]);
    addVertex();
}

void Gpu3D::vtx10Cmd(uint32_t param) {
    // Set the X, Y, and Z coordinates
    savedVertex.x = (int16_t)((param & 0x000003FF) << 6);
    savedVertex.y = (int16_t)((param & 0x000FFC00) >> 4);
    savedVertex.z = (int16_t)((param & 0x3FF00000) >> 14);
    addVertex();
}

void Gpu3D::vtxXYCmd(uint32_t param) {
    // Set the X and Y coordinates
    savedVertex.x = (int16_t)(param >> 0);
    savedVertex.y = (int16_t)(param >> 16);
    addVertex();
}

void Gpu3D::vtxXZCmd(uint32_t param) {
    // Set the X and Z coordinates
    savedVertex.x = (int16_t)(param >> 0);
    savedVertex.z = (int16_t)(param >> 16);
    addVertex();
}

void Gpu3D::vtxYZCmd(uint32_t param) {
    // Set the Y and Z coordinates
    savedVertex.y = (int16_t)(param >> 0);
    savedVertex.z = (int16_t)(param >> 16);
    addVertex();
}

void Gpu3D::vtxDiffCmd(uint32_t param) {
    // Add offsets to the X, Y, and Z coordinates
    savedVertex.x += ((int16_t)((param & 0x000003FF) << 6) / 8) >> 3;
    savedVertex.y += ((int16_t)((param & 0x000FFC00) >> 4) / 8) >> 3;
    savedVertex.z += ((int16_t)((param & 0x3FF00000) >> 14) / 8) >> 3;
    addVertex();
}

void Gpu3D::polygonAttrCmd(uint32_t param) {
    // Set the polygon attributes
    // Values are not actually applied until the next vertex list
    polygonAttr = param;
}

void Gpu3D::texImageParamCmd(uint32_t param) {
    // Set the texture parameters
    savedPolygon.textureAddr = (param & 0x0000FFFF) << 3;
    savedPolygon.sizeS = 8 << ((param & 0x00700000) >> 20);
    savedPolygon.sizeT = 8 << ((param & 0x03800000) >> 23);
    savedPolygon.repeatS = param & BIT(16);
    savedPolygon.repeatT = param & BIT(17);
    savedPolygon.flipS = param & BIT(18);
    savedPolygon.flipT = param & BIT(19);
    savedPolygon.textureFmt = (param & 0x1C000000) >> 26;
    savedPolygon.transparent0 = param & BIT(29);
    textureCoordMode = (param & 0xC0000000) >> 30;
}

void Gpu3D::plttBaseCmd(uint32_t param) {
    // Set the palette base address
    savedPolygon.paletteAddr = param & 0x00001FFF;
}

void Gpu3D::difAmbCmd(uint32_t param) {
    // Set the diffuse and ambient reflection colors
    diffuseColor = rgb5ToRgb6(param >> 0);
    ambientColor = rgb5ToRgb6(param >> 16);

    // Directly set the vertex color
    if (param & BIT(15))
        savedVertex.color = diffuseColor;
}

void Gpu3D::speEmiCmd(uint32_t param) {
    // Set the specular reflection and emission colors
    specularColor = rgb5ToRgb6(param >> 0);
    emissionColor = rgb5ToRgb6(param >> 16);

    // Set the shininess table toggle
    shininessEnabled = param & BIT(15);
}

void Gpu3D::lightVectorCmd(uint32_t param) {
    // Set one of the light vectors
    lightVector[param >> 30].x = ((int16_t)((param & 0x000003FF) << 6)) >> 3;
    lightVector[param >> 30].y = ((int16_t)((param & 0x000FFC00) >> 4)) >> 3;
    lightVector[param >> 30].z = ((int16_t)((param & 0x3FF00000) >> 14)) >> 3;

    // Multiply the light vector by the directional matrix
    lightVector[param >> 30] = lightVector[param >> 30] * mtx.direction;

    // Set one of the half vectors
    halfVector[param >> 30].x = (lightVector[param >> 30].x) / 2;
    halfVector[param >> 30].y = (lightVector[param >> 30].y) / 2;
    halfVector[param >> 30].z = (lightVector[param >> 30].z - (1 << 12)) / 2;
}

void Gpu3D::lightColorCmd(uint32_t param) {
    // Set one of the light colors
    lightColor[param >> 30] = rgb5ToRgb6(param);
}

void Gpu3D::shininessCmd(uint32_t *params) {
    // Set the values of the specular reflection shininess table
    for (int i = 0; i < 32; i++) {
        shininess[i * 4 + 0] = params[i] >> 0;
        shininess[i * 4 + 1] = params[i] >> 8;
        shininess[i * 4 + 2] = params[i] >> 16;
        shininess[i * 4 + 3] = params[i] >> 24;
    }
}

void Gpu3D::beginVtxsCmd(uint32_t param) {
    // Clipping a polygon strip starts a new strip with the last 2 vertices of the old one
    // Discard these vertices if they're unused
    if (vertexCount < 3 + (polygonType & 1))
        vertexCountIn -= vertexCount;

    // Process the previous vertex list and start a new one
    processVertices();
    polygonType = param & 0x00000003;
    vertexCount = 0;
    clockwise = false;

    // Apply the polygon attributes
    enabledLights = polygonAttr & 0x0000000F;
    savedPolygon.mode = (polygonAttr & 0x00000030) >> 4;
    renderBack = polygonAttr & BIT(6);
    renderFront = polygonAttr & BIT(7);
    savedPolygon.transNewDepth = polygonAttr & BIT(11);
    savedPolygon.depthTestEqual = polygonAttr & BIT(14);
    savedPolygon.fog = polygonAttr & BIT(15);
    savedPolygon.alpha = ((polygonAttr & 0x001F0000) >> 16) * 2;
    if (savedPolygon.alpha > 0) savedPolygon.alpha++;
    savedPolygon.id = (polygonAttr & 0x3F000000) >> 24;
}

void Gpu3D::swapBuffersCmd(uint32_t param) {
    // Set the W-buffering toggle
    // The geometry engine is halted when the command is taken from the FIFO, until buffers swap on next V-blank
    savedPolygon.wBuffer = param & BIT(1);
}

void Gpu3D::viewportCmd(uint32_t param) {
    // Set the viewport dimensions to apply for the next vertex list
    viewportNext[0] = ((param >> 0) & 0xFF) & 0x1FF;
    viewportNext[1] = (191 - ((param >> 24) & 0xFF)) & 0xFF;
    viewportNext[2] = (((param >> 16) & 0xFF) - viewportNext[0] + 1) & 0x1FF;
    viewportNext[3] = ((191 - ((param >> 8) & 0xFF)) - viewportNext[1] + 1) & 0xFF;
}

void Gpu3D::boxTestCmd(uint32_t *params) {
    // Store the parameters (X-pos, Y-pos, Z-pos, width, height, depth)
    int16_t boxTestCoords[6] = {
        (int16_t)params[0], (int16_t)(params[0] >> 16),
        (int16_t)params[1], (int16_t)(params[1] >> 16),
        (int16_t)params[2], (int16_t)(params[2] >> 16)
    };

    // Add positions to the offsets so everything is a 16-bit position
    boxTestCoords[3] += boxTestCoords[0];
    boxTestCoords[4] += boxTestCoords[1];
    boxTestCoords[5] += boxTestCoords[2];

    // Define the coordinate indices that make up the box vertices
    static const uint8_t indices[8 * 3] = {
        0, 1, 2,
        3, 1, 2,
        0, 4, 2,
        0, 1, 5,
        3, 4, 2,
        3, 1, 5,
        0, 4, 5,
        3, 4, 5
    };

    // Update the clip matrix if necessary
    mtx.updateClip();

    // Get the transformed vertices of the box
    Vertex vertices[8];
    for (int i = 0; i < 8; i++) {
        vertices[i].x = boxTestCoords[indices[i * 3 + 0]];
        vertices[i].y = boxTestCoords[indices[i * 3 + 1]];
        vertices[i].z = boxTestCoords[indices[i * 3 + 2]];
        vertices[i].w = 1 << 12;
        vertices[i] = vertices[i] * mtx.clip;
    }

    // Arrange the vertices to represent the faces of the box
    Vertex faces[6][4] = {
        { vertices[0], vertices[1], vertices[4], vertices[2] },
        { vertices[3], vertices[5], vertices[7], vertices[6] },
        { vertices[3], vertices[5], vertices[1], vertices[0] },
        { vertices[6], vertices[7], vertices[4], vertices[2] },
        { vertices[0], vertices[3], vertices[6], vertices[2] },
        { vertices[1], vertices[5], vertices[7], vertices[4] }
    };

    // Clip the faces of the box
    // If any of the faces are in view, set the result bit
    for (int i = 0; i < 6; i++) {
        uint8_t size = 4;
        Vertex clipped[10];
        clipPolygon(faces[i], clipped, &size);

        if (size > 0) {
            gxStatExec |= BIT(1);
            return;
        }
    }

    // Clear the result bit if none of the faces were in view
    gxStatExec &= ~BIT(1);
}

void Gpu3D::posTestCmd(uint32_t *params) {
    // Set the X, Y, and Z coordinates, overwriting the saved vertex
    savedVertex.x = (int16_t)(params[0] >> 0);
    savedVertex.y = (int16_t)(params[0] >> 16);
    savedVertex.z = (int16_t)(params[1]);
    savedVertex.w = 1 << 12;

    // Update the clip matrix if necessary
    mtx.updateClip();

    // Multiply the vertex with the clip matrix and set the result
    Vertex vertex = savedVertex * mtx.clip;
    posResult[0] = vertex.x;
    posResult[1] = vertex.y;
    posResult[2] = vertex.z;
    posResult[3] = vertex.w;
}

void Gpu3D::vecTestCmd(uint32_t param) {
    // Set the vector components
    Vertex vector;
    vector.x = ((int16_t)((param & 0x000003FF) << 6)) >> 3;
    vector.y = ((int16_t)((param & 0x000FFC00) >> 4)) >> 3;
    vector.z = ((int16_t)((param & 0x3FF00000) >> 14)) >> 3;

    // Multiply the vector with the directional matrix and set the result
    // The result has 4-bit signs and 12-bit fractions
    vector = vector * mtx.direction;
    vecResult[0] = ((int16_t)(vector.x << 3)) >> 3;
    vecResult[1] = ((int16_t)(vector.y << 3)) >> 3;
    vecResult[2] = ((int16_t)(vector.z << 3)) >> 3;
}

void Gpu3D::trackEntry(uint8_t command) {
    switch (command) {
    case 0x11: case 0x12: // MTX_PUSH, MTX_POP
        // Track queued matrix commands and set the busy bit until they finish
        matrixQueue++;
        gxStat |= BIT(14);
        break;

    case 0x70: case 0x71: case 0x72: // BOX_TEST, POS_TEST, VEC_TEST
        // Track queued test commands and set the busy bit until they finish
        testQueue++;
        gxStat |= BIT(0);
        break;
    }
}

FORCE_INLINE void Gpu3D::queueEntry(Entry entry) {
    // Add an entry to the FIFO, leaving the FIFO status to be updated after a batch of entries
    // No time passes and nothing else runs between entries of a batch, so the result is the same as updating each time
    // Data moves directly into the pipe if the FIFO is empty and the pipe isn't full
    if (fifo.size() - pipeSize == 0 && pipeSize < 4) {
        pipeSize++;
        gxStat |= BIT(27); // Commands executing
    }
    fifo.push_back(entry);
    trackEntry(entry.command);
}

void Gpu3D::finishEntries(size_t oldSize) {
    // Update the FIFO status after entries were added, if any were; entries only go into the FIFO itself once
    // the pipe is full, and only increase the count, so checks against the final count match checking after each entry
    if (fifo.size() == oldSize) return;
    if (uint32_t count = fifo.size() - pipeSize) {
        // If the FIFO was full when an entry was added, halt the CPU until space is free
        if (count > 256)
            core->interpreter[0].halt(1);

        // Update the FIFO status
        gxStat = (gxStat & ~0x01FF0000) | (count << 16); // FIFO entries
        gxStat &= ~BIT(26); // FIFO not empty

        // If the FIFO is half full or more, disable GXFIFO DMA transfers
        if (count >= 128 && (gxStat & BIT(25)))
            gxStat &= ~BIT(25);
    }

    // Start executing commands if one is ready, after a moment so that words written one by one share a batch
    if (state == GX_IDLE && fifo.size() >= paramCounts[fifo.front().command]) {
        core->schedule(GPU3D_COMMANDS, GPU3D_START);
        state = GX_RUNNING;
    }
}

void Gpu3D::addEntry(Entry entry) {
    // Add a single entry and update the FIFO status
    size_t oldSize = fifo.size();
    queueEntry(entry);
    finishEntries(oldSize);
}

void Gpu3D::queueGxFifo(const uint8_t *data, int count) {
    // Add entries for little-endian words written to GXFIFO, without updating the FIFO status; there must be room for them
    // The FIFO's state is kept in local variables, since otherwise it's read again after every store
    Entry *entries = fifo.entries;
    size_t mask = fifo.mask, tail = fifo.head + fifo.count, size = fifo.count;
    uint32_t pipe = pipeSize, packed = gxFifo;
    uint8_t received = gxFifoCount;

    auto add = [&](uint8_t command, uint32_t param) __attribute__((always_inline)) {
        // Data moves directly into the pipe if the FIFO is empty and the pipe isn't full
        if (size == pipe && pipe < 4) {
            pipe++;
            gxStat |= BIT(27); // Commands executing
        }
        entries[tail++ & mask] = Entry(command, param);
        size++;
        if (command >= 0x11 && (command <= 0x12 || command >= 0x70)) trackEntry(command);
    };

    for (int i = 0; i < count; i++) {
        uint32_t value = readLsb<uint32_t>(&data[i * 4]);
        if (packed == 0) {
            // Read new packed commands
            packed = value;
        }
        else {
            // Add a command parameter
            add(packed, value);

            // Move to the next command once all parameters have been sent
            if (++received == paramCounts[packed & 0xFF]) {
                packed >>= 8;
                received = 0;
            }
        }

        // Add entries for commands with no parameters
        while (packed != 0 && paramCounts[packed & 0xFF] == 0) {
            add(packed, 0);
            packed >>= 8;
        }
    }

    fifo.count = size;
    pipeSize = pipe;
    gxFifo = packed;
    gxFifoCount = received;
}

void Gpu3D::writeGxFifo(uint32_t mask, uint32_t value) {
    // Write a word to GXFIFO
    uint8_t data[4];
    writeLsb<uint32_t>(data, value & mask);
    writeGxFifo(data, 1);
}

void Gpu3D::writeGxFifo(const uint8_t *data, int count) {
    // Write a batch of little-endian words to GXFIFO, as a DMA transfer does, updating the FIFO status once at the end
    EP_START(fifoStart);
    size_t oldSize = fifo.size();
    while (count > 0) {
        // A word adds at most 4 entries, so make room for a run of words first
        int run = std::min<size_t>(count, (fifo.mask + 1 - fifo.size()) / 4);
        if (run == 0) {
            fifo.grow();
            continue;
        }
        queueGxFifo(data, run);
        data += run * 4;
        count -= run;
    }
    finishEntries(oldSize);
    EP_ADD(EP_GXFIFO, fifoStart);
}

void Gpu3D::writeMtxMode(uint32_t mask, uint32_t value) {
    // Add an entry to the FIFO
    Entry entry(0x10, value & mask);
    addEntry(entry);
}

void Gpu3D::writeMtxPush(uint32_t mask, uint32_t value) {
    // Add an entry to the FIFO
    Entry entry(0x11, value & mask);
    addEntry(entry);
}

void Gpu3D::writeMtxPop(uint32_t mask, uint32_t value) {
    // Add an entry to the FIFO
    Entry entry(0x12, value & mask);
    addEntry(entry);
}

void Gpu3D::writeMtxStore(uint32_t mask, uint32_t value) {
    // Add an entry to the FIFO
    Entry entry(0x13, value & mask);
    addEntry(entry);
}

void Gpu3D::writeMtxRestore(uint32_t mask, uint32_t value) {
    // Add an entry to the FIFO
    Entry entry(0x14, value & mask);
    addEntry(entry);
}

void Gpu3D::writeMtxIdentity(uint32_t mask, uint32_t value) {
    // Add an entry to the FIFO
    Entry entry(0x15, value & mask);
    addEntry(entry);
}

void Gpu3D::writeMtxLoad44(uint32_t mask, uint32_t value) {
    // Add an entry to the FIFO
    Entry entry(0x16, value & mask);
    addEntry(entry);
}

void Gpu3D::writeMtxLoad43(uint32_t mask, uint32_t value) {
    // Add an entry to the FIFO
    Entry entry(0x17, value & mask);
    addEntry(entry);
}

void Gpu3D::writeMtxMult44(uint32_t mask, uint32_t value) {
    // Add an entry to the FIFO
    Entry entry(0x18, value & mask);
    addEntry(entry);
}

void Gpu3D::writeMtxMult43(uint32_t mask, uint32_t value) {
    // Add an entry to the FIFO
    Entry entry(0x19, value & mask);
    addEntry(entry);
}

void Gpu3D::writeMtxMult33(uint32_t mask, uint32_t value) {
    // Add an entry to the FIFO
    Entry entry(0x1A, value & mask);
    addEntry(entry);
}

void Gpu3D::writeMtxScale(uint32_t mask, uint32_t value) {
    // Add an entry to the FIFO
    Entry entry(0x1B, value & mask);
    addEntry(entry);
}

void Gpu3D::writeMtxTrans(uint32_t mask, uint32_t value) {
    // Add an entry to the FIFO
    Entry entry(0x1C, value & mask);
    addEntry(entry);
}

void Gpu3D::writeColor(uint32_t mask, uint32_t value) {
    // Add an entry to the FIFO
    Entry entry(0x20, value & mask);
    addEntry(entry);
}

void Gpu3D::writeNormal(uint32_t mask, uint32_t value) {
    // Add an entry to the FIFO
    Entry entry(0x21, value & mask);
    addEntry(entry);
}

void Gpu3D::writeTexCoord(uint32_t mask, uint32_t value) {
    // Add an entry to the FIFO
    Entry entry(0x22, value & mask);
    addEntry(entry);
}

void Gpu3D::writeVtx16(uint32_t mask, uint32_t value) {
    // Add an entry to the FIFO
    Entry entry(0x23, value & mask);
    addEntry(entry);
}

void Gpu3D::writeVtx10(uint32_t mask, uint32_t value) {
    // Add an entry to the FIFO
    Entry entry(0x24, value & mask);
    addEntry(entry);
}

void Gpu3D::writeVtxXY(uint32_t mask, uint32_t value) {
    // Add an entry to the FIFO
    Entry entry(0x25, value & mask);
    addEntry(entry);
}

void Gpu3D::writeVtxXZ(uint32_t mask, uint32_t value) {
    // Add an entry to the FIFO
    Entry entry(0x26, value & mask);
    addEntry(entry);
}

void Gpu3D::writeVtxYZ(uint32_t mask, uint32_t value) {
    // Add an entry to the FIFO
    Entry entry(0x27, value & mask);
    addEntry(entry);
}

void Gpu3D::writeVtxDiff(uint32_t mask, uint32_t value) {
    // Add an entry to the FIFO
    Entry entry(0x28, value & mask);
    addEntry(entry);
}

void Gpu3D::writePolygonAttr(uint32_t mask, uint32_t value) {
    // Add an entry to the FIFO
    Entry entry(0x29, value & mask);
    addEntry(entry);
}

void Gpu3D::writeTexImageParam(uint32_t mask, uint32_t value) {
    // Add an entry to the FIFO
    Entry entry(0x2A, value & mask);
    addEntry(entry);
}

void Gpu3D::writePlttBase(uint32_t mask, uint32_t value) {
    // Add an entry to the FIFO
    Entry entry(0x2B, value & mask);
    addEntry(entry);
}

void Gpu3D::writeDifAmb(uint32_t mask, uint32_t value) {
    // Add an entry to the FIFO
    Entry entry(0x30, value & mask);
    addEntry(entry);
}

void Gpu3D::writeSpeEmi(uint32_t mask, uint32_t value) {
    // Add an entry to the FIFO
    Entry entry(0x31, value & mask);
    addEntry(entry);
}

void Gpu3D::writeLightVector(uint32_t mask, uint32_t value) {
    // Add an entry to the FIFO
    Entry entry(0x32, value & mask);
    addEntry(entry);
}

void Gpu3D::writeLightColor(uint32_t mask, uint32_t value) {
    // Add an entry to the FIFO
    Entry entry(0x33, value & mask);
    addEntry(entry);
}

void Gpu3D::writeShininess(uint32_t mask, uint32_t value) {
    // Add an entry to the FIFO
    Entry entry(0x34, value & mask);
    addEntry(entry);
}

void Gpu3D::writeBeginVtxs(uint32_t mask, uint32_t value) {
    // Add an entry to the FIFO
    Entry entry(0x40, value & mask);
    addEntry(entry);
}

void Gpu3D::writeEndVtxs(uint32_t mask, uint32_t value) {
    // Add an entry to the FIFO
    Entry entry(0x41, value & mask);
    addEntry(entry);
}

void Gpu3D::writeSwapBuffers(uint32_t mask, uint32_t value) {
    // Add an entry to the FIFO
    Entry entry(0x50, value & mask);
    addEntry(entry);
}

void Gpu3D::writeViewport(uint32_t mask, uint32_t value) {
    // Add an entry to the FIFO
    Entry entry(0x60, value & mask);
    addEntry(entry);
}

void Gpu3D::writeBoxTest(uint32_t mask, uint32_t value) {
    // Add an entry to the FIFO
    Entry entry(0x70, value & mask);
    addEntry(entry);
}

void Gpu3D::writePosTest(uint32_t mask, uint32_t value) {
    // Add an entry to the FIFO
    Entry entry(0x71, value & mask);
    addEntry(entry);
}

void Gpu3D::writeVecTest(uint32_t mask, uint32_t value) {
    // Add an entry to the FIFO
    Entry entry(0x72, value & mask);
    addEntry(entry);
}

void Gpu3D::writeGxStat(uint32_t mask, uint32_t value) {
    // Clear the error bit and reset the projection stack pointer
    if (value & BIT(15))
        gxStat &= ~0xA000;

    // Write to the GXSTAT register
    mask &= 0xC0000000;
    gxStat = (gxStat & ~mask) | (value & mask);

    // The interrupt follows the FIFO's state, so it's sent right away if the FIFO already meets the new condition
    checkFifoIrq();
}

void Gpu3D::checkFifoIrq() {
    // Send a GXFIFO interrupt if the FIFO meets the enabled condition
    switch ((gxStat & 0xC0000000) >> 30) {
        case 1: if (gxStat & BIT(25)) core->interpreter[0].sendInterrupt(21); break;
        case 2: if (gxStat & BIT(26)) core->interpreter[0].sendInterrupt(21); break;
    }
}

uint32_t Gpu3D::readGxStat() {
    // Read from the GXSTAT register, once queued commands that change the bits set by execution have run
    syncStat();
    return gxStat | gxStatExec;
}

uint32_t Gpu3D::readRamCount() {
    // Read from the RAM_COUNT register, after any queued commands have run
    sync();
    return (vertexCountIn << 16) | polygonCountIn;
}

uint32_t Gpu3D::readClipMtxResult(int index) {
    // Read from one of the CLIPMTX_RESULT registers, using the FIFO's copy of the matrices if there's a worker
    MatrixState &m = worker ? fifoMtx : mtx;
    m.updateClip();
    return m.clip.data[index];
}

uint32_t Gpu3D::readVecMtxResult(int index) {
    // Read from one of the VECMTX_RESULT registers, using the FIFO's copy of the matrices if there's a worker
    MatrixState &m = worker ? fifoMtx : mtx;
    return m.direction.data[(index / 3) * 4 + index % 3];
}
