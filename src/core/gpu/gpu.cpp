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
#include <malloc.h>
#include <mutex>

#include "../core.h"

#ifdef FAST2D_CHECK
#include <coreinit/debug.h>
#include <coreinit/internal.h>
#endif
#include "../emu_profile.h"
#include "../frame_capture.h"

// Framebuffers freed by the frontend, kept for reuse since a few are made every frame; allocating and freeing such
// large blocks all the time fragments the heap until they no longer fit
static std::mutex poolMutex;
static uint32_t *pool[8];
static int poolCount = 0;

// Allocate framebuffers for the frontend, aligned to cache blocks on Wii U so lines can be claimed without reading them
static uint32_t *newFramebuffer(size_t count) {
    {
        std::lock_guard<std::mutex> guard(poolMutex);
        for (int i = 0; i < poolCount; i++) {
            size_t size = malloc_usable_size(pool[i]);
            if (size < count * sizeof(uint32_t) || size > count * sizeof(uint32_t) + 64) continue;
            uint32_t *buffer = pool[i];
            pool[i] = pool[--poolCount];
            return buffer;
        }
    }
    return (uint32_t*)memalign(0x1000, count * sizeof(uint32_t)); // Aligned enough for the GPU to write too
}

// High-res 3D frames that have more than one holder, with how many extra each has, since frames without new 3D reuse
// the last high-res frame instead of copying it again
static std::mutex sharedMutex;
static std::pair<uint32_t*, int> shared[8];

static uint32_t *shareFramebuffer(uint32_t *buffer) {
    // Add a holder to a framebuffer, which is only freed once every holder has freed it, or return null if too many
    // framebuffers are shared already
    std::lock_guard<std::mutex> guard(sharedMutex);
    for (auto &entry : shared) {
        if (entry.first != buffer || !entry.second) continue;
        entry.second++;
        return buffer;
    }
    for (auto &entry : shared) {
        if (entry.second) continue;
        entry = { buffer, 1 };
        return buffer;
    }
    return nullptr;
}

static void freeFramebuffer(uint32_t *buffer) {
    if (!buffer) return;
    {
        // Only drop a holder if the framebuffer has others
        std::lock_guard<std::mutex> guard(sharedMutex);
        for (auto &entry : shared) {
            if (entry.first != buffer || !entry.second) continue;
            entry.second--;
            return;
        }
    }
    std::lock_guard<std::mutex> guard(poolMutex);
    if (poolCount < 8) {
        pool[poolCount++] = buffer;
        return;
    }
    free(buffer);
}

Gpu::Gpu(Core *core): core(core) {
    // Mark the thread as not drawing to start
    ready.store(false);
    running.store(false);
    drawing.store(0);
    drawingB.store(0);
    frameActive.store(false);
    captureDone.store(false);
}

Gpu::~Gpu() {
    // Clean up the threads and the engines' drawing copies
    stopThreads();
    for (Gpu2D *drawer : drawers)
        delete drawer;

    // Clean up any queued framebuffers
    freeFramebuffer(drawBuffer);
    while (!framebuffers.empty()) {
        Buffers &buffers = framebuffers.front();
        freeFramebuffer(buffers.framebuffer);
        freeFramebuffer(buffers.hiRes3D);
        framebuffers.pop();
    }
}

void Gpu::init() {
    // Set the new LCD initialization ready flags on DSi
    if (!core->dsiMode) return;
    dispStat[0] |= BIT(6);
    dispStat[1] |= BIT(6);
}

void Gpu::saveState(FILE *file) {
    // Write state data to the file
    fwrite(dispStat, 2, sizeof(dispStat) / 2, file);
    fwrite(&vCount, sizeof(vCount), 1, file);
    fwrite(&dispCapCnt, sizeof(dispCapCnt), 1, file);
    fwrite(&powCnt1, sizeof(powCnt1), 1, file);
}

void Gpu::loadState(FILE *file) {
    // Read state data from the file
    fread(dispStat, 2, sizeof(dispStat) / 2, file);
    fread(&vCount, sizeof(vCount), 1, file);
    fread(&dispCapCnt, sizeof(dispCapCnt), 1, file);
    fread(&powCnt1, sizeof(powCnt1), 1, file);
}

uint32_t Gpu::rgb5ToRgb8(uint32_t color) {
    // Convert an RGB5 value to an RGB8 value, with RGB6 as an intermediate
    uint8_t r = (((color >> 0) & 0x1F) << 1) * 255 / 63;
    uint8_t g = (((color >> 5) & 0x1F) << 1) * 255 / 63;
    uint8_t b = (((color >> 10) & 0x1F) << 1) * 255 / 63;
    return (0xFF << 24) | (b << 16) | (g << 8) | r;
}

// Each 6-bit channel value scaled to 8 bits, looked up since the scaling divides
struct Rgb6Table {
    uint8_t values[64];
    constexpr Rgb6Table(): values() {
        for (int i = 0; i < 64; i++)
            values[i] = i * 255 / 63;
    }
};
static constexpr Rgb6Table rgb6Table;

uint32_t *Gpu::newHiRes3D() {
    // Allocate a buffer for a high-res 3D frame
    return newFramebuffer(256 * 192 * 4);
}

void Gpu::freeHiRes3D(uint32_t *buffer) {
    // Free a high-res 3D frame taken from getFrame
    freeFramebuffer(buffer);
}

uint32_t *Gpu::shareHiRes3D(uint32_t *buffer) {
    // Add a holder to a high-res 3D frame, which each holder frees, or return null if that's not possible
    return buffer ? shareFramebuffer(buffer) : nullptr;
}

