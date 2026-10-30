/*
    Copyright 2019-2026 Hydr8gon

    This file is part of NooDS.

    NooDS is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    NooDS is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with NooDS. If not, see <https://www.gnu.org/licenses/>.
*/

#include <cstring>
#include <vector>

#include "../core.h"
#include "../emu_profile.h"

Gpu3DRenderer::Gpu3DRenderer(Core *core): core(core) {
    // Mark the scanlines as ready to start
    // This is mainly in case 3D is requested before the threads have a chance to start
    for (int i = 0; i < 192 * 2; i++)
        ready[i].store(3);
}

Gpu3DHardware *Gpu3DRenderer::hardware = nullptr;

void Gpu3DRenderer::finishHardwareFrame() {
    if (!hardwareFrame) return;
    SpinWait spin;
    while (ready[191].load() < 3) spin.pause();
}

Gpu3DRenderer::~Gpu3DRenderer() {
    // Let a hardware frame finish, since it writes to this renderer
    finishHardwareFrame();

    // Clean up the threads
    for (size_t i = 0; i < threads.size(); i++) {
        threads[i]->join();
        delete threads[i];
    }
}

void Gpu3DRenderer::saveState(FILE *file) {
    // Write state data to the file
    fwrite(&disp3DCnt, sizeof(disp3DCnt), 1, file);
    fwrite(edgeColor, 2, sizeof(edgeColor) / 2, file);
    fwrite(&clearColor, sizeof(clearColor), 1, file);
    fwrite(&clearDepth, sizeof(clearDepth), 1, file);
    fwrite(&fogColor, sizeof(fogColor), 1, file);
    fwrite(&fogOffset, sizeof(fogOffset), 1, file);
    fwrite(fogTable, 1, sizeof(fogTable), file);
    fwrite(toonTable, 2, sizeof(toonTable) / 2, file);
    fwrite(&alphaTestRef, sizeof(alphaTestRef), 1, file);
}

void Gpu3DRenderer::loadState(FILE *file) {
    // Read state data from the file
    fread(&disp3DCnt, sizeof(disp3DCnt), 1, file);
    fread(edgeColor, 2, sizeof(edgeColor) / 2, file);
    fread(&clearColor, sizeof(clearColor), 1, file);
    fread(&clearDepth, sizeof(clearDepth), 1, file);
    fread(&fogColor, sizeof(fogColor), 1, file);
    fread(&fogOffset, sizeof(fogOffset), 1, file);
    fread(fogTable, 1, sizeof(fogTable), file);
    fread(toonTable, 2, sizeof(toonTable) / 2, file);
    fread(&alphaTestRef, sizeof(alphaTestRef), 1, file);
}

uint32_t Gpu3DRenderer::rgba5ToRgba6(uint32_t color) {
    // Convert an RGBA5 value to an RGBA6 value (the way the 3D engine does it)
    uint8_t r = ((color >> 0) & 0x1F) * 2; if (r > 0) r++;
    uint8_t g = ((color >> 5) & 0x1F) * 2; if (g > 0) g++;
    uint8_t b = ((color >> 10) & 0x1F) * 2; if (b > 0) b++;
    uint8_t a = ((color >> 15) & 0x1F) * 2; if (a > 0) a++;
    return (a << 18) | (b << 12) | (g << 6) | r;
}

uint32_t *Gpu3DRenderer::getLine(int line) {
    // Get 2 lines when high-res is enabled, to ensure they're both finished
    if (resShift) {
        uint32_t *data = getLine1(line * 2);
        getLine1(line * 2 + 1);
        return data;
    }

    return getLine1(line);
}

uint32_t *Gpu3DRenderer::getLine1(int line) {
    // If a thread is falling behind, see if this thread can help out instead of waiting around
    // Threads go back for the final pass after drawing their next scanline, so check 2 scanlines ahead
    // There's nothing to help with when the hardware renderer is drawing the frame
    if (!hardwareFrame && ready[line].load() < 3 && line + activeThreads * 2 < (192 << resShift)) {
        int next = line + activeThreads * 2;
        switch (ready[next].exchange(1)) {
        case 0: {
            // Draw the scanline if it hasn't been started yet
            EP_START_EMU(help);
            drawScanline1(next);
            ready[next].store(2);
            EP_ADD_EMU(EP_3D_HELP, help);
            break;
        }

        case 2:
            // If the thread somehow caught up, restore the scanline's state
            if (ready[next].exchange(2) == 3)
                ready[next].store(3);
            break;

        case 3:
            // If the thread somehow caught up, restore the scanline's state
            ready[next].store(3);
            break;
        }
    }

    // Wait until a scanline is ready, and then return it
    HB_SET(HB_EMU_WAIT, 3);
    EP_START_EMU(wait);
    HB_TIMER(hbWait);
    SpinWait spin;
    while (ready[line].load() < 3) spin.pause();
    HB_ADD_US(HB_3D_WAIT_US, hbWait);
    EP_ADD_EMU(EP_3D_WAIT, wait);
    HB_SET(HB_EMU_WAIT, 0);
#ifdef GPU_DUMP
    // Save the finished 3D layer of frames whose polygons were dumped, as the 2D engine gets it
    if (!dumpLayerPath.empty()) {
        static uint32_t layer[256 * 192];
        memcpy(&layer[line * 256], &framebuffer[0][line * 256 * 2], 256 * sizeof(uint32_t));
        if (line == 191) {
            if (FILE *file = fopen(dumpLayerPath.c_str(), "wb")) {
                for (int i = 0; i < 256 * 192; i++) {
                    uint8_t b[4] = { uint8_t(layer[i] >> 24), uint8_t(layer[i] >> 16), uint8_t(layer[i] >> 8), uint8_t(layer[i]) };
                    fwrite(b, 4, 1, file);
                }
                fclose(file);
            }
            dumpLayerPath.clear();
        }
    }
#endif
    return &framebuffer[0][line * 256 * 2];
}

void Gpu3DRenderer::drawScanline(int line) {
    if (line == 0) {
#ifdef GPU_DUMP
        // Debug builds save a frame's polygons and vertices every 60 frames, cycling through 4 files for each
        // renderer, as plain big-endian words: polygon count, then for each the first vertex, size, crossed flag and
        // texture size and address, then for each vertex x, y, z, w, s, t, and color
        static uint32_t dumpFrames = 0;
        if (++dumpFrames % 60 == 0) {
            bool hw = hardware && Settings::hardware3D && !Settings::highRes3D;
            std::string path = Settings::basePath + (hw ? "/polydump_gpu" : "/polydump_sw") +
                std::to_string((dumpFrames / 60) % 4) + ".bin";
            dumpLayerPath = Settings::basePath + (hw ? "/layer_gpu" : "/layer_sw") +
                std::to_string((dumpFrames / 60) % 4) + ".bin";
            if (FILE *file = fopen(path.c_str(), "wb")) {
                Gpu3D &g = core->gpu3D;
                auto put = [&](uint32_t value) {
                    uint8_t b[4] = { uint8_t(value >> 24), uint8_t(value >> 16), uint8_t(value >> 8), uint8_t(value) };
                    fwrite(b, 4, 1, file);
                };
                put(g.polygonCountOut);
                uint32_t vertexCount = 0;
                for (int i = 0; i < g.polygonCountOut; i++) {
                    _Polygon &p = g.polygonsOut[i];
                    put(p.vertices); put(p.size); put(p.crossed); put(p.sizeS); put(p.sizeT); put(p.textureAddr);
                    vertexCount = std::max<uint32_t>(vertexCount, p.vertices + p.size);
                }
                put(vertexCount);
                for (uint32_t i = 0; i < vertexCount; i++) {
                    Vertex &v = g.verticesOut[i];
                    put(v.x); put(v.y); put(v.z); put(v.w); put(v.s); put(v.t); put(v.color);
                }
                fclose(file);
            }
        }
#endif

        // Calculate the scanline bounds for each polygon
        for (int i = 0; i < core->gpu3D.polygonCountOut; i++) {
            polygonTop[i] = 192 * 2;
            polygonBot[i] = 0 * 2;

            _Polygon *polygon = &core->gpu3D.polygonsOut[i];
            for (int j = 0; j < polygon->size; j++) {
                Vertex *vertex = &core->gpu3D.verticesOut[polygon->vertices + j];
                if (vertex->y < polygonTop[i]) polygonTop[i] = vertex->y;
                if (vertex->y > polygonBot[i]) polygonBot[i] = vertex->y;
            }

            // Allow horizontal line polygons to be drawn
            if (polygonTop[i] == polygonBot[i]) polygonBot[i]++;
        }

#ifdef HEARTBEAT
        // Count which 3D features are used, to see what a renderer needs to support
        HB_INC(HB_3D_FRAMES);
        Heartbeat::values[HB_3D_FLAGS] |= disp3DCnt;
        Heartbeat::values[HB_3D_POLYS] += core->gpu3D.polygonCountOut;
        for (int i = 0; i < core->gpu3D.polygonCountOut; i++) {
            _Polygon *polygon = &core->gpu3D.polygonsOut[i];
            if (polygon->alpha == 0) HB_INC(HB_3D_WIREFRAME);
            if (polygon->mode == 2) HB_INC((disp3DCnt & BIT(1)) ? HB_3D_HIGHLIGHT : HB_3D_TOON);
            if (polygon->mode == 3) HB_INC(polygon->id ? HB_3D_SHADOWS : HB_3D_SHADOW_MASKS);
        }
#endif

        // List the polygons on each scanline in order, so drawing a scanline doesn't have to check every polygon
        // Each scanline's list starts where the previous one ends, sized by counting the polygons on each scanline
        int counts[192 * 2 + 1] = {};
        int total = 0;
        for (int i = 0; i < core->gpu3D.polygonCountOut; i++) {
            int top = std::max(polygonTop[i], 0), bot = std::min(polygonBot[i], 192 * 2);
            for (int j = top; j < bot; j++) counts[j]++;
            total += std::max(bot - top, 0);
        }
        linePolygons.resize(total);
        for (int j = 0, start = 0; j <= 192 * 2; j++) {
            lineStarts[j] = start;
            start += counts[j];
            counts[j] = lineStarts[j];
        }
        for (int i = 0; i < core->gpu3D.polygonCountOut; i++) {
            int top = std::max(polygonTop[i], 0), bot = std::min(polygonBot[i], 192 * 2);
            for (int j = top; j < bot; j++) linePolygons[counts[j]++] = i;
        }

        // Update the resolution shift for the next frame
        resShift = Settings::highRes3D;

        // Clean up any existing threads
        EP_START(join);
        for (size_t i = 0; i < threads.size(); i++) {
            threads[i]->join();
            delete threads[i];
        }
        threads.clear();

        // Render the frame on the host's GPU if enabled, which marks scanlines as done when they're ready
        // It only renders at the native resolution so far
        // The last hardware frame has to finish first, since the 2D engine might not have waited for all of it
        HB_TIMER(hbWait);
        finishHardwareFrame();
        HB_ADD_US(HB_EMU_3D_WAIT_US, hbWait);
        hardwareFrame = hardware && Settings::hardware3D && !resShift;
        if (hardwareFrame) {
            activeThreads = 0;
            for (int i = 0; i < 192; i++)
                ready[i].store(0);
            hardware->render(this);
            EP_ADD(EP_JOIN3D, join);
            return;
        }

        // Set up threaded 3D rendering if enabled
        if ((activeThreads = Settings::threaded3D & 0xF)) {
            // Mark the scanlines as not ready
            for (int i = 0; i < (192 << resShift); i++)
                ready[i].store(0);

            // Create threads to draw the scanlines
            for (uint8_t i = 0; i < activeThreads; i++)
                threads.push_back(new WorkerThread(&Gpu3DRenderer::drawThreaded, this, i));
        }
        EP_ADD(EP_JOIN3D, join);
    }

    // Draw scanlines normally when threading is disabled
    if (activeThreads == 0 && !hardwareFrame) {
        if (resShift) {
            // Draw two scanlines at a time when high-res is enabled
            for (int i = line * 2; i < line * 2 + 2; i++) {
                drawScanline1(i);
                if (i > 0) finishScanline(i - 1);
                if (i == 383) finishScanline(383);
            }
        }
        else {
            // Draw one scanline at a time
            drawScanline1(line);
            if (line > 0) finishScanline(line - 1);
            if (line == 191) finishScanline(191);
        }
    }
}

