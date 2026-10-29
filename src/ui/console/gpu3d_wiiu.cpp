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

#ifdef __WIIU__

#include <coreinit/cache.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/thread.h>
#include <gx2/clear.h>
#include <gx2/context.h>
#include <gx2/draw.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/sampler.h>
#include <gx2/shaders.h>
#include <gx2/state.h>
#include <gx2/surface.h>
#include <gx2/texture.h>
#include <whb/gfx.h>
#ifdef GPU_DUMP
#include <cstdio>
#include "../../core/settings.h"
#endif
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <thread>
#include <unordered_map>
#include <vector>

#include "gpu3d_wiiu.h"
#include "../../core/core.h"
#include "../../core/heartbeat.h"
#include "../../core/worker_thread.h"

extern const uint8_t gpu3d_color_gsh[], gpu3d_modulate_gsh[], gpu3d_decal_gsh[], gpu3d_modtrans_gsh[], gpu3d_edge_gsh[];

std::mutex gx2Mutex;

namespace {

// The vertex layout the shaders read: clip-space position, texture coordinates and color
const GX2ChannelMask NO_CHANNELS = (GX2ChannelMask)0;

struct GpuVertex {
    float pos[4];
    float tex[2];
    float color[4];
    float attrib[4]; // Polygon ID times 4 plus flags (1 = solid pixel, 2 = fog), unused, and two values for the depth
};

// A run of triangles drawn with the same state
struct GpuDraw {
    int shader; // 0 = vertex color, 1 = modulate, 2 = decal
    GX2Texture *texture; // Null for untextured polygons
    int sampler;
    bool translucent;
    bool depthEqual;
    bool depthWrite;
    uint8_t stencil; // 0 for solid polygons, or the translucent flag with the polygon ID
    bool split; // Whether pixels can be opaque or translucent, so they're drawn separately to follow different rules
    uint32_t first, count;
};

class Gpu3DWiiU: public Gpu3DHardware {
public:
    bool init();
    void render(Gpu3DRenderer *renderer) override;

private:
    static const int MAX_VERTICES = 2048 * 8 * 3;
    static constexpr float EDGE_BIAS = 1.0f / 64; // How far short of half a pixel vertices are moved

    WHBGfxShaderGroup groups[5];
    GX2Sampler samplers[9];
    GX2ContextState *state = nullptr;
    GX2ColorBuffer colorBuffer;
    GX2ColorBuffer attribBuffer; // Per-pixel attributes of solid polygons, for edge marking and fog
    GX2Texture attribTexture; // The attributes again, for the edge marking pass to read
    GX2Texture edgeTexture; // The 8 edge colors, with how much they're blended in as alpha
    GX2DepthBuffer depthBuffer;
    GpuVertex *vertices = nullptr;

    // Decoded textures are kept between frames, and decoded again when the data they came from changes
    struct CachedTexture {
        GX2Texture *texture;
        uint64_t hash;
        uint32_t frame;
    };
    std::unordered_map<uint64_t, CachedTexture> textures;
    std::vector<uint32_t> decoded;
    std::vector<GX2Texture*> updated;
    std::vector<GpuDraw> draws;
    uint32_t frameCount = 0;

    std::mutex mutex;
    std::condition_variable cond;
    Gpu3DRenderer *job = nullptr;
    std::atomic<bool> busy;