uint32_t Gpu::rgb6ToRgb8(uint32_t color) {
    // Convert an RGB6 value to an RGB8 value
    uint8_t r = rgb6Table.values[(color >> 0) & 0x3F];
    uint8_t g = rgb6Table.values[(color >> 6) & 0x3F];
    uint8_t b = rgb6Table.values[(color >> 12) & 0x3F];
    return (0xFF << 24) | (b << 16) | (g << 8) | r;
}

uint16_t Gpu::rgb6ToRgb5(uint32_t color) {
    // Convert an RGB6 value to an RGB5 value
    uint8_t r = ((color >> 0) & 0x3F) / 2;
    uint8_t g = ((color >> 6) & 0x3F) / 2;
    uint8_t b = ((color >> 12) & 0x3F) / 2;
    return BIT(15) | (b << 10) | (g << 5) | r;
}

bool Gpu::getFrame(uint32_t *out, bool gbaCrop, uint32_t **hiRes3D, bool *top3D) {
    // Check if a new frame is ready
    if (!ready.load())
        return false;

    // Get the next queued buffers
    Buffers &buffers = framebuffers.front();
    lastFrameId = buffers.id;

    // When the caller takes high-res 3D frames to draw them itself, output native frames with 3D pixels marked
    bool upscale = (frontendUpscale >= 0) ? (frontendUpscale && !hiRes3D) :
        ((Settings::highRes3D || Settings::screenFilter == 1) && !hiRes3D);

    if (gbaCrop) {
        // Output the frame in RGB8 format, cropped for GBA
        if (upscale) {
            // GBA doesn't have 3D, but draw the screen upscaled for consistency
            for (int y = 0; y < 160; y++) {
                uint32_t *line = &out[y * 240 * 4];
                for (int x = 0; x < 240; x++)
                    line[x * 2] = line[x * 2 + 1] = rgb5ToRgb8(buffers.framebuffer[y * 256 + x]);
                memcpy(&line[240 * 2], line, 240 * 8);
            }
        }
        else {
            // Draw to a native resolution buffer
            for (int y = 0; y < 160; y++)
                for (int x = 0; x < 240; x++)
                    out[y * 240 + x] = rgb5ToRgb8(buffers.framebuffer[y * 256 + x]);
        }
    }
    else if (core->gbaMode) {
        int offset = (powCnt1 & BIT(15)) ? 0 : (256 * 192); // Display swap
        uint32_t base = 0x6800000 + gbaBlock * 0x20000;
        gbaBlock = !gbaBlock;

        // The DS draws the GBA screen by capturing it to alternating VRAM blocks and then displaying that
        // While not used officially, it's possible to copy images into VRAM before entering GBA mode to use as a border
        // Output the GBA frame, centered, with the current VRAM border around it
        if (upscale) {
            // GBA doesn't have 3D, but draw the screen upscaled for consistency
            for (int y = 0; y < 192; y++) {
                uint32_t *line = &out[offset * 4 + y * 256 * 4];
                for (int x = 0; x < 256; x++)
                    line[x * 2] = line[x * 2 + 1] = rgb5ToRgb8((x >= 8 && x < 248 && y >= 16 && y < 176) ? buffers.
                        framebuffer[(y - 16) * 256 + x - 8] : core->memory.read<uint16_t>(0, base + (y * 256 + x) * 2));
                memcpy(&line[256 * 2], line, 256 * 8);
            }

            // Clear the secondary display
            memset(&out[(256 * 192 - offset) * 4], 0, 256 * 192 * 4 * sizeof(uint32_t));
        }
        else {
            // Draw to a native resolution buffer
            for (int y = 0; y < 192; y++)
                for (int x = 0; x < 256; x++)
                    out[offset + y * 256 + x] = rgb5ToRgb8((x >= 8 && x < 248 && y >= 16 && y < 176) ? buffers.
                        framebuffer[(y - 16) * 256 + x - 8] : core->memory.read<uint16_t>(0, base + (y * 256 + x) * 2));

            // Clear the secondary display
            memset(&out[256 * 192 - offset], 0, 256 * 192 * sizeof(uint32_t));
        }
    }
    else {
        // Output the full frame in RGB8 format
        if (upscale) {
            if (buffers.hiRes3D) {
                // Draw the screens upscaled, replacing any 3D pixels with high-res output
                // Both screens use the same high-res lines, since 3D can be on either of them
                for (int y = 0; y < 192 * 2; y++) {
                    const uint32_t *line = &buffers.framebuffer[y * 256];
                    const uint32_t *hiRes = &buffers.hiRes3D[(y % 192) * 256 * 4];
                    uint32_t *out0 = &out[y * 256 * 4], *out1 = out0 + 256 * 2;
                    for (int x = 0; x < 256; x++) {
                        uint32_t value = line[x];
                        if (value & BIT(26)) { // 3D
                            uint32_t value2 = hiRes[x * 2 + 0];
                            out0[x * 2 + 0] = rgb6ToRgb8((value2 & 0xFC0000) ? value2 : value);
                            value2 = hiRes[x * 2 + 1];
                            out0[x * 2 + 1] = rgb6ToRgb8((value2 & 0xFC0000) ? value2 : value);
                            value2 = hiRes[x * 2 + 512];
                            out1[x * 2 + 0] = rgb6ToRgb8((value2 & 0xFC0000) ? value2 : value);
                            value2 = hiRes[x * 2 + 513];
                            out1[x * 2 + 1] = rgb6ToRgb8((value2 & 0xFC0000) ? value2 : value);
                        }
                        else {
                            uint32_t color = rgb6ToRgb8(value);
                            out0[x * 2 + 0] = out0[x * 2 + 1] = color;
                            out1[x * 2 + 0] = out1[x * 2 + 1] = color;
                        }
                    }
                }
            }
            else {
                // Even when 3D isn't enabled, draw the screens upscaled for consistency
                for (int y = 0; y < 192 * 2; y++) {
                    uint32_t *line = &out[y * 256 * 4];
                    for (int x = 0; x < 256; x++)
                        line[x * 2] = line[x * 2 + 1] = rgb6ToRgb8(buffers.framebuffer[y * 256 + x]);
                    memcpy(&line[256 * 2], line, 256 * 8);
                }
            }
        }
        else if (hiRes3D) {
            // Draw to a native resolution buffer, with alpha cleared where pixels show 3D
            for (int i = 0; i < 256 * 192 * 2; i++) {
                uint32_t value = buffers.framebuffer[i];
                out[i] = rgb6ToRgb8(value) & ((value & BIT(26)) ? 0x00FFFFFF : 0xFFFFFFFF);
            }
        }
        else {
            // Draw to a native resolution buffer
            for (int i = 0; i < 256 * 192 * 2; i++)
                out[i] = rgb6ToRgb8(buffers.framebuffer[i]);
        }
    }

    // Free the used buffers, handing over the high-res 3D frame if the caller takes it
    freeFramebuffer(buffers.framebuffer);
    if (hiRes3D) {
        *hiRes3D = buffers.hiRes3D;
        if (top3D) *top3D = buffers.top3D;
    }
    else {
        freeFramebuffer(buffers.hiRes3D);
    }

    if (Settings::screenGhost && !frontendGhosting) {
        // Get the size of the output framebuffer
        static uint32_t prev[256 * 192 * 8];
        uint32_t width = (gbaCrop ? 240 : 256) << upscale;
        uint32_t height = (gbaCrop ? 160 : (192 * 2)) << upscale;
        uint32_t size = width * height;

        // Blend output with the previous frame if ghosting is enabled
        for (uint32_t i = 0; i < size; i++) {
            uint8_t r = (((prev[i] >> 0) & 0xFF) + ((out[i] >> 0) & 0xFF)) >> 1;
            uint8_t g = (((prev[i] >> 8) & 0xFF) + ((out[i] >> 8) & 0xFF)) >> 1;
            uint8_t b = (((prev[i] >> 16) & 0xFF) + ((out[i] >> 16) & 0xFF)) >> 1;
            prev[i] = out[i];
            out[i] = 0xFF000000 | (b << 16) | (g << 8) | r;
        }
    }

    // Remove the frame from the queue
    mutex.lock();
    framebuffers.pop();
    ready.store(!framebuffers.empty());
    mutex.unlock();
    return true;
}