#ifdef GPU_DUMP
void Gpu3DRenderer::softwareFrame(uint32_t *out) {
    // Draw the current frame in software for debug builds to compare with a hardware renderer
    for (int i = 0; i < 192; i++) {
        drawScanline1(i);
        if (i > 0) finishScanline(i - 1);
    }
    finishScanline(191);
    for (int y = 0; y < 192; y++)
        memcpy(&out[y * 256], &framebuffer[0][y * 256 * 2], 256 * sizeof(uint32_t));
}
#endif

void Gpu3DRenderer::drawThreaded(int thread) {
    // Draw the 3D scanlines in a threaded sequence
    // The amount of scanlines skipped per thread depends on the number of active threads
    // Together, they render the entire 3D image
    int i, end = 192 << resShift;
    for (i = thread; i < end; i += activeThreads) {
        switch (ready[i].exchange(1)) {
        case 0: {
            // Draw a scanline if it hasn't been started, save for the final pass
            EP_START(draw);
            drawScanline1(i);
            RP_ADD(thread, RP_DRAW, draw);
            ready[i].store(2);
            break;
        }

        case 2:
            // Restore the scanline's state if it was already drawn
            ready[i].store(2);
            break;
        }

        if (i < activeThreads) continue;
        int prev = i - activeThreads;

        // Wait for this thread's previous scanline and its surrounding scanlines to be drawn
        EP_START(wait);
        SpinWait spin;
        while ((prev > 0 && ready[prev - 1].load() < 2) || ready[prev].load() < 2 || ready[prev + 1].load() < 2)
            spin.pause();
        RP_ADD(thread, RP_WAIT, wait);

        // Finish this thread's previous scanline
        EP_START(finish);
        finishScanline(prev);
        RP_ADD(thread, RP_FINISH, finish);
        ready[prev].store(3);
    }

    int prev = i - activeThreads;

    // Wait for this thread's final scanline and its surrounding scanlines to be drawn
    SpinWait spin;
    while (ready[prev - 1].load() < 2 || ready[prev].load() < 2 || (prev < 191 && ready[prev + 1].load() < 2))
        spin.pause();

    // Finish this thread's final scanline
    HB_INC(HB_3D_LINES);
    finishScanline(prev);
    ready[prev].store(3);
}

void Gpu3DRenderer::drawScanline1(int line) {
    PX_SLOTS;
    {
    PX_TIMER(PX_CLEAR);
    // Convert the clear values
    // The attribute buffer contains the polygon IDs (0-5, 6-11), transparency bit (12), fog bit (13), edge bit (14), and edge alpha (15-20)
    uint32_t color = BIT(26) | rgba5ToRgba6(((clearColor & 0x001F0000) >> 1) | (clearColor & 0x00007FFF));
    int32_t depth = (clearDepth == 0x7FFF) ? 0xFFFFFF : (clearDepth << 9);
    uint32_t attrib = ((clearColor & BIT(15)) >> 2) | ((clearColor & 0x3F000000) >> 18) | ((clearColor & 0x3F000000) >> 24) |
        (0x3F << 15) | (((clearColor & 0x001F0000) && ((clearColor & 0x001F0000) >> 16) < 31) << 12);

    // Clear the scanline buffers with the clear values
    int start = line * 256 * 2, end = start + (256 << resShift);
#ifdef __WIIU__
    // Zero whole cache blocks first, so the CPU doesn't fetch lines from memory that are about to be overwritten
    uint32_t *buffers[] = { &framebuffer[0][start], (uint32_t*)&depthBuffer[0][start], &attribBuffer[0][start] };
    for (uint32_t *buffer : buffers) {
        uintptr_t block = ((uintptr_t)buffer + 31) & ~31;
        uintptr_t last = ((uintptr_t)&buffer[end - start]) & ~31;
        for (; block < last; block += 32)
            asm volatile("dcbz 0, %0" :: "r"(block) : "memory");
    }
#endif
    for (int i = start; i < end; i++) {
        framebuffer[0][i] = color;
        depthBuffer[0][i] = depth;
        attribBuffer[0][i] = attrib;
    }

    stencilClear[line] = false;
    }

    uint16_t translucent[2048];
    int translucentCount = 0;

    // Draw the polygons on the current scanline
    _Polygon *polygons = core->gpu3D.polygonsOut;
    for (int j = lineStarts[line], end = lineStarts[line + 1]; j < end; j++) {
        // Draw solid polygons and save the translucent ones for later
        int i = linePolygons[j];
        _Polygon *polygon = &polygons[i];
        if (polygon->alpha < 0x3F || polygon->textureFmt == 1 || polygon->textureFmt == 6)
            translucent[translucentCount++] = i;
        else
            drawPolygon(line, i);
    }

    // Draw the translucent polygons
    for (int i = 0; i < translucentCount; i++)
        drawPolygon(line, translucent[i]);
}