    static int threadEntry(int argc, const char **argv);
    void run();
    void renderFrame(Gpu3DRenderer *renderer);
    GX2Texture *getTexture(Gpu3DRenderer *renderer, _Polygon *polygon);
    void freeTextures();
};

bool Gpu3DWiiU::init() {
    // Load the shaders, which share one vertex shader and its layout
    const uint8_t *shaders[] = { gpu3d_color_gsh, gpu3d_modulate_gsh, gpu3d_decal_gsh, gpu3d_modtrans_gsh, gpu3d_edge_gsh };
    for (int i = 0; i < 5; i++) {
        if (!WHBGfxLoadGFDShaderGroup(&groups[i], 0, shaders[i])) return false;
        WHBGfxInitShaderAttribute(&groups[i], "position", 0, offsetof(GpuVertex, pos), GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32);
        WHBGfxInitShaderAttribute(&groups[i], "tex_coords", 0, offsetof(GpuVertex, tex), GX2_ATTRIB_FORMAT_FLOAT_32_32);
        WHBGfxInitShaderAttribute(&groups[i], "vtx_color", 0, offsetof(GpuVertex, color), GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32);
        WHBGfxInitShaderAttribute(&groups[i], "vtx_attr", 0, offsetof(GpuVertex, attrib), GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32);
        if (!WHBGfxInitFetchShader(&groups[i])) return false;

        // The assembler can only enable the red channel of a second output, so enable all of them here
        if (i < 4) groups[i].pixelShader->regs.cb_shader_mask = 0xFF;
    }

    // Set up nearest-neighbor samplers for each combination of clamping, repeating and flipping
    static const GX2TexClampMode modes[] = { GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_CLAMP_MODE_WRAP, GX2_TEX_CLAMP_MODE_MIRROR };
    for (int i = 0; i < 9; i++) {
        GX2InitSampler(&samplers[i], GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_POINT);
        GX2InitSamplerClamping(&samplers[i], modes[i % 3], modes[i / 3], GX2_TEX_CLAMP_MODE_CLAMP);
    }

    // Create a linear color buffer the CPU can read, and a float depth buffer for precision with W-buffering
    memset(&colorBuffer, 0, sizeof(colorBuffer));
    colorBuffer.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    colorBuffer.surface.width = 256;
    colorBuffer.surface.height = 192;
    colorBuffer.surface.depth = 1;
    colorBuffer.surface.mipLevels = 1;
    colorBuffer.surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    colorBuffer.surface.aa = GX2_AA_MODE1X;
    colorBuffer.surface.use = GX2_SURFACE_USE_COLOR_BUFFER;
    colorBuffer.surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    colorBuffer.viewNumSlices = 1;
    GX2CalcSurfaceSizeAndAlignment(&colorBuffer.surface);
    GX2InitColorBufferRegs(&colorBuffer);
    colorBuffer.surface.image = MEMAllocFromDefaultHeapEx(colorBuffer.surface.imageSize, colorBuffer.surface.alignment);

    // Create a linear float buffer for the attributes, written as a second render target
    // Each pixel has the ID and flags, and the depth the DS would compare, which the shaders get by dividing the last 2
    // attributes: W interpolates to itself across the screen, and Z times W over W interpolates to Z
    memset(&attribBuffer, 0, sizeof(attribBuffer));
    attribBuffer.surface = colorBuffer.surface;
    attribBuffer.surface.format = GX2_SURFACE_FORMAT_FLOAT_R32_G32;
    attribBuffer.surface.use = (GX2SurfaceUse)(GX2_SURFACE_USE_COLOR_BUFFER | GX2_SURFACE_USE_TEXTURE);
    attribBuffer.surface.image = nullptr;
    attribBuffer.viewNumSlices = 1;
    GX2CalcSurfaceSizeAndAlignment(&attribBuffer.surface);
    GX2InitColorBufferRegs(&attribBuffer);
    attribBuffer.surface.image = MEMAllocFromDefaultHeapEx(attribBuffer.surface.imageSize, attribBuffer.surface.alignment);
    memset(&attribTexture, 0, sizeof(attribTexture));
    attribTexture.surface = attribBuffer.surface;
    attribTexture.surface.use = GX2_SURFACE_USE_TEXTURE;
    attribTexture.viewNumSlices = 1;
    attribTexture.compMap = 0x00010405;
    GX2InitTextureRegs(&attribTexture);

    // Create the edge color texture
    memset(&edgeTexture, 0, sizeof(edgeTexture));
    edgeTexture.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    edgeTexture.surface.width = 8;
    edgeTexture.surface.height = 1;
    edgeTexture.surface.depth = 1;
    edgeTexture.surface.mipLevels = 1;
    edgeTexture.surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    edgeTexture.surface.aa = GX2_AA_MODE1X;
    edgeTexture.surface.use = GX2_SURFACE_USE_TEXTURE;
    edgeTexture.surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    edgeTexture.viewNumSlices = 1;
    edgeTexture.compMap = 0x00010203;
    GX2CalcSurfaceSizeAndAlignment(&edgeTexture.surface);
    GX2InitTextureRegs(&edgeTexture);
    edgeTexture.surface.image = MEMAllocFromDefaultHeapEx(edgeTexture.surface.imageSize, edgeTexture.surface.alignment);

    memset(&depthBuffer, 0, sizeof(depthBuffer));
    depthBuffer.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    depthBuffer.surface.width = 256;
    depthBuffer.surface.height = 192;
    depthBuffer.surface.depth = 1;
    depthBuffer.surface.mipLevels = 1;
    // The depth is 24-bit fixed point with 8-bit stencil, which matches the DS's Z-buffer precision
    // Stencil doesn't seem to work with the 32-bit float format, and the 24-bit float one loses too much precision
    // with W-buffering, which is handled by scaling depths into a good range each frame
    depthBuffer.surface.format = GX2_SURFACE_FORMAT_UNORM_R24_X8;
    depthBuffer.surface.aa = GX2_AA_MODE1X;
    depthBuffer.surface.use = GX2_SURFACE_USE_DEPTH_BUFFER;
    depthBuffer.surface.tileMode = GX2_TILE_MODE_DEFAULT;
    depthBuffer.viewNumSlices = 1;
    depthBuffer.depthClear = 1.0f;
    GX2CalcSurfaceSizeAndAlignment(&depthBuffer.surface);
    GX2InitDepthBufferRegs(&depthBuffer);
    depthBuffer.surface.image = MEMAllocFromDefaultHeapEx(depthBuffer.surface.imageSize, depthBuffer.surface.alignment);

    // Allocate the vertex buffer and a context state of its own, so the UI's state isn't disturbed
    // There's room for a screen-sized quad at the end, for edge marking
    vertices = (GpuVertex*)MEMAllocFromDefaultHeapEx((MAX_VERTICES + 6) * sizeof(GpuVertex), GX2_VERTEX_BUFFER_ALIGNMENT);
    state = (GX2ContextState*)MEMAllocFromDefaultHeapEx(sizeof(GX2ContextState), GX2_CONTEXT_STATE_ALIGNMENT);
    if (!colorBuffer.surface.image || !attribBuffer.surface.image || !edgeTexture.surface.image ||
            !depthBuffer.surface.image || !vertices || !state)
        return false;
    GX2SetupContextStateEx(state, TRUE);

    // Render on a thread on the UI's core, which has to be the one that uses GX2, with priority over the UI
    busy.store(false);
    return WorkerThread::createThread(threadEntry, (void*)this, 0x10000, OSGetThreadPriority(OSGetCurrentThread()) - 1,
        OS_THREAD_ATTRIB_AFFINITY_CPU2) != nullptr;
}

int Gpu3DWiiU::threadEntry(int argc, const char **argv) {
    ((Gpu3DWiiU*)argv)->run();
    return 0;
}

void Gpu3DWiiU::render(Gpu3DRenderer *renderer) {
    // Wait for the last frame to finish in case it was skipped, and then hand over the next one
    HB_TIMER(hbWait);
    while (busy.load()) std::this_thread::yield();
    HB_ADD_US(HB_EMU_3D_WAIT_US, hbWait);
    std::lock_guard<std::mutex> guard(mutex);
    busy.store(true);
    job = renderer;
    cond.notify_one();
}

void Gpu3DWiiU::run() {
    // Render frames as they're handed over
    while (true) {
        HB_SET(HB_GPU_STAGE, 1);
        std::unique_lock<std::mutex> lock(mutex);
        cond.wait(lock, [&] { return job != nullptr; });
        Gpu3DRenderer *renderer = job;
        job = nullptr;
        lock.unlock();
        renderFrame(renderer);
        HB_INC(HB_GPU_FRAMES);
        busy.store(false);
    }
}

void Gpu3DWiiU::freeTextures() {
    // Free textures that haven't been used for a couple of seconds, which the GPU has finished with
    for (auto it = textures.begin(); it != textures.end();) {
        if (frameCount - it->second.frame > 120) {
            MEMFreeToDefaultHeap(it->second.texture->surface.image);
            delete it->second.texture;
            it = textures.erase(it);
        }
        else {
            it++;
        }
    }
}

GX2Texture *Gpu3DWiiU::getTexture(Gpu3DRenderer *renderer, _Polygon *polygon) {
    // Look up the texture by everything that affects how it decodes; sizes are powers of 2 from 8 to 1024
    uint64_t key = polygon->textureAddr | (uint64_t(polygon->paletteAddr) << 19) |
        (uint64_t(__builtin_ctz(polygon->sizeS) - 3) << 36) | (uint64_t(__builtin_ctz(polygon->sizeT) - 3) << 39) |
        (uint64_t(polygon->textureFmt) << 42) | (uint64_t(polygon->transparent0) << 45);
    CachedTexture &cached = textures[key];
    if (cached.texture && cached.frame == frameCount) return cached.texture;

    // Check the texture once per frame, keeping the decoded copy if its data hasn't changed
    uint64_t hash = renderer->hashTexture(polygon);
    cached.frame = frameCount;
    if (cached.texture && cached.hash == hash) return cached.texture;
    cached.hash = hash;

    // Create the texture the first time
    GX2Texture *texture = cached.texture;
    if (!texture) {
        texture = new GX2Texture;
        memset(texture, 0, sizeof(GX2Texture));
        texture->surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
        texture->surface.width = polygon->sizeS;
        texture->surface.height = polygon->sizeT;
        texture->surface.depth = 1;
        texture->surface.mipLevels = 1;
        texture->surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
        texture->surface.aa = GX2_AA_MODE1X;
        texture->surface.use = GX2_SURFACE_USE_TEXTURE;
        texture->surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
        texture->viewNumSlices = 1;
        texture->compMap = 0x00010203;
        GX2CalcSurfaceSizeAndAlignment(&texture->surface);
        GX2InitTextureRegs(texture);
        texture->surface.image = MEMAllocFromDefaultHeapEx(texture->surface.imageSize, texture->surface.alignment);
        if (!texture->surface.image) {
            delete texture;
            textures.erase(key);
            return nullptr;
        }
        cached.texture = texture;
    }

    // Decode the texture into it, with rows at its pitch
    decoded.resize(polygon->sizeS * polygon->sizeT);
    renderer->decodeTexture(polygon, decoded.data());
    for (int y = 0; y < polygon->sizeT; y++)
        memcpy((uint32_t*)texture->surface.image + y * texture->surface.pitch, &decoded[y * polygon->sizeS],
            polygon->sizeS * sizeof(uint32_t));

    // Invalidating it adds a GPU command, so that waits until the frame is drawn while holding GX2
    updated.push_back(texture);
    return texture;
}

void Gpu3DWiiU::renderFrame(Gpu3DRenderer *renderer) {
    Gpu3D &gpu3D = renderer->getCore()->gpu3D;
    _Polygon *polygons = gpu3D.polygonsOut;
    Vertex *verts = gpu3D.verticesOut;
    int polygonCount = gpu3D.polygonCountOut;
    uint16_t disp3DCnt = renderer->readDisp3DCnt();
    bool texturing = disp3DCnt & BIT(0), blending = disp3DCnt & BIT(3);
    bool wBuffer = polygonCount && polygons[0].wBuffer;
    HB_SET(HB_GPU_STAGE, 2);
    HB_SET(HB_GPU_DISP3DCNT, disp3DCnt);
    HB_TIMER(hbBuild);
    frameCount++;
    freeTextures();
    updated.clear();
    draws.clear();

    // Build the triangles, with solid polygons first and then translucent ones, in order like the software renderer
    // W-buffered depth is 1/W, which interpolates linearly across the screen like Z, so the GPU compares it exactly;
    // it's reversed so precision is best up close, and compared with greater instead of less
    // It's scaled by the smallest W so the closest polygon is at 1, using the whole range of the depth buffer
    float minW = 0x7FFFFFFF;
    if (wBuffer) {
        for (int i = 0; i < polygonCount; i++)
            for (int j = 0; j < polygons[i].size; j++)
                if (verts[polygons[i].vertices + j].w > 0)
                    minW = std::min(minW, float(verts[polygons[i].vertices + j].w));
    }

    uint32_t count = 0;
#ifdef GPU_DUMP
    std::vector<uint32_t> dumpMap; // Pairs of polygon index and first vertex
#endif
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < polygonCount; i++) {
            _Polygon *polygon = &polygons[i];
            bool translucent = (polygon->alpha < 0x3F || polygon->textureFmt == 1 || polygon->textureFmt == 6);
            if (translucent != (pass == 1)) continue;

            // Skip what isn't supported yet: shadows and wireframe polygons
            if (polygon->size < 3 || polygon->mode == 3 || polygon->alpha == 0) continue;
            if (count + (polygon->size - 2) * 3 > MAX_VERTICES) break;

            // Pick the shader and texture; toon and highlight polygons are drawn like modulated ones for now
            GpuDraw draw = {};
            bool textured = texturing && polygon->textureFmt != 0;
            HB_TIMER(hbTexture);
            draw.texture = textured ? getTexture(renderer, polygon) : nullptr;
            HB_ADD_US(HB_GPU_TEXTURE_US, hbTexture);
            draw.shader = !draw.texture ? 0 : (polygon->mode == 1) ? 2 : 1;
            draw.sampler = (polygon->repeatS ? (polygon->flipS ? 2 : 1) : 0) + (polygon->repeatT ? (polygon->flipT ? 2 : 1) : 0) * 3;
            draw.translucent = translucent && blending;
            draw.depthEqual = polygon->depthTestEqual;
            draw.depthWrite = !translucent || polygon->transNewDepth;
            // Pixels are translucent if their final alpha isn't full, which is all of them for translucent polygons,
            // while full-alpha polygons with translucent textures can also give opaque pixels, which act like solid ones
            draw.stencil = translucent ? (0x40 | polygon->id) : 0;
            draw.split = translucent && polygon->alpha == 0x3F;
            draw.first = count;
#ifdef GPU_DUMP
            dumpMap.push_back(i);
            dumpMap.push_back(count);
#endif

            // Get a polygon vertex, swapping back the last 2 of unclipped quad strip polygons, which are swapped
            auto vertex = [&](int k) -> Vertex& {
                if (polygon->crossed && k >= 2) k ^= 1;
                return verts[polygon->vertices + k];
            };

            // The DS samples pixels at their top-left corners while the GPU uses their centers, so vertices move half
            // a pixel to line them up, but just short of that, so pixels exactly on an edge are drawn or skipped like
            // on the DS; texture coordinates move back by the same amount, using how they change across the polygon
            float shiftS = 0, shiftT = 0;
            if (textured) {
                Vertex &a = vertex(0), &b = vertex(1), &c = vertex(2);
                float bx = b.x - a.x, by = b.y - a.y, cx = c.x - a.x, cy = c.y - a.y;
                float bs = (b.s - a.s) / 16.0f, bt = (b.t - a.t) / 16.0f, cs = (c.s - a.s) / 16.0f, ct = (c.t - a.t) / 16.0f;
                if (float det = bx * cy - cx * by) {
                    shiftS = EDGE_BIAS * ((bs * cy - cs * by) + (cs * bx - bs * cx)) / det;
                    shiftT = EDGE_BIAS * ((bt * cy - ct * by) + (ct * bx - bt * cx)) / det;
                }
            }

            // Convert a polygon vertex for the shaders, with texture coordinates in texels
            float alpha = polygon->alpha / 63.0f;
            bool toon = (polygon->mode == 2 && !(disp3DCnt & BIT(1)));
            auto convert = [&](GpuVertex &out, int k) {
                Vertex &v = vertex(k);
                float w = (v.w > 0) ? float(v.w) : 1.0f;
                float depth = wBuffer ? (minW / w) : (float(v.z) / 0x1000000);
                out.pos[0] = ((float(v.x) + 0.5f - EDGE_BIAS) / 128 - 1) * w;
                out.pos[1] = (1 - (float(v.y) + 0.5f - EDGE_BIAS) / 96) * w;
                out.pos[2] = depth * w;
                out.pos[3] = w;
                out.tex[0] = v.s / 16.0f - shiftS;
                out.tex[1] = v.t / 16.0f - shiftT;
                if (toon) {
                    // Toon shading replaces the color with an entry from the toon table, picked by the red level
                    uint16_t entry = renderer->getToon(((v.color >> 0) & 0x3F) / 2);
                    out.color[0] = ((entry >> 0) & 0x1F) / 31.0f;
                    out.color[1] = ((entry >> 5) & 0x1F) / 31.0f;
                    out.color[2] = ((entry >> 10) & 0x1F) / 31.0f;
                }
                else {
                    out.color[0] = ((v.color >> 0) & 0x3F) / 63.0f;
                    out.color[1] = ((v.color >> 6) & 0x3F) / 63.0f;
                    out.color[2] = ((v.color >> 12) & 0x3F) / 63.0f;
                }
                out.color[3] = alpha;
                out.attrib[0] = polygon->id * 4 + 1 + (polygon->fog ? 2 : 0);
                out.attrib[1] = 0.0f;
                out.attrib[2] = wBuffer ? 1.0f : w;
                out.attrib[3] = wBuffer ? w : float(v.z) * w;
            };

            // Add the polygon as a fan of triangles
            for (int j = 1; j + 1 < polygon->size; j++) {
                convert(vertices[count++], 0);
                convert(vertices[count++], j);
                convert(vertices[count++], j + 1);
            }

            // Merge the triangles into the last draw if the state is the same
            draw.count = count - draw.first;
            GpuDraw *last = draws.empty() ? nullptr : &draws.back();
            if (last && last->shader == draw.shader && last->texture == draw.texture && last->sampler == draw.sampler &&
                    last->translucent == draw.translucent && last->depthEqual == draw.depthEqual &&
                    last->depthWrite == draw.depthWrite && last->stencil == draw.stencil && last->split == draw.split &&
                    last->first + last->count == draw.first)
                last->count += draw.count;
            else
                draws.push_back(draw);
        }
    }

    // Edge marking is done on the GPU from the attributes of solid pixels, but with fog, the renderer applies it and
    // everything else from them on the CPU like it does for its own scanlines, which is slower
    bool finish = disp3DCnt & BIT(7);
    bool gpuEdges = (disp3DCnt & BIT(5)) && !finish;
    bool writeAttribs = finish || gpuEdges;

    // Get the clear values; the clear color is 5-bit with a 5-bit alpha
    uint32_t clear = renderer->getClearColor();
    float clearR = ((clear >> 0) & 0x1F) / 31.0f, clearG = ((clear >> 5) & 0x1F) / 31.0f;
    float clearB = ((clear >> 10) & 0x1F) / 31.0f, clearA = ((clear >> 16) & 0x1F) / 31.0f;
    uint16_t clearDepth = renderer->getClearDepth();
    float depthClear = wBuffer ? 0.0f : (clearDepth == 0x7FFF) ? 1.0f : float(clearDepth << 9) / 0x1000000;
    uint32_t clearAttrib = ((clear & BIT(15)) >> 2) | ((clear & 0x3F000000) >> 18) | ((clear & 0x3F000000) >> 24) | (0x3F << 15);
    int32_t clearDepthValue = (clearDepth == 0x7FFF) ? 0xFFFFFF : (clearDepth << 9);

    if (gpuEdges) {
        // Set the edge colors, blended in halfway with anti-aliasing, which softens marked edges
        uint32_t *texels = (uint32_t*)edgeTexture.surface.image;
        for (int i = 0; i < 8; i++) {
            uint16_t color = renderer->getEdgeColor(i);
            texels[i] = ((((color >> 0) & 0x1F) * 255 / 31) << 24) | ((((color >> 5) & 0x1F) * 255 / 31) << 16) |
                ((((color >> 10) & 0x1F) * 255 / 31) << 8) | ((disp3DCnt & BIT(4)) ? 0x80 : 0xFF);
        }

        // Add a quad covering the screen, passing the coordinates of each pixel and its left and right, then down and
        // up neighbors in the attributes texture
        static const float corners[6][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
        for (int i = 0; i < 6; i++) {
            float u = corners[i][0], v = corners[i][1];
            GpuVertex &out = vertices[count + i];
            out = { { u * 2 - 1, 1 - v * 2, 0, 1 }, { u, v }, { u + 1.0f / 256, v, u - 1.0f / 256, v },
                { u, v + 1.0f / 192, u, v - 1.0f / 192 } };
        }
    }

    {
        // Draw the frame while holding GX2, setting this context's state up again after clearing
        HB_ADD_US(HB_GPU_BUILD_US, hbBuild);
        HB_SET(HB_GPU_STAGE, 3);
        HB_TIMER(hbLock);
        std::unique_lock<std::mutex> guard(gx2Mutex);
        HB_ADD_US(HB_GPU_LOCK_US, hbLock);
        HB_SET(HB_GPU_STAGE, 4);
        HB_TIMER(hbDraw);

        // Make what the CPU wrote visible to the GPU; this adds GPU commands, so it has to be done with GX2 held
        for (GX2Texture *texture : updated)
            GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, texture->surface.image, texture->surface.imageSize);
        GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, vertices, (count + (gpuEdges ? 6 : 0)) * sizeof(GpuVertex));
        if (gpuEdges)
            GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, edgeTexture.surface.image, edgeTexture.surface.imageSize);
        GX2SetContextState(state);
        GX2ClearColor(&colorBuffer, clearR, clearG, clearB, clearA);
        GX2ClearColor(&attribBuffer, float(clearAttrib & 0x3F) * 4 + ((clearAttrib & BIT(13)) ? 2 : 0), clearDepthValue, 0, 0);
        GX2ClearDepthStencilEx(&depthBuffer, depthClear, 0, GX2_CLEAR_FLAGS_BOTH);
        GX2SetContextState(state);
        GX2SetColorBuffer(&colorBuffer, GX2_RENDER_TARGET_0);
        GX2SetColorBuffer(&attribBuffer, GX2_RENDER_TARGET_1);
        GX2SetDepthBuffer(&depthBuffer);
        GX2SetViewport(0, 0, 256, 192, 0, 1);
        GX2SetScissor(0, 0, 256, 192);
        GX2SetRasterizerClipControlHalfZ(TRUE, FALSE, TRUE);
        GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, FALSE);
        GX2SetAlphaTest(TRUE, GX2_COMPARE_FUNC_GREATER, 0.0f);
        GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_INV_SRC_ALPHA,
            GX2_BLEND_COMBINE_MODE_ADD, TRUE, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_ONE, GX2_BLEND_COMBINE_MODE_MAX);
        GX2SetAttribBuffer(0, (count + (gpuEdges ? 6 : 0)) * sizeof(GpuVertex), sizeof(GpuVertex), vertices);

        for (GpuDraw &draw : draws) {
            WHBGfxShaderGroup &group = groups[draw.shader];
            GX2SetFetchShader(&group.fetchShader);
            GX2SetVertexShader(group.vertexShader);
            GX2SetPixelShader(group.pixelShader);
            if (draw.texture) {
                // The shaders take texture coordinates in texels, and are given the size of a texel
                float texelSize[] = { 1.0f / draw.texture->surface.width, 1.0f / draw.texture->surface.height, 0, 0 };
                GX2SetPixelTexture(draw.texture, 0);
                GX2SetPixelSampler(&samplers[draw.sampler], 0);
                GX2SetPixelUniformReg(0, 4, texelSize);
            }
            GX2CompareFunction func = wBuffer ? (draw.depthEqual ? GX2_COMPARE_FUNC_GEQUAL : GX2_COMPARE_FUNC_GREATER) :
                (draw.depthEqual ? GX2_COMPARE_FUNC_LEQUAL : GX2_COMPARE_FUNC_LESS);
            GX2SetColorControl(GX2_LOGIC_OP_COPY, draw.translucent ? 0x01 : 0x00, FALSE, TRUE);

            // The stencil buffer tracks which polygon ID last drew a translucent pixel, since translucent pixels
            // aren't drawn over ones from the same ID; opaque pixels clear it, and always write depth
            if (draw.split) {
                // Draw the opaque pixels first, then the translucent ones with a shader that drops opaque pixels
                // Only modulated textures can give both, since other modes use the polygon's alpha
                GX2SetAlphaTest(TRUE, GX2_COMPARE_FUNC_GREATER, 0.99f);
                GX2SetDepthStencilControl(TRUE, TRUE, func, TRUE, FALSE,
                    GX2_COMPARE_FUNC_ALWAYS, GX2_STENCIL_FUNCTION_REPLACE, GX2_STENCIL_FUNCTION_KEEP,
                    GX2_STENCIL_FUNCTION_KEEP, GX2_COMPARE_FUNC_ALWAYS, GX2_STENCIL_FUNCTION_KEEP,
                    GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP);
                GX2SetStencilMask(0x7F, 0x7F, 0, 0x7F, 0x7F, 0);
                GX2SetTargetChannelMasks(GX2_CHANNEL_MASK_RGBA, writeAttribs ? GX2_CHANNEL_MASK_RGBA : NO_CHANNELS,
                    NO_CHANNELS, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS);
                GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, draw.count, draw.first, 1);
                GX2SetAlphaTest(TRUE, GX2_COMPARE_FUNC_GREATER, 0.0f);
                if (draw.shader != 1) continue;
                WHBGfxShaderGroup &trans = groups[3];
                GX2SetFetchShader(&trans.fetchShader);
                GX2SetVertexShader(trans.vertexShader);
                GX2SetPixelShader(trans.pixelShader);
            }
            GX2SetDepthStencilControl(TRUE, draw.depthWrite, func, TRUE, FALSE,
                draw.stencil ? GX2_COMPARE_FUNC_NOT_EQUAL : GX2_COMPARE_FUNC_ALWAYS, GX2_STENCIL_FUNCTION_REPLACE,
                GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP, GX2_COMPARE_FUNC_ALWAYS,
                GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP);
            GX2SetStencilMask(0x7F, 0x7F, draw.stencil, 0x7F, 0x7F, draw.stencil);
            // Only solid pixels set the attributes, since translucent ones keep the solid polygon's ID underneath
            GX2SetTargetChannelMasks(GX2_CHANNEL_MASK_RGBA,
                (writeAttribs && !draw.stencil) ? GX2_CHANNEL_MASK_RGBA : NO_CHANNELS,
                NO_CHANNELS, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS);
            GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, draw.count, draw.first, 1);
        }

        if (gpuEdges) {
            // Mark edges with a pass over the screen, once the attributes are written out where it can read them
            // Marked pixels get the edge color blended in, and other pixels are dropped
            GX2Invalidate((GX2InvalidateMode)(GX2_INVALIDATE_MODE_COLOR_BUFFER | GX2_INVALIDATE_MODE_TEXTURE),
                attribBuffer.surface.image, attribBuffer.surface.imageSize);
            WHBGfxShaderGroup &group = groups[4];
            GX2SetFetchShader(&group.fetchShader);
            GX2SetVertexShader(group.vertexShader);
            GX2SetPixelShader(group.pixelShader);
            GX2SetPixelTexture(&attribTexture, 0);
            GX2SetPixelSampler(&samplers[0], 0);
            GX2SetPixelTexture(&edgeTexture, 1);
            GX2SetPixelSampler(&samplers[0], 1);
            GX2SetDepthStencilControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS, FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS,
                GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP, GX2_COMPARE_FUNC_ALWAYS,
                GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP);
            GX2SetAlphaTest(FALSE, GX2_COMPARE_FUNC_ALWAYS, 0.0f);
            GX2SetColorControl(GX2_LOGIC_OP_COPY, 0x01, FALSE, TRUE);
            GX2SetTargetChannelMasks(GX2_CHANNEL_MASK_RGBA, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS,
                NO_CHANNELS, NO_CHANNELS);
            GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, 6, count, 1);
        }

        // Write the color buffers out to memory, and wait for the GPU to finish without holding GX2, so the UI can
        // submit its frame in the meantime
        GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, colorBuffer.surface.image, colorBuffer.surface.imageSize);
        if (finish)
            GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, attribBuffer.surface.image, attribBuffer.surface.imageSize);
        GX2Flush();
        OSTime submitted = GX2GetLastSubmittedTimeStamp();
        guard.unlock();
        GX2WaitTimeStamp(submitted);
        HB_ADD_US(HB_GPU_DRAW_US, hbDraw);
    }