void Gpu::gbaScanline240() {
    if (vCount < 160) {
        if (thread) {
            // Wait for the thread to finish the scanline
            SpinWait spin;
            while (drawing.load() != 0)
                spin.pause();
        }
        else if (frames == 0) {
            // Draw the current scanline
            core->gpu2D[0].drawGbaScanline(vCount);
        }

        // Trigger H-blank DMA transfers for visible scanlines
        core->dma[1].trigger(2);
    }

    // Set the H-blank flag
    dispStat[1] |= BIT(1);

    // Trigger an H-blank IRQ if enabled
    if (dispStat[1] & BIT(4))
        core->interpreter[1].sendInterrupt(1);

    // Reschedule the task for the next scanline
    core->schedule(GBA_SCANLINE240, 308 * 4);
}

void Gpu::gbaScanline308() {
    // Clear the H-blank flag
    dispStat[1] &= ~BIT(1);

    // Move to the next scanline
    switch (++vCount) {
    case 160: // End of visible scanlines
        // Stop the thread now that the frame has been drawn
        if (thread) {
            running.store(false);
            thread->join();
            delete thread;
            thread = nullptr;
        }

        // Set the V-blank flag
        dispStat[1] |= BIT(0);

        // Trigger a V-blank IRQ if enabled
        if (dispStat[1] & BIT(3))
            core->interpreter[1].sendInterrupt(0);

        // Trigger V-blank DMA transfers
        core->dma[1].trigger(1);

        // Allow up to 2 framebuffers to be queued, to preserve frame pacing if emulation runs ahead
        if (frames == 0 && framebuffers.size() < 2) {
            // Copy the completed sub-framebuffer to a new framebuffer
            Buffers buffers;
            buffers.id = frameNumber;
            buffers.framebuffer = newFramebuffer(256 * 160);
            memcpy(buffers.framebuffer, core->gpu2D[0].getFramebuffer(), 256 * 160 * sizeof(uint32_t));

            // Add the frame to the queue
            mutex.lock();
            framebuffers.push(buffers);
            ready.store(true);
            mutex.unlock();
        }
        frameNumber++;

        // Update the frame count to skip frames when non-zero
        if (frames++ >= Settings::frameskip)
            frames = 0;

        // Stop execution here in case the frontend needs to do things
        core->endFrame();
        break;

    case 227: // Last scanline
        // Clear the V-blank flag
        dispStat[1] &= ~BIT(0);
        break;

    case 228: // End of frame
        // Start the next frame
        vCount = 0;
        core->gpu2D[0].reloadRegisters();

        // Start the 2D thread if enabled
        if (Settings::threads2D() && frames == 0 && !thread) {
            running.store(true);
            thread = new WorkerThread(&Gpu::drawGbaThreaded, this);
        }
        break;
    }

    // Update window flags for the next scanline
    core->gpu2D[0].updateWindows(vCount);

    // Signal that the next scanline should start drawing
    if (vCount < 160 && thread)
        drawing.store(1);

    // Check if the current scanline matches the V-counter
    if (vCount == (dispStat[1] >> 8)) {
        // Set the V-counter flag
        dispStat[1] |= BIT(2);

        // Trigger a V-counter IRQ if enabled
        if (dispStat[1] & BIT(5))
            core->interpreter[1].sendInterrupt(2);
    }
    else if (dispStat[1] & BIT(2)) {
        // Clear the V-counter flag on the next line
        dispStat[1] &= ~BIT(2);
    }

    // Reschedule the task for the next scanline
    core->schedule(GBA_SCANLINE308, 308 * 4);
}