void Gpu3DRenderer::finishScanline(int line) {
    // Perform edge marking if enabled
    if (disp3DCnt & BIT(5)) {
        int offset = line * 256 * 2;
        int w = (256 << resShift) - 1;
        int h = (192 << resShift) - 1;

        for (int i = offset; i <= offset + w; i++) {
            if (attribBuffer[0][i] & BIT(14)) { // Edge bit
                // Get the polygon IDs of the surrounding pixels
                uint32_t id[4] = {
                    (((i & w) > 0) ? attribBuffer[0][i - 1] : (clearColor >> 24)) & 0x3F, // Left
                    (((i & w) < w) ? attribBuffer[0][i + 1] : (clearColor >> 24)) & 0x3F, // Right
                    ((line > 0) ? attribBuffer[0][i - 512] : (clearColor >> 24)) & 0x3F, // Up
                    ((line < h) ? attribBuffer[0][i + 512] : (clearColor >> 24)) & 0x3F // Down
                };

                // Get the depth values of the surrounding pixels
                int32_t depth[4] = {
                    (((i & w) > 0) ? depthBuffer[0][i - 1] : ((clearDepth == 0x7FFF) ? 0xFFFFFF : (clearDepth << 9))), // Left
                    (((i & w) < w) ? depthBuffer[0][i + 1] : ((clearDepth == 0x7FFF) ? 0xFFFFFF : (clearDepth << 9))), // Right
                    ((line > 0) ? depthBuffer[0][i - 512] : ((clearDepth == 0x7FFF) ? 0xFFFFFF : (clearDepth << 9))), // Up
                    ((line < h) ? depthBuffer[0][i + 512] : ((clearDepth == 0x7FFF) ? 0xFFFFFF : (clearDepth << 9))) // Down
                };

                // Check the surrounding pixels, and mark the edge if at least one has a different ID and greater depth
                for (int j = 0; j < 4; j++) {
                    if ((attribBuffer[0][i] & 0x3F) != id[j] && depthBuffer[0][i] < depth[j]) {
                        framebuffer[0][i] = BIT(26) | rgba5ToRgba6((0x1F << 15) | edgeColor[(attribBuffer[0][i] & 0x3F) >> 3]);
                        attribBuffer[0][i] = (attribBuffer[0][i] & ~(0x3F << 15)) | (0x20 << 15);
                        break;
                    }
                }
            }
        }
    }

    // Draw fog if enabled
    if (disp3DCnt & BIT(7)) {
        uint32_t fog = rgba5ToRgba6(((fogColor & 0x001F0000) >> 1) | (fogColor & 0x00007FFF));
        int fogStep = 0x400 >> ((disp3DCnt & 0x0F00) >> 8);

        for (int layer = 0; layer < ((disp3DCnt & BIT(4)) ? 2 : 1); layer++) { // Apply to the back layer as well if anti-aliased
            int start = line * 256 * 2, end = start + (256 << resShift);
            for (int i = start; i < end; i++) {
                if (attribBuffer[layer][i] & BIT(13)) { // Fog bit
                    // Determine the fog table index for the current pixel's depth
                    int32_t offset = ((depthBuffer[layer][i] / 0x200) - fogOffset);
                    int n = (fogStep > 0) ? (offset / fogStep - 1) : ((offset > 0) ? 31 : 0);

                    // Get the fog density from the table
                    uint8_t density;
                    if (n >= 31) { // Maximum
                        density = fogTable[31];
                    }
                    else if (n < 0 || fogStep == 0) { // Minimum
                        density = fogTable[0];
                    }
                    else { // Linear interpolation
                        int m = offset % fogStep;
                        density = ((m >= 0) ? ((fogTable[n + 1] * m + fogTable[n] * (fogStep - m)) / fogStep) : fogTable[0]);
                    }

                    if (density == 127)
                        density++;

                    // Blend the fog with the pixel
                    uint8_t a = (((fog >> 18) & 0x3F) * density + ((framebuffer[layer][i] >> 18) & 0x3F) * (128 - density)) / 128;
                    if (disp3DCnt & BIT(6)) { // Only alpha
                        framebuffer[layer][i] = (framebuffer[layer][i] & ~(0x3F << 18)) | (a << 18);
                    }
                    else {
                        uint8_t r = (((fog >> 0) & 0x3F) * density + ((framebuffer[layer][i] >> 0) & 0x3F) * (128 - density)) / 128;
                        uint8_t g = (((fog >> 6) & 0x3F) * density + ((framebuffer[layer][i] >> 6) & 0x3F) * (128 - density)) / 128;
                        uint8_t b = (((fog >> 12) & 0x3F) * density + ((framebuffer[layer][i] >> 12) & 0x3F) * (128 - density)) / 128;
                        framebuffer[layer][i] = BIT(26) | (a << 18) | (b << 12) | (g << 6) | r;
                    }
                }
            }
        }
    }

    // Perform anti-aliasing if enabled
    if (disp3DCnt & BIT(4)) {
        int start = line * 256 * 2, end = start + (256 << resShift);
        for (int i = start; i < end; i++) {
            if (((attribBuffer[0][i] >> 15) & 0x3F) < 0x3F) { // Edge not opaque
                // Blend with the lower pixel, or simply set the alpha if the lower pixel has alpha 0
                if ((framebuffer[1][i] >> 18) & 0x3F)
                    framebuffer[0][i] = BIT(26) | blendColor(framebuffer[1][i], framebuffer[0][i], (attribBuffer[0][i] >> 15) & 0x3F);
                else
                    framebuffer[0][i] = (framebuffer[0][i] & ~0xFC0000) | ((attribBuffer[0][i] & 0x1F8000) << 3);
            }
        }
    }
}

uint64_t Gpu3DRenderer::hashTexture(_Polygon *polygon) {
    // Hash the data a polygon's texture is decoded from, so a hardware renderer can tell if a decoded copy is current
    // This covers the texel data, the palette, and for compressed textures, their block data and block palettes
    static const uint8_t texelBits[] = { 0, 8, 2, 4, 8, 2, 8, 16 };
    static const uint16_t paletteSizes[] = { 0, 64, 8, 32, 512, 0, 16, 0 };
    uint64_t hash = 14695981039346656037ULL;
    auto add = [&](uint8_t *data, uint32_t size) {
        if (!data) {
            hash = (hash ^ 0xFFFFFFFF) * 1099511628211ULL;
            return;
        }
        for (uint32_t i = 0; i + 4 <= size; i += 4)
            hash = (hash ^ U8TO32(data, i)) * 1099511628211ULL;
        for (uint32_t i = size & ~3; i < size; i++)
            hash = (hash ^ data[i]) * 1099511628211ULL;
    };
    auto addTexture = [&](uint32_t address, uint32_t size) {
        // Texture slots are 128KB each, so data is added a slot at a time
        while (size > 0) {
            uint32_t chunk = std::min(size, 0x20000 - (address & 0x1FFFF));
            add(getTexture(address), chunk);
            address += chunk;
            size -= chunk;
        }
    };
    auto addPalette = [&](uint32_t address, uint32_t size) {
        // Palette slots are 16KB each
        while (size > 0) {
            uint32_t chunk = std::min(size, 0x4000 - (address & 0x3FFF));
            add(getPalette(address), chunk);
            address += chunk;
            size -= chunk;
        }
    };

    uint32_t texels = polygon->sizeS * polygon->sizeT;
    addTexture(polygon->textureAddr, texels * texelBits[polygon->textureFmt] / 8);
    addPalette(polygon->paletteAddr, paletteSizes[polygon->textureFmt]);
    if (polygon->textureFmt == 5) {
        // Compressed textures have 2 bytes per 4x4 block in slot 1, each choosing a palette of up to 4 colors
        uint32_t blockAddr = 0x20000 + (polygon->textureAddr % 0x20000) / 2 + ((polygon->textureAddr / 0x20000 == 2) ? 0x10000 : 0);
        for (uint32_t i = 0; i < texels / 16; i++) {
            uint8_t *block = getTexture(blockAddr + i * 2);
            if (!block) continue;
            uint16_t palBase = U8TO16(block, 0);
            hash = (hash ^ palBase) * 1099511628211ULL;
            addPalette(polygon->paletteAddr + (palBase & 0x3FFF) * 4, 8);
        }
    }
    return hash;
}

void Gpu3DRenderer::decodeTexture(_Polygon *polygon, uint32_t *out) {
    // Decode a polygon's whole texture for a hardware renderer, as 8-bit RGBA with red in the top byte
    // Channels are expanded from 6 bits by repeating their top bits, so full values stay full
    for (int t = 0; t < polygon->sizeT; t++) {
        for (int s = 0; s < polygon->sizeS; s++) {
            uint32_t texel = readTexture(polygon, s, t);
            uint32_t r = (texel >> 0) & 0x3F, g = (texel >> 6) & 0x3F, b = (texel >> 12) & 0x3F, a = (texel >> 18) & 0x3F;
            *out++ = (((r << 2) | (r >> 4)) << 24) | (((g << 2) | (g >> 4)) << 16) | (((b << 2) | (b >> 4)) << 8) |
                ((a << 2) | (a >> 4));
        }
    }
}

uint8_t *Gpu3DRenderer::getTexture(uint32_t address) {
    // Get a pointer to texture data
    uint8_t *slot = core->memory.tex3D[address >> 17];
    return slot ? &slot[address & 0x1FFFF] : nullptr;
}

uint8_t *Gpu3DRenderer::getPalette(uint32_t address) {
    // Get a pointer to palette data
    uint8_t *slot = core->memory.pal3D[address >> 14];
    return slot ? &slot[address & 0x3FFF] : nullptr;
}

uint32_t Gpu3DRenderer::interpolateLinear(uint32_t v1, uint32_t v2, uint32_t x1, uint32_t x, uint32_t x2) {
    if (x <= x1) return v1;
    if (x >= x2) return v2;

    // Linearly interpolate a new value between the min and max values
    if (v1 <= v2)
        return v1 + (v2 - v1) * (x - x1) / (x2 - x1);
    else
        return v2 + (v1 - v2) * (x2 - x) / (x2 - x1);
}

// Steps linear interpolation across consecutive X values, using a quotient and remainder instead of dividing
// It only steps when the products in interpolateLinear can't overflow, so the results always match it exactly
struct LinearStepper {
    uint32_t v1, v2, x1, x2, next;
    uint32_t diff, den, quot, rem, stepQuot, stepRem;
    bool reverse, valid;

    LinearStepper(uint32_t v1, uint32_t v2, uint32_t x1, uint32_t x2): v1(v1), v2(v2), x1(x1), x2(x2), next(x1) {
        // Interpolate forwards from the first value or backwards from the second, like interpolateLinear
        reverse = (v1 > v2);
        diff = reverse ? (v1 - v2) : (v2 - v1);
        den = x2 - x1;
        valid = (x2 > x1) && (diff <= UINT32_MAX / den);
        if (!valid) return;
        quot = reverse ? diff : 0;
        rem = 0;
        stepQuot = diff / den;
        stepRem = diff % den;
    }