#ifdef GPU_DUMP
    // Debug builds also draw dumped frames in software, before marking any scanline done so the frame can't change
    std::vector<uint32_t> software;
    if (frameCount % 15 == 0) {
        software.resize(256 * 192);
        renderer->softwareFrame(software.data());
    }
#endif

    // Convert each scanline to the renderer's format, with 6-bit channels and the 3D pixel bit, and mark it done
    // With fog, the renderer applies it, edge marking, and anti-aliasing from the attributes the same way it does for
    // its own scanlines, which needs the scanlines around each one converted first
    HB_SET(HB_GPU_STAGE, 5);
    HB_TIMER(hbConvert);
    bool edges = disp3DCnt & BIT(5);
    bool solid[2][256];
    DCInvalidateRange(colorBuffer.surface.image, colorBuffer.surface.imageSize);
    if (finish)
        DCInvalidateRange(attribBuffer.surface.image, attribBuffer.surface.imageSize);
    for (int y = 0; y < 192; y++) {
        const uint32_t *src = (uint32_t*)colorBuffer.surface.image + y * colorBuffer.surface.pitch;
        uint32_t *dst = renderer->hardwareLine(y);
        for (int x = 0; x < 256; x++) {
            uint32_t p = src[x];
            dst[x] = BIT(26) | (((p >> 2) & 0x3F) << 18) | (((p >> 10) & 0x3F) << 12) | (((p >> 18) & 0x3F) << 6) | ((p >> 26) & 0x3F);
        }
        if (!finish) {
            renderer->hardwareDone(y);
            continue;
        }

        // Fill in the polygon IDs, fog bits, and depths of solid pixels, or the clear values elsewhere
        // The original colors go in the back layer, which anti-aliasing blends marked edges with
        // The GPU writes the floats little-endian
        const uint32_t *attrib = (uint32_t*)attribBuffer.surface.image + y * attribBuffer.surface.pitch * 2;
        uint32_t *attribs = renderer->hardwareAttribs(y);
        int32_t *depths = renderer->hardwareDepths(y);
        uint32_t *back = renderer->hardwareBackLine(y);
        bool *solidLine = solid[y & 1], *solidPrev = solid[(y - 1) & 1];
        for (int x = 0; x < 256; x++) {
            uint32_t bits[2] = { __builtin_bswap32(attrib[x * 2]), __builtin_bswap32(attrib[x * 2 + 1]) };
            float a[2];
            memcpy(a, bits, sizeof(a));
            uint32_t value = uint32_t(a[0] + 0.5f);
            back[x] = dst[x];
            solidLine[x] = value & 1;
            if (value & 1) {
                attribs[x] = ((value >> 2) & 0x3F) | ((value & 2) << 12) | (0x3F << 15);
                depths[x] = int32_t(a[1]);
            }
            else {
                attribs[x] = clearAttrib;
                depths[x] = clearDepthValue;
            }
        }

        // Set the edge bit on solid pixels next to a different ID with greater depth, comparing each pair of
        // neighbors once, so the renderer only checks those for edge marking instead of every solid pixel
        // The depth has to be greater by a margin, since the GPU's depths for surfaces that touch, like windows on
        // walls, can differ slightly where the DS's are equal, which made edges flicker
        // Pixels on the screen border are compared to the clear values by the renderer, so they're always checked
        auto further = [](int32_t depth, int32_t other) { return other - depth > (depth >> 8); };
        if (edges) {
            uint32_t *prevAttribs = renderer->hardwareAttribs(std::max(y - 1, 0));
            int32_t *prevDepths = renderer->hardwareDepths(std::max(y - 1, 0));
            for (int x = 0; x < 256; x++) {
                if (x > 0 && ((attribs[x] ^ attribs[x - 1]) & 0x3F)) {
                    if (solidLine[x] && further(depths[x], depths[x - 1])) attribs[x] |= BIT(14);
                    if (solidLine[x - 1] && further(depths[x - 1], depths[x])) attribs[x - 1] |= BIT(14);
                }
                if (y > 0 && ((attribs[x] ^ prevAttribs[x]) & 0x3F)) {
                    if (solidLine[x] && further(depths[x], prevDepths[x])) attribs[x] |= BIT(14);
                    if (solidPrev[x] && further(prevDepths[x], depths[x])) prevAttribs[x] |= BIT(14);
                }
                if ((y == 0 || y == 191 || x == 0 || x == 255) && solidLine[x])
                    attribs[x] |= BIT(14);
            }
        }
        if (y > 0) {
            renderer->hardwareFinish(y - 1);
            renderer->hardwareDone(y - 1);
        }
    }
    if (finish) {
        renderer->hardwareFinish(191);
        renderer->hardwareDone(191);
    }
    HB_ADD_US(HB_GPU_CONVERT_US, hbConvert);