void Gpu::scanline256() {
    if (vCount < 192) {
        if (thread && !fastFrame) {
            // Finish each engine's scanline, drawing it here if its thread hasn't started it, or waiting for it
            // Engine B comes first, since its thread shares a core and is more likely to be behind
            for (int engine = 1; engine >= 0; engine--) {
                Atomic32<int> &state = engine ? drawingB : drawing;
                int expected = 1;
                if (state.compare_exchange_strong(expected, 3)) {
                    EP_START(draw);
                    core->gpu2D[engine].drawScanline(vCount);
                    state.store(0);
                    EP_ADD(EP_DRAW2D, draw);
                    EP_INC(engine ? EC_LINES_HALF : EC_LINES_EMU);
                    HB_INC(engine ? HB_2D_EMU_LINES_B : HB_2D_EMU_LINES_A);
                }
                else {
                    EP_START(wait);
                    HB_SET(HB_EMU_WAIT, 2);
                    SpinWait spin;
                    while (state.load() != 0)
                        spin.pause();
                    HB_SET(HB_EMU_WAIT, 0);
                    EP_ADD(EP_WAIT2D, wait);
                }
            }
        }
        else if (!thread && frames == 0) {
            // Draw the current scanlines
            core->gpu2D[0].drawScanline(vCount);
            core->gpu2D[1].drawScanline(vCount);
        }

        // Trigger H-blank DMA transfers for visible scanlines (ARM9 only)
        core->dma[0].trigger(2);

        // Start a display capture at the beginning of the frame if one was requested
        if (vCount == 0 && (dispCapCnt & BIT(31)))
            displayCapture = true;

        // Perform a display capture, unless engine A's thread already did with the same settings
        // In fast 2D mode, the thread always does it when it draws the scanline
        if (displayCapture) {
            EP_START(capture);
            if (!thread || (!fastFrame && (!captureDone.load() || captureCnt != dispCapCnt)))
                captureLine(dispCapCnt, vCount, core->gpu2D[0].getRawLine(), false);

            // End the display capture after its last scanline, from the heights of the capture sizes
            static const uint16_t heights[] = { 128, 64, 128, 192 };
            if (vCount + 1 == heights[(dispCapCnt >> 20) & 0x3]) {
                displayCapture = false;
                dispCapCnt &= ~BIT(31);
            }
            EP_ADD(EP_CAPTURE, capture);
        }
    }

    // Draw 3D scanlines 48 lines in advance, if the current 3D is dirty
    // This is also when a hardware renderer starts a frame; starting at the buffer swap instead would miss 3D settings
    // that games change during V-blank
    // If the 3D parameters haven't changed since the last frame, there's no need to draw it again
    // Bit 0 of the dirty variable represents invalidation, and bit 1 represents a frame currently drawing
    if (frames == 0 && dirty3D && (core->gpu2D[0].readDispCnt() & BIT(3)) && ((vCount + 48) % 263) < 192) {
        if (vCount == 215) dirty3D = BIT(1);
        core->gpu3DRenderer.drawScanline((vCount + 48) % 263);
        if (vCount == 143) dirty3D &= ~BIT(1);
    }

    for (int i = 0; i < 2; i++) {
        // Set the H-blank flag
        dispStat[i] |= BIT(1);

        // Trigger an H-blank IRQ if enabled
        if (dispStat[i] & BIT(4))
            core->interpreter[i].sendInterrupt(1);
    }

    // Reschedule the task for the next scanline
    core->schedule(NDS_SCANLINE256, 355 * 6);
}