    FORCE_INLINE uint32_t get(uint32_t x) {
        // Fall back to the regular calculation if stepping isn't possible
        if (!valid || x < x1 || x >= x2)
            return Gpu3DRenderer::interpolateLinear(v1, v2, x1, x, x2);

        // Recalculate the position directly if X didn't follow on from the last call
        if (x != next) {
            uint32_t n = diff * (reverse ? (x2 - x) : (x - x1));
            quot = n / den;
            rem = n % den;
        }

        // Get the value at X, and step the numerator to the next X
        uint32_t value = reverse ? (v2 + quot) : (v1 + quot);
        if (!reverse) {
            quot += stepQuot;
            rem += stepRem;
            if (rem >= den) { rem -= den; quot++; }
        }
        else if (rem < stepRem) {
            rem += den - stepRem;
            quot -= stepQuot + 1;
        }
        else {
            rem -= stepRem;
            quot -= stepQuot;
        }
        next = x + 1;
        return value;
    }
};

uint32_t Gpu3DRenderer::interpolateLinRev(uint32_t v1, uint32_t v2, uint32_t x1, uint32_t x, uint32_t x2) {
    if (x <= x1) return v1;
    if (x >= x2) return v2;

    // Linearly interpolate a new value between the min and max values (reverse formula)
    if (v1 <= v2)
        return v2 - (v2 - v1) * (x2 - x) / (x2 - x1);
    else
        return v1 - (v1 - v2) * (x - x1) / (x2 - x1);
}

uint32_t Gpu3DRenderer::interpolateFactor(uint32_t factor, uint32_t shift, uint32_t v1, uint32_t v2) {
    // Interpolate a new value between the min and max values using a factor
    if (v1 <= v2)
        return v1 + (((v2 - v1) * factor) >> shift);
    else
        return v2 + (((v1 - v2) * ((1 << shift) - factor)) >> shift);
}

FORCE_INLINE static uint32_t blendChannel(uint32_t v1, uint32_t v2, uint32_t a) {
    // Interpolate a 6-bit channel like interpolateLinear(v1, v2, 0, a, 63), without dividing
    // The products are at most 63 * 63, and (n + 1) * 4161 >> 18 equals n / 63 for all of them
    if (a == 0) return v1;
    if (a >= 63) return v2;
    if (v1 <= v2)
        return v1 + ((((v2 - v1) * a + 1) * 4161) >> 18);
    else
        return v2 + ((((v1 - v2) * (63 - a) + 1) * 4161) >> 18);
}

uint32_t Gpu3DRenderer::blendColor(uint32_t c1, uint32_t c2, uint32_t a) {
    // Blend 2 colors by an alpha, giving the same result as interpolateColor(c1, c2, 0, a, 63)
    uint32_t r = blendChannel((c1 >> 0) & 0x3F, (c2 >> 0) & 0x3F, a);
    uint32_t g = blendChannel((c1 >> 6) & 0x3F, (c2 >> 6) & 0x3F, a);
    uint32_t b = blendChannel((c1 >> 12) & 0x3F, (c2 >> 12) & 0x3F, a);
    uint32_t a1 = (c1 >> 18) & 0x3F, a2 = (c2 >> 18) & 0x3F;
    return ((a1 > a2 ? a1 : a2) << 18) | (b << 12) | (g << 6) | r;
}

uint32_t Gpu3DRenderer::interpolateColor(uint32_t c1, uint32_t c2, uint32_t x1, uint32_t x, uint32_t x2) {
    // Apply linear interpolation separately on the RGB values
    uint32_t r = interpolateLinear((c1 >> 0) & 0x3F, (c2 >> 0) & 0x3F, x1, x, x2);
    uint32_t g = interpolateLinear((c1 >> 6) & 0x3F, (c2 >> 6) & 0x3F, x1, x, x2);
    uint32_t b = interpolateLinear((c1 >> 12) & 0x3F, (c2 >> 12) & 0x3F, x1, x, x2);
    uint32_t a = (((c1 >> 18) & 0x3F) > ((c2 >> 18) & 0x3F)) ? ((c1 >> 18) & 0x3F) : ((c2 >> 18) & 0x3F);
    return (a << 18) | (b << 12) | (g << 6) | r;
}

NO_INLINE uint32_t Gpu3DRenderer::readTexture(_Polygon *polygon, int s, int t) {
    // Handle S-coordinate overflows
    if (polygon->repeatS) {
        // Flip the S-coordinate every second repeat
        if (polygon->flipS && (s & polygon->sizeS))
            s = -1 - s;

        // Wrap the S-coordinate
        s &= polygon->sizeS - 1;
    }
    else if (s < 0) {
        // Clamp the S-coordinate on the left
        s = 0;
    }
    else if (s >= polygon->sizeS) {
        // Clamp the S-coordinate on the right
        s = polygon->sizeS - 1;
    }

    // Handle T-coordinate overflows
    if (polygon->repeatT) {
        // Flip the T-coordinate every second repeat
        if (polygon->flipT && (t & polygon->sizeT))
            t = -1 - t;

        // Wrap the T-coordinate
        t &= polygon->sizeT - 1;
    }
    else if (t < 0) {
        // Clamp the T-coordinate on the top
        t = 0;
    }
    else if (t >= polygon->sizeT) {
        // Clamp the T-coordinate on the bottom
        t = polygon->sizeT - 1;
    }

    // Decode a texel
    switch (polygon->textureFmt) {
    case 1: { // A3I5 translucent
        // Get the 8-bit palette index
        uint32_t address = polygon->textureAddr + (t * polygon->sizeS + s);
        uint8_t *data = getTexture(address);
        if (!data) return 0;
        uint8_t index = *data;

        // Get the palette
        uint8_t *palette = getPalette(polygon->paletteAddr);
        if (!palette) return 0;

        // Return the palette color
        uint16_t color = U8TO16(palette, (index & 0x1F) * 2) & ~BIT(15);
        uint8_t alpha = (index >> 5) * 4 + (index >> 5) / 2;
        return rgba5ToRgba6((alpha << 15) | color);
    }

    case 2: { // 4-color palette
        // Get the 2-bit palette index
        uint32_t address = polygon->textureAddr + (t * polygon->sizeS + s) / 4;
        uint8_t *data = getTexture(address);
        if (!data) return 0;
        uint8_t index = (*data >> ((s % 4) * 2)) & 0x03;

        // Return a transparent pixel if enabled
        if (polygon->transparent0 && index == 0)
            return 0;

        // Get the palette
        uint8_t *palette = getPalette(polygon->paletteAddr);
        if (!palette) return 0;

        // Return the palette color
        return rgba5ToRgba6((0x1F << 15) | U8TO16(palette, index * 2));
    }

    case 3: { // 16-color palette
        // Get the 4-bit palette index
        uint32_t address = polygon->textureAddr + (t * polygon->sizeS + s) / 2;
        uint8_t *data = getTexture(address);
        if (!data) return 0;
        uint8_t index = (*data >> ((s % 2) * 4)) & 0x0F;

        // Return a transparent pixel if enabled
        if (polygon->transparent0 && index == 0)
            return 0;

        // Get the palette
        uint8_t *palette = getPalette(polygon->paletteAddr);
        if (!palette) return 0;

        // Return the palette color
        return rgba5ToRgba6((0x1F << 15) | U8TO16(palette, index * 2));
    }

    case 4: { // 256-color palette
        // Get the 8-bit palette index
        uint32_t address = polygon->textureAddr + (t * polygon->sizeS + s);
        uint8_t *data = getTexture(address);
        if (!data) return 0;
        uint8_t index = *data;

        // Return a transparent pixel if enabled
        if (polygon->transparent0 && index == 0)
            return 0;

        // Get the palette
        uint8_t *palette = getPalette(polygon->paletteAddr);
        if (!palette) return 0;

        // Return the palette color
        return rgba5ToRgba6((0x1F << 15) | U8TO16(palette, index * 2));
    }

    case 5: { // 4x4 compressed
        // Get the 2-bit palette index
        int tile = (t / 4) * (polygon->sizeS / 4) + (s / 4);
        uint32_t address = polygon->textureAddr + (tile * 4 + t % 4);
        uint8_t *data = getTexture(address);
        if (!data) return 0;
        uint8_t index = (*data >> ((s % 4) * 2)) & 0x03;

        // Get the palette, using the base for the tile stored in slot 1
        address = 0x20000 + (polygon->textureAddr % 0x20000) / 2 + ((polygon->textureAddr / 0x20000 == 2) ? 0x10000 : 0);
        if (!(data = getTexture(address))) return 0;
        uint16_t palBase = U8TO16(data, tile * 2);
        uint8_t *palette = getPalette(polygon->paletteAddr + (palBase & 0x3FFF) * 4);
        if (!palette) return 0;

        // Return the palette color or a transparent or interpolated color based on the mode
        uint32_t c1, c2;
        switch ((palBase & 0xC000) >> 14) { // Interpolation mode
        case 0:
            if (index == 3) return 0;
            return rgba5ToRgba6((0x1F << 15) | U8TO16(palette, index * 2));

        case 1:
            switch (index) {
            case 2:
                c1 = rgba5ToRgba6((0x1F << 15) | U8TO16(palette, 0));
                c2 = rgba5ToRgba6((0x1F << 15) | U8TO16(palette, 2));
                return interpolateColor(c1, c2, 0, 1, 2);

            case 3:
                return 0;

            default:
                return rgba5ToRgba6((0x1F << 15) | U8TO16(palette, index * 2));
            }

        case 2:
            return rgba5ToRgba6((0x1F << 15) | U8TO16(palette, index * 2));

        case 3:
            switch (index) {
            case 2:
                c1 = rgba5ToRgba6((0x1F << 15) | U8TO16(palette, 0));
                c2 = rgba5ToRgba6((0x1F << 15) | U8TO16(palette, 2));
                return interpolateColor(c1, c2, 0, 3, 8);

            case 3:
                c1 = rgba5ToRgba6((0x1F << 15) | U8TO16(palette, 0));
                c2 = rgba5ToRgba6((0x1F << 15) | U8TO16(palette, 2));
                return interpolateColor(c1, c2, 0, 5, 8);

            default:
                return rgba5ToRgba6((0x1F << 15) | U8TO16(palette, index * 2));
            }
        }
    }

    case 6: { // A5I3 translucent
        // Get the 8-bit palette index
        uint32_t address = polygon->textureAddr + (t * polygon->sizeS + s);
        uint8_t *data = getTexture(address);
        if (!data) return 0;
        uint8_t index = *data;

        // Get the palette
        uint8_t *palette = getPalette(polygon->paletteAddr);
        if (!palette) return 0;

        // Return the palette color
        uint16_t color = U8TO16(palette, (index & 0x07) * 2) & ~BIT(15);
        uint8_t alpha = index >> 3;
        return rgba5ToRgba6((alpha << 15) | color);
    }

    default: { // Direct color
        // Get the color data
        uint8_t *data = getTexture(polygon->textureAddr);
        if (!data) return 0;

        // Return the direct color
        uint16_t color = U8TO16(data, (t * polygon->sizeS + s) * 2);
        uint8_t alpha = (color & BIT(15)) ? 0x1F : 0;
        return rgba5ToRgba6((alpha << 15) | color);
    }}
}