#ifdef GPU_DUMP
    // Debug builds save what the GPU was given and what it drew 4 times a second, cycling through 20 files
    // They also redraw the solid polygons untextured, colored by polygon index, to see which one won each pixel,
    // and save the textures as the GPU has them
    if (frameCount % 15 == 0) {
        std::vector<uint32_t> image((uint32_t*)colorBuffer.surface.image,
            (uint32_t*)colorBuffer.surface.image + colorBuffer.surface.pitch * 192);
        std::vector<GpuVertex> original(vertices, vertices + count);
        for (size_t k = 0; k < dumpMap.size(); k += 2) {
            uint32_t end = (k + 2 < dumpMap.size()) ? dumpMap[k + 3] : count;
            for (uint32_t v = dumpMap[k + 1]; v < end; v++) {
                vertices[v].color[0] = (dumpMap[k] & 0xFF) / 255.0f;
                vertices[v].color[1] = (dumpMap[k] >> 8) / 255.0f;
                vertices[v].color[2] = 0.0f;
                vertices[v].color[3] = 1.0f;
            }
        }
        {
            std::lock_guard<std::mutex> guard(gx2Mutex);
            GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, vertices, count * sizeof(GpuVertex));
            GX2SetContextState(state);
            GX2ClearColor(&colorBuffer, 1.0f, 1.0f, 1.0f, 1.0f);
            GX2ClearDepthStencilEx(&depthBuffer, depthClear, 0, GX2_CLEAR_FLAGS_BOTH);
            GX2SetContextState(state);
            GX2SetColorBuffer(&colorBuffer, GX2_RENDER_TARGET_0);
            GX2SetDepthBuffer(&depthBuffer);
            GX2SetViewport(0, 0, 256, 192, 0, 1);
            GX2SetScissor(0, 0, 256, 192);
            GX2SetRasterizerClipControlHalfZ(TRUE, FALSE, TRUE);
            GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, FALSE);
            GX2SetAlphaTest(FALSE, GX2_COMPARE_FUNC_GREATER, 0.0f);
            GX2SetColorControl(GX2_LOGIC_OP_COPY, 0x00, FALSE, TRUE);
            GX2SetTargetChannelMasks(GX2_CHANNEL_MASK_RGBA, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS,
                NO_CHANNELS, NO_CHANNELS);
            GX2SetAttribBuffer(0, count * sizeof(GpuVertex), sizeof(GpuVertex), vertices);
            WHBGfxShaderGroup &group = groups[0];
            GX2SetFetchShader(&group.fetchShader);
            GX2SetVertexShader(group.vertexShader);
            GX2SetPixelShader(group.pixelShader);
            for (GpuDraw &draw : draws) {
                if (draw.translucent || draw.stencil) continue;
                GX2CompareFunction func = wBuffer ? (draw.depthEqual ? GX2_COMPARE_FUNC_GEQUAL : GX2_COMPARE_FUNC_GREATER) :
                    (draw.depthEqual ? GX2_COMPARE_FUNC_LEQUAL : GX2_COMPARE_FUNC_LESS);
                GX2SetDepthOnlyControl(TRUE, draw.depthWrite, func);
                GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, draw.count, draw.first, 1);
            }
            GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, colorBuffer.surface.image, colorBuffer.surface.imageSize);
            GX2DrawDone();
        }
        DCInvalidateRange(colorBuffer.surface.image, colorBuffer.surface.imageSize);

        std::string path = Settings::basePath + "/gpudump" + std::to_string((frameCount / 15) % 20) + ".bin";
        if (FILE *file = fopen(path.c_str(), "wb")) {
            uint32_t header[] = { 0x47444D54, frameCount, count, (uint32_t)draws.size(), wBuffer, (uint32_t)dumpMap.size() / 2,
                colorBuffer.surface.pitch, (uint32_t)sizeof(GpuDraw) };
            fwrite(header, sizeof(header), 1, file);
            fwrite(draws.data(), sizeof(GpuDraw), draws.size(), file);
            fwrite(dumpMap.data(), 4, dumpMap.size(), file);
            fwrite(original.data(), sizeof(GpuVertex), count, file);
            fwrite(image.data(), 4, image.size(), file);
            fwrite(colorBuffer.surface.image, 4, colorBuffer.surface.pitch * 192, file);
            fwrite(software.data(), 4, software.size(), file);
            fwrite(&disp3DCnt, 2, 1, file);
            fwrite(&disp3DCnt, 2, 1, file);
            for (int y = 0; y < 192; y++)
                fwrite(renderer->hardwareLine(y), 4, 256, file);
            fwrite(attribBuffer.surface.image, 8, attribBuffer.surface.pitch * 192, file);
            for (auto &entry : textures) {
                GX2Texture *texture = entry.second.texture;
                if (!texture || entry.second.frame != frameCount) continue;
                DCInvalidateRange(texture->surface.image, texture->surface.imageSize);
                uint32_t info[] = { (uint32_t)texture, texture->surface.width, texture->surface.height,
                    texture->surface.pitch, (uint32_t)entry.first, (uint32_t)(entry.first >> 32) };
                fwrite(info, sizeof(info), 1, file);
                for (uint32_t y = 0; y < texture->surface.height; y++)
                    fwrite((uint32_t*)texture->surface.image + y * texture->surface.pitch, 4, texture->surface.width, file);
            }
            fclose(file);
        }
    }
#endif
}

} // namespace

void initGpu3D() {
    // Make the GPU renderer available to the core if it could be set up
    Gpu3DWiiU *gpu3D = new Gpu3DWiiU();
    if (gpu3D->init())
        Gpu3DRenderer::hardware = gpu3D;
}

#endif // __WIIU__