void Gpu::scanline355() {
    // Move to the next scanline
    switch (++vCount) {
    case 192: // End of visible scanlines
#if defined(PROFILE_EMU) && defined(HEARTBEAT)
        EmuProfile::reachedEnd = EmuProfile::sinceFrameStart(OSGetSystemTick());
#endif
        // In fast 2D mode, stop sending register writes and wait for the threads to finish the frame
        if (thread && fastFrame) {
            HB_TIMER(hbJoin);
            for (int i = 0; i < 2; i++) {
                EP_START(join);
                core->gpu2D[i].setQueue(nullptr);
                SpinWait spin;
                while (linesDone[i].load() < 192 || !queues[i].empty())
                    spin.pause();
                EP_ADD(i ? EP_JOIN2D_B : EP_JOIN2D_A, join);
            }
            HB_ADD_US(HB_2D_JOIN_US, hbJoin);
        }

        // Let the threads sleep now that the frame has been drawn
        frameActive.store(false);

        for (int i = 0; i < 2; i++) {
            // Set the V-blank flag
            dispStat[i] |= BIT(0);

            // Trigger a V-blank IRQ if enabled
            if (dispStat[i] & BIT(3))
                core->interpreter[i].sendInterrupt(0);

            // Trigger V-blank DMA transfers
            core->dma[i].trigger(1);
        }

        // Swap the buffers of the 3D engine if needed
        if (core->gpu3D.shouldSwap()) {
            EP_START(swap);
            core->gpu3D.swapBuffers();
            EP_ADD(EP_SWAP3D, swap);
        }

        // Allow up to 2 framebuffers to be queued, to preserve frame pacing if emulation runs ahead
        if (frames == 0 && framebuffers.size() < 2) {
            EP_START(frameCopy);
            Buffers buffers;
            if (drawBuffer) {
                // Use the buffer the engines drew into, fixing the screen order if display swap changed mid-frame
                buffers.framebuffer = drawBuffer;
                drawBuffer = nullptr;
                if (!(powCnt1 & BIT(0))) // LCDs disabled
                    memset(buffers.framebuffer, 0, 256 * 192 * 2 * sizeof(uint32_t));
                else if (bool(powCnt1 & BIT(15)) != drawSwap)
                    std::swap_ranges(buffers.framebuffer, &buffers.framebuffer[256 * 192], &buffers.framebuffer[256 * 192]);
            }
            else {
                // Copy the completed sub-framebuffers to a new framebuffer
                buffers.framebuffer = newFramebuffer(256 * 192 * 2);
                if (powCnt1 & BIT(0)) { // LCDs enabled
                    if (powCnt1 & BIT(15)) { // Display swap
                        memcpy(&buffers.framebuffer[0], core->gpu2D[0].getFramebuffer(), 256 * 192 * sizeof(uint32_t));
                        memcpy(&buffers.framebuffer[256 * 192], core->gpu2D[1].getFramebuffer(), 256 * 192 * sizeof(uint32_t));
                    }
                    else {
                        memcpy(&buffers.framebuffer[0], core->gpu2D[1].getFramebuffer(), 256 * 192 * sizeof(uint32_t));
                        memcpy(&buffers.framebuffer[256 * 192], core->gpu2D[0].getFramebuffer(), 256 * 192 * sizeof(uint32_t));
                    }
                }
                else {
                    memset(buffers.framebuffer, 0, 256 * 192 * 2 * sizeof(uint32_t));
                }
            }

            // Copy the upscaled 3D output to a new buffer if enabled
            if (Settings::highRes3D && (core->gpu2D[0].readDispCnt() & BIT(3))) {
                // Use the copy a hardware renderer made if there is one, since reading its output is slow
                HB_TIMER(hbHiRes);
                EP_START(hiRes);
                // If the 3D frame wasn't drawn again, it's the same as the last one, so that can be shown again
                if (!(buffers.hiRes3D = core->gpu3DRenderer.takeHardwareCopy()) &&
                        !(buffers.hiRes3D = core->gpu3DRenderer.reuseHardwareCopy())) {
                    buffers.hiRes3D = newFramebuffer(256 * 192 * 4);
                    uint32_t *frame = core->gpu3DRenderer.getLine(0);
                    core->gpu3DRenderer.forgetHardwareRows(0, 384);
                    memcpy(buffers.hiRes3D, frame, 256 * 192 * 4 * sizeof(uint32_t));
                }
                HB_ADD_US(HB_EMU_HIRES_US, hbHiRes);
                EP_ADD(EP_HIRES, hiRes);
                buffers.top3D = (powCnt1 & BIT(15));
            }

            buffers.id = frameNumber;
#ifdef FRAME_CAPTURE
            // Record both screens as the 2D engine finished them
            if (FrameCapture::Frame *capture = FrameCapture::slot(FrameCapture::emuFrame)) {
                capture->emuFrame = FrameCapture::emuFrame;
                memcpy(capture->screens, buffers.framebuffer, sizeof(capture->screens));
            }
#endif

            // Add the frame to the queue
            mutex.lock();
            framebuffers.push(buffers);
            ready.store(true);
            mutex.unlock();
            EP_ADD(EP_FRAMECOPY, frameCopy);
        }

        frameNumber++;
#ifdef FRAME_CAPTURE
        FrameCapture::emuFrame++;
#endif

        // Drop the drawn buffer if the frame wasn't queued, and let the engines draw into their own buffers again
        freeFramebuffer(drawBuffer);
        drawBuffer = nullptr;
        core->gpu2D[0].setFramebuffer(nullptr);
        core->gpu2D[1].setFramebuffer(nullptr);

        // Update the frame count to skip frames when non-zero
        if (frames++ >= Settings::frameskip)
            frames = 0;

        // Apply cheats and stop execution in case the frontend needs to do things
        core->actionReplay.applyCheats();
        core->endFrame();
        break;

    case 262: // Last scanline
        // Clear the V-blank flags
        dispStat[0] &= ~BIT(0);
        dispStat[1] &= ~BIT(0);
        break;

#if defined(PROFILE_EMU) && defined(HEARTBEAT)
    case 96:
        // Note how far the 2D threads got by the middle of the frame
        EmuProfile::reachedMiddle = EmuProfile::sinceFrameStart(OSGetSystemTick());
        EmuProfile::draw2D[0].atMiddle = linesDone[0].load();
        EmuProfile::draw2D[1].atMiddle = linesDone[1].load();
        break;
#endif

    case 263: // End of frame
#if defined(PROFILE_EMU) && defined(HEARTBEAT)
        EmuProfile::frameStart = OSGetSystemTick();
        EmuProfile::draw2D[0] = EmuProfile::draw2D[1] = {};
#endif
        // Start the next frame
        vCount = 0;
        core->gpu2D[0].reloadRegisters();
        core->gpu2D[1].reloadRegisters();

        // Draw the frame straight into a buffer for the frontend if it won't be skipped, saving a copy at V-blank
        // Each engine gets the half it's expected to be shown in, based on the current display swap
        if (frames == 0 && !drawBuffer) {
            drawBuffer = newFramebuffer(256 * 192 * 2);
            drawSwap = (powCnt1 & BIT(15));
            core->gpu2D[0].setFramebuffer(&drawBuffer[drawSwap ? 0 : (256 * 192)]);
            core->gpu2D[1].setFramebuffer(&drawBuffer[drawSwap ? (256 * 192) : 0]);
        }

        // Start the 2D threads if enabled, one per engine, on different cores where threads can be placed
        // Engine A's thread has core 0 to itself, since it also does display captures and waits on 3D
        // Otherwise stop them, since they're kept between frames
        EP_START(start);
        if (Settings::threads2D() && frames == 0) {
            if (!thread) {
                running.store(true);
                thread = new WorkerThread(WorkerThread::CORE_0, &Gpu::drawThreaded, this, 0);
                threadB = new WorkerThread(WorkerThread::CORE_2, &Gpu::drawThreaded, this, 1);
            }

            // In fast 2D mode, sync the drawing copies and send them register writes until the frame ends
            if ((fastFrame = Settings::fast2D)) {
                for (int i = 0; i < 2; i++) {
                    if (!drawers[i]) drawers[i] = new Gpu2D(core, i);
                    drawers[i]->copyRegisters(core->gpu2D[i]);
                    drawers[i]->setFramebuffer(core->gpu2D[i].getFramebuffer());
#ifdef FAST2D_CHECK
                    fastTargets[i] = drawers[i]->getFramebuffer();
#endif
                    linesDone[i].store(0);
                    core->gpu2D[i].setQueue(&queues[i]);
                }
            }
            std::lock_guard<std::mutex> guard(frameMutex);
            frameActive.store(true);
            frameCond.notify_all();
        }
        else if (thread) {
            stopThreads();
        }
        EP_ADD(EP_JOIN2D, start);
        break;
    }

    // Update window flags for the next scanline
    core->gpu2D[0].updateWindows(vCount);
    core->gpu2D[1].updateWindows(vCount);

    // Signal that the next scanline should start drawing, along with the display capture settings if it's captured
    // The capture is checked again at H-blank, in case its settings changed
    // In fast 2D mode, the scanline is sent to the threads with the capture settings, since they draw behind
    if (vCount < 192 && thread) {
        bool capturing = displayCapture || (vCount == 0 && (dispCapCnt & BIT(31)));
        if (fastFrame) {
            for (int i = 0; i < 2; i++)
                queues[i].push({ Gpu2DEntry::LINE, uint8_t(i == 0 && capturing), dispCapCnt, uint32_t(vCount) });
        }
        else {
            captureCnt = capturing ? dispCapCnt : 0;
            captureDone.store(false);
            drawing.store(1);
            drawingB.store(1);
        }
    }

    for (int i = 0; i < 2; i++) {
        // Check if the current scanline matches the V-counter
        if (vCount == ((dispStat[i] >> 8) | ((dispStat[i] & BIT(7)) << 1))) {
            // Set the V-counter flag
            dispStat[i] |= BIT(2);

            // Trigger a V-counter IRQ if enabled
            if (dispStat[i] & BIT(5))
                core->interpreter[i].sendInterrupt(2);
        }
        else if (dispStat[i] & BIT(2)) {
            // Clear the V-counter flag on the next line
            dispStat[i] &= ~BIT(2);
        }

        // Clear the H-blank flag
        dispStat[i] &= ~BIT(1);
    }

    // Reschedule the task for the next scanline
    core->schedule(NDS_SCANLINE355, 355 * 6);
}