void Gpu3DRenderer::drawPolygon(int line, int polygonIndex) {
    PX_SLOTS;
    PX_START(spanStart);
    _Polygon *polygon = &core->gpu3D.polygonsOut[polygonIndex];

    // Get the polygon vertices
    Vertex *vertices[10];
    for (int i = 0; i < polygon->size; i++)
        vertices[i] = &core->gpu3D.verticesOut[polygon->vertices + i];

    // Unclipped quad strip polygons have their vertices crossed, so uncross them
    if (polygon->crossed)
        SWAP(vertices[2], vertices[3]);

    // Find the starting (top) vertex
    int start = 0;
    for (int i = 0; i < polygon->size; i++) {
        if (vertices[start]->y > vertices[i]->y)
            start = i;
    }

    // Set the starting edges
    int v[4] = {
        start, (start + 1) % polygon->size,
        start, (start - 1 + polygon->size) % polygon->size
    };

    // Follow the vertices forwards to the first intersecting edge
    while (vertices[v[1]]->y <= line) {
        v[0] = v[1];
        v[1] = (v[1] + 1) % polygon->size;
        if (v[0] == start) break; // Full loop, no intersection
    }

    // Follow the vertices backwards to the first intersecting edge
    while (vertices[v[3]]->y <= line) {
        v[2] = v[3];
        v[3] = (v[3] - 1 + polygon->size) % polygon->size;
        if (v[2] == start) break; // Full loop, no intersection
    }

    // Swap the edges depending on polygon orientation
    if (polygon->clockwise) {
        SWAP(v[0], v[2]);
        SWAP(v[1], v[3]);
    }

    // Rearrange the vertices so the lower Y values come first
    if (vertices[v[0]]->y > vertices[v[1]]->y) SWAP(v[0], v[1]);
    if (vertices[v[2]]->y > vertices[v[3]]->y) SWAP(v[2], v[3]);

    uint32_t x1, x2, x3, x4;
    uint32_t x1a = 0x3F, x2a = 0x3F, x3a = 0x3F, x4a = 0x3F;

    // Calculate the left edge bounds
    if (vertices[v[0]]->y == vertices[v[1]]->y) { // Horizontal
        x1 = vertices[v[0]]->x;
        x2 = vertices[v[1]]->x;

        // Rearrange the vertices so the lower X values come first
        if (vertices[v[0]]->x > vertices[v[1]]->x) {
            SWAP(v[0], v[1]);
            SWAP(x1, x2);
        }
    }
    else if (abs(vertices[v[1]]->x - vertices[v[0]]->x) > vertices[v[1]]->y - vertices[v[0]]->y) { // X-major
        // Interpolate with an extra bit of precision so the result can be rounded
        x1 = interpolateLinear(vertices[v[0]]->x << 1, vertices[v[1]]->x << 1, vertices[v[0]]->y, line, vertices[v[1]]->y) + 1;
        x2 = interpolateLinear(vertices[v[0]]->x << 1, vertices[v[1]]->x << 1, vertices[v[0]]->y, line + 1, vertices[v[1]]->y) + 1;

        bool negative = (vertices[v[0]]->x > vertices[v[1]]->x);

        // Rearrange the vertices so the lower X values come first
        if (negative) {
            SWAP(v[0], v[1]);
            SWAP(x1, x2);
        }

        // Calculate the edge alpha values if anti-aliasing is enabled
        if (disp3DCnt & BIT(4)) {
            x1a = interpolateLinear(vertices[v[0]]->y << 6, vertices[v[1]]->y << 6, vertices[v[0]]->x << 1, x1, vertices[v[1]]->x << 1) & 0x3F;
            x2a = interpolateLinear(vertices[v[0]]->y << 6, vertices[v[1]]->y << 6, vertices[v[0]]->x << 1, x2 - 2, vertices[v[1]]->x << 1) & 0x3F;

            if (negative) {
                x1a = 0x3F - x1a;
                x2a = 0x3F - x2a;
            }
        }

        x1 >>= 1;
        x2 >>= 1;
    }
    else { // Y-major
        // Interpolate with extra precision for edge alpha calculation
        // Note that negative Y-major edges seem to be interpolated in reverse from positive ones
        if (vertices[v[0]]->x > vertices[v[1]]->x)
            x1 = interpolateLinRev(vertices[v[0]]->x << 6, vertices[v[1]]->x << 6, vertices[v[0]]->y, line, vertices[v[1]]->y) - 1;
        else
            x1 = interpolateLinear(vertices[v[0]]->x << 6, vertices[v[1]]->x << 6, vertices[v[0]]->y, line, vertices[v[1]]->y);

        // Set the edge alpha values if anti-aliasing is enabled
        if (disp3DCnt & BIT(4)) {
            if (abs(vertices[v[1]]->x - vertices[v[0]]->x) == vertices[v[1]]->y - vertices[v[0]]->y)
                x2a = x1a = 0x20;
            else
                x2a = x1a = 0x3F - (x1 & 0x3F);
        }

        x2 = x1 >>= 6;
    }

    // Calculate the right edge bounds
    if (vertices[v[2]]->y == vertices[v[3]]->y) { // Horizontal
        x3 = vertices[v[2]]->x;
        x4 = vertices[v[3]]->x;

        // Rearrange the vertices so the lower X values come first
        if (vertices[v[2]]->x > vertices[v[3]]->x) {
            SWAP(v[2], v[3]);
            SWAP(x3, x4);
        }
    }
    else if (vertices[v[2]]->x == vertices[v[3]]->x) { // Vertical
        // Unless a line polygon, vertical right edges have their X reduced by 1
        x3 = vertices[v[2]]->x;
        if ((vertices[v[0]]->x != vertices[v[1]]->x || vertices[v[0]]->x != vertices[v[2]]->x) && x3 > 0) x3--;
        x4 = x3;
    }
    else if (abs(vertices[v[3]]->x - vertices[v[2]]->x) > vertices[v[3]]->y - vertices[v[2]]->y) { // X-major
        // Interpolate with an extra bit of precision so the result can be rounded
        x3 = interpolateLinear(vertices[v[2]]->x << 1, vertices[v[3]]->x << 1, vertices[v[2]]->y, line, vertices[v[3]]->y) + 1;
        x4 = interpolateLinear(vertices[v[2]]->x << 1, vertices[v[3]]->x << 1, vertices[v[2]]->y, line + 1, vertices[v[3]]->y) + 1;

        bool negative = (vertices[v[2]]->x > vertices[v[3]]->x);

        // Rearrange the vertices so the lower X values come first
        if (negative) {
            SWAP(v[2], v[3]);
            SWAP(x3, x4);
        }

        // Calculate the edge alpha values if anti-aliasing is enabled
        if (disp3DCnt & BIT(4)) {
            x3a = interpolateLinear(vertices[v[2]]->y << 6, vertices[v[3]]->y << 6, vertices[v[2]]->x << 1, x3, vertices[v[3]]->x << 1) & 0x3F;
            x4a = interpolateLinear(vertices[v[2]]->y << 6, vertices[v[3]]->y << 6, vertices[v[2]]->x << 1, x4 - 2, vertices[v[3]]->x << 1) & 0x3F;

            if (!negative) {
                x3a = 0x3F - x3a;
                x4a = 0x3F - x4a;
            }
        }

        x3 >>= 1;
        x4 >>= 1;
    }
    else { // Y-major
        // Interpolate with extra precision for edge alpha calculation
        // Note that negative Y-major edges seem to be interpolated in reverse from positive ones
        if (vertices[v[2]]->x > vertices[v[3]]->x)
            x3 = interpolateLinRev(vertices[v[2]]->x << 6, vertices[v[3]]->x << 6, vertices[v[2]]->y, line, vertices[v[3]]->y) - 1;
        else
            x3 = interpolateLinear(vertices[v[2]]->x << 6, vertices[v[3]]->x << 6, vertices[v[2]]->y, line, vertices[v[3]]->y);

        // Set the edge alpha values if anti-aliasing is enabled
        if (disp3DCnt & BIT(4)) {
            if (abs(vertices[v[3]]->x - vertices[v[2]]->x) == vertices[v[3]]->y - vertices[v[2]]->y)
                x4a = x3a = 0x20;
            else
                x4a = x3a = x3 & 0x3F;
        }

        x4 = x3 >>= 6;
    }

    // Reduce the bounds so they don't overlap
    if (x2 > x1) x2--;
    if (x4 > x3) x4--;

    // Handle crossed edges; these become "dotted" if their orientation is wrong
    if (x2 > x3) { x2 = x3; }
    if (x1 > x2) { x2 = x1; x1a = 0x3F - x1a; x2a = 0x3F - x2a; }
    if (x2 > x3) { x3 = x2; }
    if (x3 > x4) { x3 = x4; x3a = 0x3F - x3a; x4a = 0x3F - x4a; }

    // Swap the edges so the leftmost one comes first
    if (x1 > x4) {
        SWAP(v[0], v[2]);
        SWAP(v[1], v[3]);
        SWAP(x1, x3);
        SWAP(x2, x4);
        SWAP(x1a, x3a);
        SWAP(x2a, x4a);
    }

    uint32_t xe1[2], xe[2], xe2[2];
    bool hideLeft, hideRight;

    // Choose between X and Y coordinates for interpolation on the left (whichever is more precise)
    if (abs(vertices[v[1]]->x - vertices[v[0]]->x) > abs(vertices[v[1]]->y - vertices[v[0]]->y)) { // X-major
        xe1[0] = vertices[v[0]]->x;
        xe[0] = x1;
        xe2[0] = vertices[v[1]]->x;
        hideLeft = (vertices[v[0]]->y < vertices[v[1]]->y);
    }
    else { // Y-major
        xe1[0] = vertices[v[0]]->y;
        xe[0] = line;
        xe2[0] = vertices[v[1]]->y;
        hideLeft = false;
    }

    // Choose between X and Y coordinates for interpolation on the right (whichever is more precise)
    if (abs(vertices[v[3]]->x - vertices[v[2]]->x) > abs(vertices[v[3]]->y - vertices[v[2]]->y)) { // X-major
        xe1[1] = vertices[v[2]]->x;
        xe[1] = x4;
        xe2[1] = vertices[v[3]]->x;
        hideRight = (vertices[v[2]]->y > vertices[v[3]]->y);
    }
    else { // Y-major
        xe1[1] = vertices[v[2]]->y;
        xe[1] = line;
        xe2[1] = vertices[v[3]]->y;
        hideRight = (vertices[v[2]]->x != vertices[v[3]]->x);
    }

    // Apply W-shift to reduce (or expand) W values to 16 bits
    uint32_t ws[4];
    if (polygon->wShift >= 0) {
        for (int i = 0; i < 4; i++)
            ws[i] = vertices[v[i]]->w >> polygon->wShift;
    }
    else {
        for (int i = 0; i < 4; i++)
            ws[i] = vertices[v[i]]->w << -polygon->wShift;
    }

    uint32_t ze[2], we[2];
    uint32_t re[2], ge[2], be[2];
    uint32_t se[2], te[2];

    // Interpolate values along the left and right polygon edges for the current line
    for (int i = 0; i < 2; i++) {
        int i2 = i * 2;

        // Linearly interpolate the Z value of a polygon edge
        ze[i] = interpolateLinear(vertices[v[i2]]->z, vertices[v[i2 + 1]]->z, xe1[i], xe[i], xe2[i]);
        
        // Fall back to linear interpolation if the W values are equal and their lower bits are clear
        if (ws[i2] == ws[i2 + 1] && !(ws[i2] & 0x00FE)) {
            // Linearly interpolate the W value of a polygon edge
            we[i] = interpolateLinear(ws[i2], ws[i2 + 1], xe1[i], xe[i], xe2[i]);

            // Linearly interpolate the vertex color of a polygon edge
            // The color values are expanded to 9 bits during interpolation for extra precision
            re[i] = interpolateLinear(((vertices[v[i2]]->color >> 0) & 0x3F) << 3, ((vertices[v[i2 + 1]]->color >> 0) & 0x3F) << 3, xe1[i], xe[i], xe2[i]);
            ge[i] = interpolateLinear(((vertices[v[i2]]->color >> 6) & 0x3F) << 3, ((vertices[v[i2 + 1]]->color >> 6) & 0x3F) << 3, xe1[i], xe[i], xe2[i]);
            be[i] = interpolateLinear(((vertices[v[i2]]->color >> 12) & 0x3F) << 3, ((vertices[v[i2 + 1]]->color >> 12) & 0x3F) << 3, xe1[i], xe[i], xe2[i]);

            // Linearly interpolate the texture coordinates of a polygon edge
            // Interpolation is unsigned, so temporarily convert the signed values to unsigned
            se[i] = interpolateLinear((int32_t)vertices[v[i2]]->s + 0xFFFF, (int32_t)vertices[v[i2 + 1]]->s + 0xFFFF, xe1[i], xe[i], xe2[i]) - 0xFFFF;
            te[i] = interpolateLinear((int32_t)vertices[v[i2]]->t + 0xFFFF, (int32_t)vertices[v[i2 + 1]]->t + 0xFFFF, xe1[i], xe[i], xe2[i]) - 0xFFFF;
        }
        else {
            // Calculate the interpolation factor with a precision of 9 bits for polygon edges
            uint32_t factor;
            if (xe[i] <= xe1[i]) {
                // Clamp to the minimum value
                factor = 0;
            }
            else if (xe[i] >= xe2[i]) {
                // Clamp to the maximum value
                factor = (1 << 9);
            }
            else {
                // Adjust W values to be 15-bit so that a 32-bit calculation wouldn't overflow
                // The calculation is actually 64-bit for upscaling, but it can still be accurate
                // Use a 32-bit division when the numerator fits, since 64-bit division is slow on 32-bit CPUs
                uint32_t wa = (ws[i2] >> 1) + ((ws[i2] & 1) && !(ws[i2 + 1] & 1));
                uint64_t num = uint64_t((ws[i2] >> 1) * (xe[i] - xe1[i])) << 9;
                uint32_t den = (ws[i2 + 1] >> 1) * (xe2[i] - xe[i]) + wa * (xe[i] - xe1[i]);
                factor = (num >> 32) ? (num / den) : (uint32_t(num) / den);
            }

            // Interpolate the W value of a polygon edge using a factor
            we[i] = interpolateFactor(factor, 9, ws[i2], ws[i2 + 1]);

            // Interpolate the vertex color of a polygon edge using a factor
            // The color values are expanded to 9 bits during interpolation for extra precision
            re[i] = interpolateFactor(factor, 9, ((vertices[v[i2]]->color >> 0) & 0x3F) << 3, ((vertices[v[i2 + 1]]->color >> 0) & 0x3F) << 3);
            ge[i] = interpolateFactor(factor, 9, ((vertices[v[i2]]->color >> 6) & 0x3F) << 3, ((vertices[v[i2 + 1]]->color >> 6) & 0x3F) << 3);
            be[i] = interpolateFactor(factor, 9, ((vertices[v[i2]]->color >> 12) & 0x3F) << 3, ((vertices[v[i2 + 1]]->color >> 12) & 0x3F) << 3);

            // Interpolate the texture coordinates of a polygon edge using a factor
            // Interpolation is unsigned, so temporarily convert the signed values to unsigned
            se[i] = interpolateFactor(factor, 9, (int32_t)vertices[v[i2]]->s + 0xFFFF, (int32_t)vertices[v[i2 + 1]]->s + 0xFFFF) - 0xFFFF;
            te[i] = interpolateFactor(factor, 9, (int32_t)vertices[v[i2]]->t + 0xFFFF, (int32_t)vertices[v[i2 + 1]]->t + 0xFFFF) - 0xFFFF;
        }
    }

    // Keep track of shadow mask polygons
    if (polygon->mode == 3 && polygon->id == 0) { // Shadow mask polygon
        // Clear the stencil buffer at the start of a shadow mask polygon group
        if (!stencilClear[line]) {
            memset(&stencilBuffer[line * 256 * 2], 0, 256 << resShift);
            stencilClear[line] = true;
        }
    }
    else {
        // End a shadow mask polygon group
        stencilClear[line] = false;
    }

    // Increment the right bound not only for drawing, but for interpolation across the scanline as well
    // This seems to give results accurate to hardware
    uint32_t x1e = x1, x4e = ++x4;

    // Set special bounds that hide some edges for opaque pixels with no edge effects
    if (polygon->alpha != 0 && !(disp3DCnt & (BIT(4) | BIT(5)))) {
        if (hideLeft) x1e = x2 + 1;
        if (hideRight) x4e = x3;
        if (!(hideLeft && hideRight) && x4e <= x1e)
            x4e = x1e + 1;
    }

    // Because edge traversal doesn't consider equal values to be intersecting, it skips horizontal edges
    // Instead, simply consider the entire span across the top and bottom of a polygon to be an edge
    bool horizontal = (line == polygonTop[polygonIndex] || line == polygonBot[polygonIndex] - 1);

    PX_ADD(PX_SPAN, spanStart);

    // Draw the span with a version of the loop specialized for the polygon's main settings
    Span span = { polygon, line, x1, x2, x3, x4, x1e, x4e, x1a, x2a, x3a, x4a, horizontal,
        { ze[0], ze[1] }, { we[0], we[1] }, { re[0], re[1] }, { ge[0], ge[1] }, { be[0], be[1] }, { se[0], se[1] }, { te[0], te[1] } };
    switch ((polygon->wBuffer << 2) | (bool(disp3DCnt & BIT(4)) << 1) | (polygon->textureFmt != 0)) {
        case 0: drawSpan<false, false, false>(span); break;
        case 1: drawSpan<false, false, true>(span); break;
        case 2: drawSpan<false, true, false>(span); break;
        case 3: drawSpan<false, true, true>(span); break;
        case 4: drawSpan<true, false, false>(span); break;
        case 5: drawSpan<true, false, true>(span); break;
        case 6: drawSpan<true, true, false>(span); break;
        case 7: drawSpan<true, true, true>(span); break;
    }
}

template <bool wBuffer, bool antiAlias, bool textured> void Gpu3DRenderer::drawSpan(const Span &span) {
    // Unpack the span, so the loop reads like it did inside the polygon setup
    PX_SLOTS;
    _Polygon *polygon = span.polygon;
    int line = span.line;
    uint32_t x1 = span.x1, x2 = span.x2, x3 = span.x3, x4 = span.x4, x1e = span.x1e, x4e = span.x4e;
    uint32_t x1a = span.x1a, x2a = span.x2a, x3a = span.x3a, x4a = span.x4a;
    bool horizontal = span.horizontal;
    const uint32_t *ze = span.ze, *we = span.we, *re = span.re, *ge = span.ge, *be = span.be, *se = span.se, *te = span.te;

    int lastS = 0xFFFF, lastT = 0xFFFF;
    uint32_t texel;
    LinearStepper zStep(ze[0], ze[1], x1, x4);

    // When W is the same at both ends, attributes are interpolated linearly; step them instead of dividing per pixel
    // Steppers give the same results as interpolateLinear for any X; unused ones get an empty range, which skips
    // the divisions of setting them up
    const bool linear = (we[0] == we[1] && !(we[0] & 0x7F));
    const uint32_t xc = linear ? x4 : x1, xt = (linear && textured) ? x4 : x1;
    LinearStepper rStep(re[0], re[1], x1, xc), gStep(ge[0], ge[1], x1, xc), bStep(be[0], be[1], x1, xc);
    LinearStepper sStep(se[0] + 0xFFFF, se[1] + 0xFFFF, x1, xt), tStep(te[0] + 0xFFFF, te[1] + 0xFFFF, x1, xt);

    // Keep polygon settings in locals, since buffer stores would otherwise make them be reloaded for every pixel
    const uint8_t alpha = polygon->alpha, mode = polygon->mode, id = polygon->id, textureFmt = polygon->textureFmt;
    const bool depthTestEqual = polygon->depthTestEqual;
    const bool transNewDepth = polygon->transNewDepth, fog = polygon->fog;
    const int8_t wShift = polygon->wShift;

    // With the alpha test, pixels are only drawn if their 5-bit alpha is greater than the reference value
    const int alphaRef = (disp3DCnt & BIT(2)) ? alphaTestRef : -1;

    // Calculate the interpolation factor with a precision of 8 bits for polygon fills
    auto getFactor = [&](uint32_t x) -> uint32_t {
        if (linear) // Linear fallback
            return -1;
        else if (x <= x1) // Clamp min
            return 0;
        else if (x >= x4) // Clamp max
            return (1 << 8);
        else if (resShift && ((x - x1) >> 8)) // 64-bit for upscaling
            return (uint64_t(we[0] * (x - x1)) << 8) / (we[1] * (x4 - x) + we[0] * (x - x1));
        else // 32-bit
            return ((we[0] * (x - x1)) << 8) / (we[1] * (x4 - x) + we[0] * (x - x1));
    };

    // Draw a line segment
    for (uint32_t x = x1; x < x4; x++) {
        PX_TIMER(PX_DEPTH);
        // Skip the polygon interior for wireframe polygons
        if (!horizontal && alpha == 0 && x == x2 + 1 && x3 > x2)
            x = x3;

        // Invalid viewports can cause out-of-bounds vertices, so only draw within bounds
        if (x >= (256 << resShift))
            break;

        bool layer = 0;
        int i = line * 256 * 2 + x;

        // Calculate the depth value of the current pixel
        // With W-buffering it comes from the interpolation factor, which is otherwise only worked out for pixels that
        // pass the depth test, since it needs a division
        uint32_t factor;
        int32_t depth;
        if (wBuffer) {
            factor = getFactor(x);
            depth = (factor == -1) ? interpolateLinear(we[0], we[1], x1, x, x4) :
                interpolateFactor(factor, 8, we[0], we[1]);
            if (wShift > 0)
                depth <<= wShift;
            else if (wShift < 0)
                depth >>= -wShift;
        }
        else {
            depth = zStep.get(x);
        }

        // Depth test the pixel on the front layer, and on the back layer if under an anti-aliased edge
        bool depthPass[2];
        if (depthTestEqual) {
            uint32_t margin = (wBuffer ? 0xFF : 0x200);
            depthPass[0] = (depthBuffer[0][i] >= depth - margin && depthBuffer[0][i] <= depth + margin);
            depthPass[1] = antiAlias && (attribBuffer[0][i] & BIT(14)) &&
                (depthBuffer[1][i] >= depth - margin && depthBuffer[1][i] <= depth + margin);
        }
        else {
            depthPass[0] = (depthBuffer[0][i] > depth);
            depthPass[1] = antiAlias && (attribBuffer[0][i] & BIT(14)) && (depthBuffer[1][i] > depth);
        }

        // Check if the pixel should be drawn
        if (mode == 3) { // Shadow
            if (id == 0) { // Mask
                // Don't render shadow mask pixels, but set a stencil buffer bit for ones that fail the depth test
                if (!depthPass[0]) stencilBuffer[i] |= BIT(0);
                if (!depthPass[1]) stencilBuffer[i] |= BIT(1);
                continue;
            }
            else if (!depthPass[0] || !(stencilBuffer[i] & BIT(0)) || (attribBuffer[0][i] & 0x3F) == id) {
                // Only render non-mask shadow pixels if the stencil bit is set and the old pixel's polygon ID differs
                if (!depthPass[1] || !(stencilBuffer[i] & BIT(1)) || (attribBuffer[1][i] & 0x3F) == id)
                    continue;

                // Draw the pixel on the back layer
                layer = 1;
            }
        }
        else if (!depthPass[0]) {
            // Only render normal pixels if the depth test passes
            if (!depthPass[1])
                continue;

            // Draw the pixel on the back layer
            layer = 1;
        }

        // Interpolate the vertex color at the current pixel
        PX_NEXT();
        if (!wBuffer) factor = getFactor(x);
        uint32_t rv, gv, bv;
        if (factor == -1) {
            rv = rStep.get(x) >> 3;
            gv = gStep.get(x) >> 3;
            bv = bStep.get(x) >> 3;
        }
        else {
            rv = interpolateFactor(factor, 8, re[0], re[1]) >> 3;
            gv = interpolateFactor(factor, 8, ge[0], ge[1]) >> 3;
            bv = interpolateFactor(factor, 8, be[0], be[1]) >> 3;
        }
        uint32_t color = ((alpha ? alpha : 0x3F) << 18) | (bv << 12) | (gv << 6) | rv;

        // Blend the texture with the vertex color
        if (textured) {
            // Interpolate the texture coordinates at the current pixel
            int s, t;
            if (factor == -1) {
                s = (int)(sStep.get(x) - 0xFFFF) >> 4;
                t = (int)(tStep.get(x) - 0xFFFF) >> 4;
            }
            else {
                s = (int)(interpolateFactor(factor, 8, se[0] + 0xFFFF, se[1] + 0xFFFF) - 0xFFFF) >> 4;
                t = (int)(interpolateFactor(factor, 8, te[0] + 0xFFFF, te[1] + 0xFFFF) - 0xFFFF) >> 4;
            }

            // Read a new texel from the texture if the coordinates changed
            PX_NEXT();
            if (s != lastS || t != lastT) {
                lastS = s; lastT = t;
                texel = readTexture(polygon, s, t);
            }
            PX_NEXT();

            // Apply texture blending
            // These formulas are a translation of the pseudocode from GBATEK to C++
            switch (mode) {
            case 0: { // Modulation
                uint8_t r = ((((texel >> 0) & 0x3F) + 1) * (((color >> 0) & 0x3F) + 1) - 1) / 64;
                uint8_t g = ((((texel >> 6) & 0x3F) + 1) * (((color >> 6) & 0x3F) + 1) - 1) / 64;
                uint8_t b = ((((texel >> 12) & 0x3F) + 1) * (((color >> 12) & 0x3F) + 1) - 1) / 64;
                uint8_t a = ((((texel >> 18) & 0x3F) + 1) * (((color >> 18) & 0x3F) + 1) - 1) / 64;
                color = (a << 18) | (b << 12) | (g << 6) | r;
                break;
            }

            case 1: // Decal
            case 3: { // Shadow
                uint8_t at = ((texel >> 18) & 0x3F);
                uint8_t r = (((texel >> 0) & 0x3F) * at + ((color >> 0) & 0x3F) * (63 - at)) / 64;
                uint8_t g = (((texel >> 6) & 0x3F) * at + ((color >> 6) & 0x3F) * (63 - at)) / 64;
                uint8_t b = (((texel >> 12) & 0x3F) * at + ((color >> 12) & 0x3F) * (63 - at)) / 64;
                uint8_t a = ((color >> 18) & 0x3F);
                color = (a << 18) | (b << 12) | (g << 6) | r;
                break;
            }

            case 2: { // Toon/Highlight
                uint32_t toon = rgba5ToRgba6(toonTable[(color & 0x3F) / 2]);
                uint8_t r, g, b;

                if (disp3DCnt & BIT(1)) { // Highlight
                    r = ((((texel >> 0) & 0x3F) + 1) * (((color >> 0) & 0x3F) + 1) - 1) / 64;
                    g = ((((texel >> 6) & 0x3F) + 1) * (((color >> 6) & 0x3F) + 1) - 1) / 64;
                    b = ((((texel >> 12) & 0x3F) + 1) * (((color >> 12) & 0x3F) + 1) - 1) / 64;
                    r += ((toon >> 0) & 0x3F); if (r > 63) r = 63;
                    g += ((toon >> 6) & 0x3F); if (g > 63) g = 63;
                    b += ((toon >> 12) & 0x3F); if (b > 63) b = 63;
                }
                else { // Toon
                    r = ((((texel >> 0) & 0x3F) + 1) * (((toon >> 0) & 0x3F) + 1) - 1) / 64;
                    g = ((((texel >> 6) & 0x3F) + 1) * (((toon >> 6) & 0x3F) + 1) - 1) / 64;
                    b = ((((texel >> 12) & 0x3F) + 1) * (((toon >> 12) & 0x3F) + 1) - 1) / 64;
                }

                uint8_t a = ((((texel >> 18) & 0x3F) + 1) * (((color >> 18) & 0x3F) + 1) - 1) / 64;
                color = (a << 18) | (b << 12) | (g << 6) | r;
                break;
            }}
        }
        else if (mode == 2) { // Toon/Highlight (no texture)
            uint32_t toon = rgba5ToRgba6(toonTable[(color & 0x3F) / 2]);
            uint8_t r, g, b;

            if (disp3DCnt & BIT(1)) { // Highlight
                r = ((color >> 0) & 0x3F) + ((toon >> 0) & 0x3F); if (r > 63) r = 63;
                g = ((color >> 6) & 0x3F) + ((toon >> 6) & 0x3F); if (g > 63) g = 63;
                b = ((color >> 12) & 0x3F) + ((toon >> 12) & 0x3F); if (b > 63) b = 63;
            }
            else { // Toon
                r = ((toon >> 0) & 0x3F);
                g = ((toon >> 6) & 0x3F);
                b = ((toon >> 12) & 0x3F);
            }

            color = (color & 0xFC0000) | (b << 12) | (g << 6) | r;
        }

        // Skip fully transparent pixels, ones that fail the alpha test, and hidden edge pixels if the pixel is opaque
        // or blending is disabled
        if (!(color & 0xFC0000) || int((color >> 19) & 0x1F) <= alphaRef ||
                ((x < x1e || x >= x4e) && ((color >> 18) == 0x3F || !(disp3DCnt & BIT(3)))))
            continue;

        // Draw a pixel, marked with an extra bit as an indicator for 2D blending
        if ((color >> 18) == 0x3F) { // Opaque
            uint8_t edgeAlpha = 0x3F;
            bool edge = (x <= x2 || x >= x3 || horizontal);

            // Push the previous pixel to the back layer if drawing a front anti-aliased edge pixel
            if (antiAlias && layer == 0 && edge) {
                framebuffer[1][i] = framebuffer[0][i];
                depthBuffer[1][i] = depthBuffer[0][i];
                attribBuffer[1][i] = attribBuffer[0][i];

                // Set the pixel transparency for anti-aliasing
                if (x <= x2)
                    edgeAlpha = interpolateLinear(x1a, x2a, x1, x, x2);
                else if (x >= x3)
                    edgeAlpha = interpolateLinear(x3a, x4a, x3, x, x4);
            }

            framebuffer[layer][i] = BIT(26) | color;
            depthBuffer[layer][i] = depth;
            attribBuffer[layer][i] = (attribBuffer[layer][i] & 0x0FC0) | (edgeAlpha << 15) |
                (edge << 14) | (fog << 13) | id;
        }
        else if (!(attribBuffer[layer][i] & BIT(12)) || ((attribBuffer[layer][i] >> 6) & 0x3F) != id) { // Transparent
            // Transparent pixels are only drawn if the old pixel isn't transparent or the polygon ID differs
            framebuffer[layer][i] = BIT(26) | (((disp3DCnt & BIT(3)) && (framebuffer[layer][i] & 0xFC0000)) ?
                blendColor(framebuffer[layer][i], color, color >> 18) : color);
            if (transNewDepth) depthBuffer[layer][i] = depth;
            attribBuffer[layer][i] = (attribBuffer[layer][i] & (0x1FC03F | (fog << 13))) | BIT(12) | (id << 6);

            // Blend with the back layer as well if drawing over a front anti-aliased edge pixel
            if (antiAlias && layer == 0 && (attribBuffer[0][i] & BIT(14))) {
                framebuffer[1][i] = BIT(26) | (((disp3DCnt & BIT(3)) && (framebuffer[1][i] & 0xFC0000)) ?
                    blendColor(framebuffer[1][i], color, color >> 18) : color);
                if (transNewDepth) depthBuffer[1][i] = depth;
                attribBuffer[1][i] = (attribBuffer[1][i] & (0x1FC03F | (fog << 13))) | BIT(12) | (id << 6);
            }
        }
    }
}