void Gpu::drawGbaThreaded() {
    while (running.load()) {
        // Wait until the next scanline should start
        SpinWait spin;
        while (drawing.load() != 1) {
            if (!running.load()) return;
            spin.pause();
        }

        // Draw the current scanline
        core->gpu2D[0].drawGbaScanline(vCount);

        // Signal that the scanline is finished
        drawing.store(0);
    }
}

void Gpu::drawThreaded(int engine) {
    if (engine == 0) HB_SET_CORE(HB_2D_CORE);
    Atomic32<int> &state = engine ? drawingB : drawing;
    SpinWait spin;
    while (running.load()) {
        // Between frames, sleep until the next one starts instead of spinning
        if (!frameActive.load()) {
            std::unique_lock<std::mutex> lock(frameMutex);
            frameCond.wait(lock, [&] { return frameActive.load() || !running.load(); });
            spin.reset();
            continue;
        }

        // In fast 2D mode, replay what the emulator sent, drawing scanlines as they come up
        if (fastFrame) {
            if (fastStep(engine)) spin.reset(); else spin.pause();
            continue;
        }

        // Otherwise, wait until the next scanline should start, and claim it unless the emulator thread already did
        int expected = 1;
        if (!state.compare_exchange_weak(expected, 2)) {
            spin.pause();
            continue;
        }
        spin.reset();

        // Draw the engine's scanline, and for engine A, the display capture that uses it, then signal that it's finished
        HB_INC(HB_2D_LINES);
        core->gpu2D[engine].drawScanline(vCount);
        if (engine) EP_INC(EC_LINES_THREAD);
        else if (captureCnt) captureDone.store(captureLine(captureCnt, vCount, core->gpu2D[0].getRawLine(), true));
        state.store(0);

        // Engine A's thread also draws engine B's scanline if its thread hasn't started it, since that one shares a core
        // with the frontend and often falls behind
        expected = 1;
        if (engine == 0 && drawingB.compare_exchange_strong(expected, 2)) {
            HB_INC(HB_2D_LINES);
            core->gpu2D[1].drawScanline(vCount);
            EP_INC(EC_LINES_THREAD);
            drawingB.store(0);
        }
    }
}

bool Gpu::fastStep(int engine) {
    // Handle an entry for the thread's engine, or for the other one if it has nothing to do, since the other thread can
    // be held up by busier threads on its core; each engine's entries are handled by one thread at a time, in order
    for (int i = 0; i < 2; i++) {
        int target = engine ^ i;
        if (queues[target].empty() || !drawerLocks[target].try_lock()) continue;
        bool handled = fastEntry(target);
        drawerLocks[target].unlock();
        if (handled) return true;
    }
    return false;
}

bool Gpu::fastEntry(int engine) {
    // Replay the next register write sent to an engine, or draw the next scanline with its drawing copy
    Gpu2DEntry entry;
    if (!queues[engine].pop(entry)) return false;
    Gpu2D *drawer = drawers[engine];
#ifdef FAST2D_CHECK
    // Debug builds stop with the details as soon as a drawing copy's framebuffer changes or a bad scanline comes in
    recentEntries[engine][recentPos[engine]++ & 7] = entry;
    auto check = [&](const char *when) {
        if (drawer->getFramebuffer() == fastTargets[engine] && (entry.type != Gpu2DEntry::LINE || entry.value < 192))
            return;
        char message[768];
        int length = __os_snprintf(message, sizeof(message), "Fast 2D check failed %s, engine %d\n"
            "framebuffer %08X, expected %08X, drawer %08X, entry type %u index %u value %u\nRecent entries:",
            when, engine, (uint32_t)drawer->getFramebuffer(), (uint32_t)fastTargets[engine], (uint32_t)drawer,
            entry.type, entry.index, entry.value);
        for (int i = 0; i < 8; i++) {
            Gpu2DEntry &e = recentEntries[engine][(recentPos[engine] + i) & 7];
            length += __os_snprintf(message + length, sizeof(message) - length, "%s %u/%u/%08X/%u", (i % 4) ? "" : "\n",
                e.type, e.index, e.mask, e.value);
        }
        OSFatal(message);
    };
    check("before");
#endif
    if (entry.type != Gpu2DEntry::LINE) {
        drawer->replay(entry);
        return true;
    }

    // Draw the scanline with the display capture settings from its start, and capture it if needed
    // Captures that can't access VRAM directly off the emulator thread are skipped
    HB_INC(HB_2D_LINES);
#if defined(PROFILE_EMU) && defined(HEARTBEAT)
    uint32_t lineStart = OSGetSystemTick(), waitStart = Heartbeat::values[HB_3D_WAIT_US];
#endif
    drawer->updateWindows(entry.value);
    drawer->drawScanline(entry.value, entry.mask);
    if (entry.index) captureLine(entry.mask, entry.value, drawer->getRawLine(), true);
#if defined(PROFILE_EMU) && defined(HEARTBEAT)
    EmuProfile::drawn2D(engine, OSGetCoreId() != (engine ? 2 : 0), lineStart, Heartbeat::values[HB_3D_WAIT_US] - waitStart);
#endif
#ifdef FAST2D_CHECK
    check("after");
#endif
    linesDone[engine].store(entry.value + 1);
    return true;
}