void Gpu3DRenderer::writeDisp3DCnt(uint16_t mask, uint16_t value) {
    // If any of the error bits are set, acknowledge the errors by clearing them
    if (value & BIT(12)) disp3DCnt &= ~BIT(12);
    if (value & BIT(13)) disp3DCnt &= ~BIT(13);

    // Write to the DISP3DCNT register and invalidate the 3D if a parameter changed
    mask &= 0x4FFF;
    if ((value & mask) == (disp3DCnt & mask)) return;
    disp3DCnt = (disp3DCnt & ~mask) | (value & mask);
    core->gpu.invalidate3D();
}

void Gpu3DRenderer::writeEdgeColor(int index, uint16_t mask, uint16_t value) {
    // Write to one of the EDGE_COLOR registers and invalidate the 3D if a parameter changed
    mask &= 0x7FFF;
    if ((value & mask) == (edgeColor[index] & mask)) return;
    edgeColor[index] = (edgeColor[index] & ~mask) | (value & mask);
    core->gpu.invalidate3D();
}

void Gpu3DRenderer::writeClearColor(uint32_t mask, uint32_t value) {
    // Write to the CLEAR_COLOR register and invalidate the 3D if a parameter changed
    mask &= 0x3F1FFFFF;
    if ((value & mask) == (clearColor & mask)) return;
    clearColor = (clearColor & ~mask) | (value & mask);
    core->gpu.invalidate3D();
}