bool Gpu::captureLine(uint32_t cnt, int line, const uint32_t *rawLine, bool directOnly) {
    // Capture a scanline with the given capture settings, from engine A's output that was just drawn
    // Off the emulator thread, VRAM can only be accessed directly, so it gives up if that's not possible

    // Determine the capture size
    static const uint16_t sizes[] = { 128, 128, 256, 64, 256, 128, 256, 192 };
    const uint16_t *size = &sizes[(cnt >> 19) & 0x6];
    const uint16_t width = size[0];

    // Get the VRAM destination address for the current scanline
    uint32_t base = 0x6800000 + ((cnt & 0x00030000) >> 16) * 0x20000;
    uint32_t writeOffset = ((cnt & 0x000C0000) >> 3) + line * width * 2;

    // Get direct pointers to a scanline's VRAM if it's mapped and within a page, or null to use memory accesses
    auto linePtr = [&](uint32_t offset, bool write) -> uint8_t* {
        offset &= 0x1FFFF;
        if ((offset & 0xFFF) + width * 2 > 0x1000) return nullptr;
        return write ? core->memory.getWritePtr(0, base + offset) : core->memory.getReadPtr(0, base + offset);
    };
    uint8_t *dst = linePtr(writeOffset, true);
    if (directOnly && !dst) return false;

    // Copy the source contents to memory as a 15-bit bitmap
    switch ((cnt & 0x60000000) >> 29) { // Capture source
    case 0: { // Source A
        // Choose from 2D engine A or the 3D engine
        // In high-res mode, skip every other pixel when capturing 3D
        const uint32_t *source = (cnt & BIT(24)) ? core->gpu3DRenderer.getLine(line) : rawLine;
        bool resShift = (Settings::highRes3D && (cnt & BIT(24)));

        // Copy a scanline to memory
        for (int i = 0; i < width; i++) {
            uint16_t color = rgb6ToRgb5(source[i << resShift]);
            if (dst) writeLsb<uint16_t>(&dst[i * 2], color);
            else core->memory.write<uint16_t>(0, base + ((writeOffset + i * 2) & 0x1FFFF), color);
        }
        break;
    }

    case 1: { // Source B
        if (cnt & BIT(25)) {
            LOG_CRIT("Unimplemented display capture source: display FIFO\n");
            break;
        }

        // Get the VRAM source address for the current scanline
        uint32_t readOffset = ((cnt & 0x0C000000) >> 11) + line * width * 2;
        uint8_t *src = linePtr(readOffset, false);
        if (directOnly && !src) return false;

        // Copy a scanline to memory
        for (int i = 0; i < width; i++) {
            uint16_t color = src ? readLsb<uint16_t>(&src[i * 2]) :
                core->memory.read<uint16_t>(0, base + ((readOffset + i * 2) & 0x1FFFF));
            if (dst) writeLsb<uint16_t>(&dst[i * 2], color);
            else core->memory.write<uint16_t>(0, base + ((writeOffset + i * 2) & 0x1FFFF), color);
        }
        break;
    }

    default: { // Blended
        if (cnt & BIT(25)) {
            LOG_CRIT("Unimplemented display capture source: display FIFO\n");
            break;
        }

        // Choose from 2D engine A or the 3D engine
        // In high-res mode, skip every other pixel when capturing 3D
        const uint32_t *source = (cnt & BIT(24)) ? core->gpu3DRenderer.getLine(line) : rawLine;
        bool resShift = (Settings::highRes3D && (cnt & BIT(24)));

        // Get the VRAM source address for the current scanline
        uint32_t readOffset = ((cnt & 0x0C000000) >> 11) + line * width * 2;
        uint8_t *src = linePtr(readOffset, false);
        if (directOnly && !src) return false;

        // Build a table of blended color channels for the current blending factors, indexed by the two sources
        uint8_t eva = std::min((cnt >> 0) & 0x1F, 16U);
        uint8_t evb = std::min((cnt >> 8) & 0x1F, 16U);
        if (blendFactors != ((eva << 8) | evb)) {
            blendFactors = (eva << 8) | evb;
            for (int c1 = 0; c1 < 32; c1++)
                for (int c2 = 0; c2 < 32; c2++)
                    blendTable[(c1 << 5) | c2] = std::min((c1 * eva + c2 * evb) / 16, 31);
        }

        // Copy a scanline to memory
        for (int i = 0; i < width; i++) {
            // Get colors from the two sources
            uint16_t c1 = rgb6ToRgb5(source[i << resShift]);
            uint16_t c2 = src ? readLsb<uint16_t>(&src[i * 2]) :
                core->memory.read<uint16_t>(0, base + ((readOffset + i * 2) & 0x1FFFF));

            // Blend the color values
            uint8_t r = blendTable[((c1 << 5) & 0x3E0) | ((c2 >> 0) & 0x1F)];
            uint8_t g = blendTable[((c1 >> 0) & 0x3E0) | ((c2 >> 5) & 0x1F)];
            uint8_t b = blendTable[((c1 >> 5) & 0x3E0) | ((c2 >> 10) & 0x1F)];

            uint16_t color = BIT(15) | (b << 10) | (g << 5) | r;
            if (dst) writeLsb<uint16_t>(&dst[i * 2], color);
            else core->memory.write<uint16_t>(0, base + ((writeOffset + i * 2) & 0x1FFFF), color);
        }
        break;
    }}
    return true;
}

void Gpu::enterGbaMode() {
    // Stop the NDS 2D threads, since they're kept between frames but GBA mode has its own, and drop any frame in progress
    stopThreads();
    frameActive.store(false);
    fastFrame = false;
    drawing.store(0);
    drawingB.store(0);
    for (int i = 0; i < 2; i++)
        core->gpu2D[i].setQueue(nullptr);

    // Start a new frame if the scanline is past where GBA mode would ever start one
    if (vCount >= 228) {
        vCount = 0;
        core->gpu2D[0].reloadRegisters();
    }
}

void Gpu::stopThreads() {
    // Stop the 2D threads and wake them if they're sleeping, then wait for them to finish
    {
        std::lock_guard<std::mutex> guard(frameMutex);
        running.store(false);
        frameCond.notify_all();
    }
    for (WorkerThread **worker : { &thread, &threadB }) {
        if (!*worker) continue;
        (*worker)->join();
        delete *worker;
        *worker = nullptr;
    }
}

void Gpu::writeDispStat(bool cpu, uint16_t mask, uint16_t value) {
    // Write to one of the DISPSTAT registers
    mask &= 0xFFB8;
    dispStat[cpu] = (dispStat[cpu] & ~mask) | (value & mask);
}

void Gpu::writeDispCapCnt(uint32_t mask, uint32_t value) {
    // Write to the DISPCAPCNT register
    mask &= 0xEF3F1F1F;
    dispCapCnt = (dispCapCnt & ~mask) | (value & mask);
}

void Gpu::writePowCnt1(uint16_t mask, uint16_t value) {
    // Write to the POWCNT1 register
    mask &= 0x820F;
    powCnt1 = (powCnt1 & ~mask) | (value & mask);
}