void Gpu3DRenderer::writeClearDepth(uint16_t mask, uint16_t value) {
    // Write to the CLEAR_DEPTH register and invalidate the 3D if a parameter changed
    mask &= 0x7FFF;
    if ((value & mask) == (clearDepth & mask)) return;
    clearDepth = (clearDepth & ~mask) | (value & mask);
    core->gpu.invalidate3D();
}

void Gpu3DRenderer::writeToonTable(int index, uint16_t mask, uint16_t value) {
    // Write to one of the TOON_TABLE registers and invalidate the 3D if a parameter changed
    mask &= 0x7FFF;
    if ((value & mask) == (toonTable[index] & mask)) return;
    toonTable[index] = (toonTable[index] & ~mask) | (value & mask);
    core->gpu.invalidate3D();
}

void Gpu3DRenderer::writeFogColor(uint32_t mask, uint32_t value) {
    // Write to the FOG_COLOR register and invalidate the 3D if a parameter changed
    mask &= 0x001F7FFF;
    if ((value & mask) == (fogColor & mask)) return;
    fogColor = (fogColor & ~mask) | (value & mask);
    core->gpu.invalidate3D();
}

void Gpu3DRenderer::writeFogOffset(uint16_t mask, uint16_t value) {
    // Write to the FOG_OFFSET register and invalidate the 3D if a parameter changed
    mask &= 0x7FFF;
    if ((value & mask) == (fogOffset & mask)) return;
    fogOffset = (fogOffset & ~mask) | (value & mask);
    core->gpu.invalidate3D();
}

void Gpu3DRenderer::writeAlphaTestRef(uint8_t value) {
    // Write to the ALPHA_TEST_REF register and invalidate the 3D if a parameter changed
    if ((value & 0x1F) == alphaTestRef) return;
    alphaTestRef = value & 0x1F;
    core->gpu.invalidate3D();
}

void Gpu3DRenderer::writeFogTable(int index, uint8_t value) {
    // Write to one of the FOG_TABLE registers and invalidate the 3D if a parameter changed
    if ((value & 0x7F) == (fogTable[index] & 0x7F)) return;
    fogTable[index] = value & 0x7F;
    core->gpu.invalidate3D();
}
