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


#include <coreinit/cache.h>
#include <coreinit/debug.h>
#include <coreinit/internal.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
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
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unordered_map>
#include <vector>

#include "gpu3d_wiiu.h"
#if defined(PROFILE_EMU) && defined(HEARTBEAT)
#include "frame_trace.h"
#endif
#include "../../core/core.h"
#include "../../core/heartbeat.h"
#include "../../core/frame_capture.h"

// The GPU 3D renderer's step and frame, which hang reports include: 1 = waiting, 2 = building, 3 = locking GX2,
// 4 = drawing, 5 = handing over scanlines, 6 = submitting, 7 = waiting for the GPU, 8 = timed out
volatile uint32_t gpu3dStage, gpu3dFrames;
#define GPU_STAGE(stage) do { gpu3dStage = (stage); HB_SET(HB_GPU_STAGE, stage); } while (0)
#include "../../core/settings.h"
#include "../../core/worker_thread.h"

extern const uint8_t gpu3d_color_gsh[], gpu3d_modulate_gsh[], gpu3d_decal_gsh[], gpu3d_modtrans_gsh[], gpu3d_edge_gsh[],
    gpu3d_fog_gsh[], gpu3d_aa_gsh[], gpu3d_toon_gsh[], gpu3d_pack_gsh[], gpu3d_clear_gsh[], gpu3d_fill_gsh[];

std::mutex gx2Mutex;

namespace {

// The vertex layout the shaders read: clip-space position, texture coordinates and color
const GX2ChannelMask NO_CHANNELS = (GX2ChannelMask)0;

struct GpuVertex {
    float pos[4];
    float tex[4]; // Texture coordinates, then the distances to 2 of the triangle's edges for anti-aliasing
    float color[4];
    float attrib[4]; // Polygon ID times 4 plus flags (1 = solid pixel, 2 = fog), the distance to the third edge, and two
                     // values for the depth
};

// Vertices fill 2 whole cache blocks in the 64-byte aligned vertex buffer, so they can be claimed before being written
static_assert(sizeof(GpuVertex) == 64, "GpuVertex must fill 2 cache blocks");

// A run of triangles drawn with the same state
struct GpuDraw {
    int shader; // 0 = vertex color, 1 = modulate, 2 = decal, 7 = toon or highlight
    GX2Texture *texture; // Null for untextured polygons
    int sampler;
    bool translucent;
    bool depthEqual;
    bool depthWrite;
    bool solid;
    uint8_t stencil; // The polygon ID, with the translucent flag for translucent polygons
    uint8_t shadow; // 0 for normal polygons, 1 for shadow masks, 2 for shadows
    bool clearMask; // Whether this starts a group of shadow masks, which clears the shadow bits first
    bool split; // Whether pixels can be opaque or translucent, so they're drawn separately to follow different rules
    bool lines; // Whether this draws the edges of wireframe polygons as lines, instead of triangles
    uint32_t first, count;
};

class Gpu3DWiiU: public Gpu3DHardware {
public:
    bool init();
    bool initMultisampling();
    void render(Gpu3DRenderer *renderer) override;

private:
    static const int MAX_VERTICES = 2048 * 8 * 3;
    static const int QUAD = MAX_VERTICES; // Where the screen quad is, after the polygons
    static const int CHUNK_POLYGONS = 256; // How many polygons a frame needs to be sent in pieces
    static const uint32_t FIRST_CHUNK_VERTICES = 768, CHUNK_VERTICES = 1536; // How many vertices go in each piece
    // How far short of half a pixel vertices are moved; on long edges, a bigger distance also takes in pixels that are
    // really outside the polygon, whose texture coordinates can then fall on the next part of the texture
    static constexpr float EDGE_BIAS = 1.0f / 256;
    static const uint32_t MAX_EDGE_INDICES = 2048 * 10 * 2; // Enough for every edge of the most polygons

    WHBGfxShaderGroup groups[11];
    GX2Sampler samplers[9];
    GX2ContextState *state = nullptr;
    GX2ColorBuffer colorBuffer;
    GX2ColorBuffer attribBuffer; // Per-pixel attributes of solid polygons, for edge marking and fog
    GX2Texture attribTexture; // The attributes again, for the edge marking pass to read
    GX2ColorBuffer attribCompact; // The attributes in 16 bits each, which is faster when edge marking doesn't need more
    GX2Texture attribCompactTexture;
    GX2Texture colorTexture; // The colors again, for the anti-aliasing pass to read
    GX2ColorBuffer packedBuffer; // The finished frame as the 2D engine's 3D pixels, so the CPU only copies it
    GX2Texture edgeTexture; // The 8 edge colors, with how much they're blended in as alpha
    GX2Texture toonTexture; // The 32 toon colors, for toon and highlight shading
    GX2Texture clearColorTexture, clearDepthTexture; // The rear-plane bitmap's colors, and its depths and fog flags
    uint8_t clearImages[2][256 * 256 * 2] = {}; // The bitmap as last converted, to only convert rows that change
    bool clearConverted = false;
    GX2Texture whiteTexture; // A white texel, for toon and highlight polygons without a texture
    GX2Texture fogTexture; // The fog density for each depth / 0x200, from 0 to 128
    uint8_t fogSettings[35] = {}; // The fog table, offset and shift the densities were made from
    GX2DepthBuffer depthBuffer;
    GX2ColorBuffer msaaColor; // Multisampled buffers, which anti-aliasing draws the polygons into and averages down
    GX2ColorBuffer msaaAttribs; // Never kept, but render targets drawn together have to be multisampled alike
    GX2DepthBuffer msaaDepth;
    GX2Surface resolved; // The averaged colors, before they're copied to the linear color buffer
    int msaaState = 0; // 0 until the multisampled buffers are first needed, then 1 if they were made or -1 if not
    GpuVertex *vertices = nullptr;
    uint16_t *edgeIndices = nullptr; // The vertices of solid polygons' edges, for finding cracks between polygons

    // Decoded textures are kept between frames, and decoded again when the data they came from changes
    struct CachedTexture {
        GX2Texture *texture;
        uint64_t hash;
        uint32_t frame;
        uint32_t mapChanges; // Texture mapping changes when its data was last checked
    };
    std::unordered_map<uint64_t, CachedTexture> textures;
    std::vector<uint32_t> decoded;
    std::vector<GX2Texture*> updated;
    std::vector<GpuDraw> draws;
    uint32_t frameCount = 0;
    uint64_t lastTextureKey = 0; // The last texture polygons used this frame; no key is 0, since textures have a format
    GX2Texture *lastTexture = nullptr;

    std::mutex mutex;
    std::condition_variable cond;
    Gpu3DRenderer *job = nullptr;
    std::atomic<bool> busy;

    static int threadEntry(int argc, void *argv);
    void run();
    void renderFrame(Gpu3DRenderer *renderer);
    GX2Texture *getTexture(Gpu3DRenderer *renderer, _Polygon *polygon);
    GX2Texture *lookUpTexture(Gpu3DRenderer *renderer, _Polygon *polygon, uint64_t key);
    void freeTextures();
};

bool Gpu3DWiiU::init() {
    // Load the shaders, which share one vertex shader and its layout
    const uint8_t *shaders[] = { gpu3d_color_gsh, gpu3d_modulate_gsh, gpu3d_decal_gsh, gpu3d_modtrans_gsh, gpu3d_edge_gsh,
        gpu3d_fog_gsh, gpu3d_aa_gsh, gpu3d_toon_gsh, gpu3d_pack_gsh, gpu3d_clear_gsh, gpu3d_fill_gsh };
    for (int i = 0; i < 11; i++) {
        if (!WHBGfxLoadGFDShaderGroup(&groups[i], 0, shaders[i])) return false;
        WHBGfxInitShaderAttribute(&groups[i], "position", 0, offsetof(GpuVertex, pos), GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32);
        WHBGfxInitShaderAttribute(&groups[i], "tex_coords", 0, offsetof(GpuVertex, tex), GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32);
        WHBGfxInitShaderAttribute(&groups[i], "vtx_color", 0, offsetof(GpuVertex, color), GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32);
        WHBGfxInitShaderAttribute(&groups[i], "vtx_attr", 0, offsetof(GpuVertex, attrib), GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32);
        if (!WHBGfxInitFetchShader(&groups[i])) return false;

        // The assembler can only enable the red channel of a second output, so enable all of them here
        if (i < 4 || i == 7 || i == 9 || i == 10) groups[i].pixelShader->regs.cb_shader_mask = 0xFF;
    }

    // Set up nearest-neighbor samplers for each combination of clamping, repeating and flipping
    static const GX2TexClampMode modes[] = { GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_CLAMP_MODE_WRAP, GX2_TEX_CLAMP_MODE_MIRROR };
    for (int i = 0; i < 9; i++) {
        GX2InitSampler(&samplers[i], GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_POINT);
        GX2InitSamplerClamping(&samplers[i], modes[i % 3], modes[i / 3], GX2_TEX_CLAMP_MODE_CLAMP);
    }

    // Create the color buffer, tiled since the GPU draws to tiled surfaces much faster, clears included; dump builds
    // keep the render targets linear so the CPU can read them
#ifdef GPU_DUMP
    const GX2TileMode targetTiling = GX2_TILE_MODE_LINEAR_ALIGNED;
#else
    const GX2TileMode targetTiling = GX2_TILE_MODE_DEFAULT;
#endif
    memset(&colorBuffer, 0, sizeof(colorBuffer));
    colorBuffer.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    colorBuffer.surface.width = 512; // Big enough for high-res frames; native ones use the top-left quarter
    colorBuffer.surface.height = 384;
    colorBuffer.surface.depth = 1;
    colorBuffer.surface.mipLevels = 1;
    colorBuffer.surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    colorBuffer.surface.aa = GX2_AA_MODE1X;
    colorBuffer.surface.use = (GX2SurfaceUse)(GX2_SURFACE_USE_COLOR_BUFFER | GX2_SURFACE_USE_TEXTURE);
    colorBuffer.surface.tileMode = targetTiling;
    colorBuffer.viewNumSlices = 1;
    GX2CalcSurfaceSizeAndAlignment(&colorBuffer.surface);
    GX2InitColorBufferRegs(&colorBuffer);
    colorBuffer.surface.image = MEMAllocFromDefaultHeapEx(colorBuffer.surface.imageSize, colorBuffer.surface.alignment);

    // Create a float buffer for the attributes, written as a second render target
    // Each pixel has the ID and flags, and the depth the DS would compare, which the shaders get by dividing the last 2
    // attributes: W interpolates to itself across the screen, and Z times W over W interpolates to Z
    memset(&attribBuffer, 0, sizeof(attribBuffer));
    attribBuffer.surface = colorBuffer.surface;
    attribBuffer.surface.format = GX2_SURFACE_FORMAT_FLOAT_R32_G32;
    attribBuffer.surface.use = (GX2SurfaceUse)(GX2_SURFACE_USE_COLOR_BUFFER | GX2_SURFACE_USE_TEXTURE);
    attribBuffer.surface.image = nullptr;
    attribBuffer.surface.tileMode = targetTiling;
    attribBuffer.surface.swizzle = 0;
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

    // Create another attributes buffer with half the bits, for frames without edge marking; drawing to it takes
    // noticeably less time in high-res, and fog and anti-aliasing work with its precision
    memset(&attribCompact, 0, sizeof(attribCompact));
    attribCompact.surface = colorBuffer.surface;
    attribCompact.surface.format = GX2_SURFACE_FORMAT_UNORM_R16_G16;
    attribCompact.surface.use = (GX2SurfaceUse)(GX2_SURFACE_USE_COLOR_BUFFER | GX2_SURFACE_USE_TEXTURE);
    attribCompact.surface.image = nullptr;
    attribCompact.surface.tileMode = targetTiling;
    attribCompact.surface.swizzle = 0;
    attribCompact.viewNumSlices = 1;
    GX2CalcSurfaceSizeAndAlignment(&attribCompact.surface);
    GX2InitColorBufferRegs(&attribCompact);
    attribCompact.surface.image = MEMAllocFromDefaultHeapEx(attribCompact.surface.imageSize, attribCompact.surface.alignment);
    memset(&attribCompactTexture, 0, sizeof(attribCompactTexture));
    attribCompactTexture.surface = attribCompact.surface;
    attribCompactTexture.surface.use = GX2_SURFACE_USE_TEXTURE;
    attribCompactTexture.viewNumSlices = 1;
    attribCompactTexture.compMap = 0x00010405;
    GX2InitTextureRegs(&attribCompactTexture);

    // Let the colors be read as a texture too, and create a linear buffer for the packed results, which the CPU reads
    // and the high-res copy for the UI is made from
    memset(&colorTexture, 0, sizeof(colorTexture));
    colorTexture.surface = colorBuffer.surface;
    colorTexture.surface.use = GX2_SURFACE_USE_TEXTURE;
    colorTexture.viewNumSlices = 1;
    colorTexture.compMap = 0x00010203;
    GX2InitTextureRegs(&colorTexture);
    memset(&packedBuffer, 0, sizeof(packedBuffer));
    packedBuffer.surface = colorBuffer.surface;
    packedBuffer.surface.use = GX2_SURFACE_USE_COLOR_BUFFER;
    packedBuffer.surface.image = nullptr;
    packedBuffer.surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    packedBuffer.surface.swizzle = 0;
    packedBuffer.viewNumSlices = 1;
    GX2CalcSurfaceSizeAndAlignment(&packedBuffer.surface);
    GX2InitColorBufferRegs(&packedBuffer);
    packedBuffer.surface.image = MEMAllocFromDefaultHeapEx(packedBuffer.surface.imageSize, packedBuffer.surface.alignment);

    // Create the toon color texture and a white one, which are small textures like the edge colors
    auto smallTexture = [](GX2Texture &texture, uint32_t width, uint32_t height = 1,
            GX2SurfaceFormat format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, uint32_t compMap = 0x00010203) {
        memset(&texture, 0, sizeof(texture));
        texture.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
        texture.surface.width = width;
        texture.surface.height = height;
        texture.surface.depth = 1;
        texture.surface.mipLevels = 1;
        texture.surface.format = format;
        texture.surface.aa = GX2_AA_MODE1X;
        texture.surface.use = GX2_SURFACE_USE_TEXTURE;
        texture.surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
        texture.viewNumSlices = 1;
        texture.compMap = compMap;
        GX2CalcSurfaceSizeAndAlignment(&texture.surface);
        GX2InitTextureRegs(&texture);
        texture.surface.image = MEMAllocFromDefaultHeapEx(texture.surface.imageSize, texture.surface.alignment);
        return texture.surface.image != nullptr;
    };
    if (!smallTexture(toonTexture, 32) || !smallTexture(whiteTexture, 1)) return false;

    // Create textures for the rear-plane bitmap, which has 256x256 colors, and depths with fog flags
    if (!smallTexture(clearColorTexture, 256, 256) ||
            !smallTexture(clearDepthTexture, 256, 256, GX2_SURFACE_FORMAT_FLOAT_R32_G32, 0x00010405))
        return false;
    *(uint32_t*)whiteTexture.surface.image = 0xFFFFFFFF;
    DCFlushRange(whiteTexture.surface.image, whiteTexture.surface.imageSize);

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

    // Create the fog density texture, with an entry for each of the 0x8000 depth steps
    memset(&fogTexture, 0, sizeof(fogTexture));
    fogTexture.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    fogTexture.surface.width = 256;
    fogTexture.surface.height = 128;
    fogTexture.surface.depth = 1;
    fogTexture.surface.mipLevels = 1;
    fogTexture.surface.format = GX2_SURFACE_FORMAT_UNORM_R8;
    fogTexture.surface.aa = GX2_AA_MODE1X;
    fogTexture.surface.use = GX2_SURFACE_USE_TEXTURE;
    fogTexture.surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    fogTexture.viewNumSlices = 1;
    fogTexture.compMap = 0x00040405;
    GX2CalcSurfaceSizeAndAlignment(&fogTexture.surface);
    GX2InitTextureRegs(&fogTexture);
    fogTexture.surface.image = MEMAllocFromDefaultHeapEx(fogTexture.surface.imageSize, fogTexture.surface.alignment);
    fogSettings[34] = 0xFF; // Not a valid shift, so the densities are made the first time

    memset(&depthBuffer, 0, sizeof(depthBuffer));
    depthBuffer.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    depthBuffer.surface.width = 512;
    depthBuffer.surface.height = 384;
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
    // There's room for a screen-sized quad at the end, for edge marking, fog, and clearing shadow marks
    vertices = (GpuVertex*)MEMAllocFromDefaultHeapEx((MAX_VERTICES + 6) * sizeof(GpuVertex), GX2_VERTEX_BUFFER_ALIGNMENT);
    edgeIndices = (uint16_t*)MEMAllocFromDefaultHeapEx(MAX_EDGE_INDICES * sizeof(uint16_t), GX2_INDEX_BUFFER_ALIGNMENT);
    if (!edgeIndices) return false;
    state = (GX2ContextState*)MEMAllocFromDefaultHeapEx(sizeof(GX2ContextState), GX2_CONTEXT_STATE_ALIGNMENT);
    if (!colorBuffer.surface.image || !attribBuffer.surface.image || !attribCompact.surface.image || !edgeTexture.surface.image || !fogTexture.surface.image ||
            !packedBuffer.surface.image ||
            !depthBuffer.surface.image || !vertices || !state)
        return false;
    GX2SetupContextStateEx(state, TRUE);

    // Render on a thread on the UI's core, which has to be the one that uses GX2, with priority over the UI
    busy.store(false);
    return WorkerThread::createThread(threadEntry, (void*)this, 0x10000, OSGetThreadPriority(OSGetCurrentThread()) - 1,
        OS_THREAD_ATTRIB_AFFINITY_CPU2) != nullptr;
}

int Gpu3DWiiU::threadEntry(int argc, void *argv) {
#if defined(PROFILE_EMU) && defined(HEARTBEAT)
    FrameTrace::startRenderer();
#endif
    ((Gpu3DWiiU*)argv)->run();
#if defined(PROFILE_EMU) && defined(HEARTBEAT)
    FrameTrace::stopRenderer();
#endif
    return 0;
}

bool Gpu3DWiiU::initMultisampling() {
    // Create the multisampled buffers for anti-aliasing, with 4 samples per pixel; color buffers also need an
    // auxiliary buffer, which starts out filled with the value that marks it as cleared
    auto multisample = [](GX2ColorBuffer &buffer, const GX2Surface &like) {
        memset(&buffer, 0, sizeof(buffer));
        buffer.surface = like;
        buffer.surface.aa = GX2_AA_MODE4X;
        buffer.surface.use = GX2_SURFACE_USE_COLOR_BUFFER;
        buffer.surface.tileMode = GX2_TILE_MODE_DEFAULT;
        buffer.surface.image = nullptr;
        buffer.viewNumSlices = 1;
        GX2CalcSurfaceSizeAndAlignment(&buffer.surface);
        uint32_t size, alignment;
        GX2CalcColorBufferAuxInfo(&buffer, &size, &alignment);
        buffer.aaBuffer = MEMAllocFromDefaultHeapEx(size, alignment);
        buffer.aaSize = size;
        buffer.surface.image = MEMAllocFromDefaultHeapEx(buffer.surface.imageSize, buffer.surface.alignment);
        if (!buffer.aaBuffer || !buffer.surface.image) return false;
        memset(buffer.aaBuffer, 0xCC, size);
        DCFlushRange(buffer.aaBuffer, size);
        GX2InitColorBufferRegs(&buffer);
        return true;
    };
    memset(&msaaDepth, 0, sizeof(msaaDepth));
    msaaDepth.surface = depthBuffer.surface;
    msaaDepth.surface.aa = GX2_AA_MODE4X;
    msaaDepth.surface.image = nullptr;
    msaaDepth.viewNumSlices = 1;
    msaaDepth.depthClear = 1.0f;
    GX2CalcSurfaceSizeAndAlignment(&msaaDepth.surface);
    GX2InitDepthBufferRegs(&msaaDepth);
    msaaDepth.surface.image = MEMAllocFromDefaultHeapEx(msaaDepth.surface.imageSize, msaaDepth.surface.alignment);
    memset(&resolved, 0, sizeof(resolved));
    resolved = colorBuffer.surface;
    resolved.tileMode = GX2_TILE_MODE_DEFAULT;
    resolved.image = nullptr;
    GX2CalcSurfaceSizeAndAlignment(&resolved);
    resolved.image = MEMAllocFromDefaultHeapEx(resolved.imageSize, resolved.alignment);
    return multisample(msaaColor, colorBuffer.surface) && multisample(msaaAttribs, attribBuffer.surface) &&
        msaaDepth.surface.image && resolved.image;
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
        GPU_STAGE(1);
        std::unique_lock<std::mutex> lock(mutex);
        cond.wait(lock, [&] { return job != nullptr; });
        Gpu3DRenderer *renderer = job;
        job = nullptr;
        lock.unlock();
        renderFrame(renderer);
        gpu3dFrames++;
        HB_INC(HB_GPU_FRAMES);
        busy.store(false);
    }
}

void Gpu3DWiiU::freeTextures() {
    // Free textures that haven't been used for a couple of seconds, which the GPU has finished with
    uint32_t bytes = 0;
    for (auto it = textures.begin(); it != textures.end();) {
        if (frameCount - it->second.frame > 120) {
            if (it->second.texture) {
                MEMFreeToDefaultHeap(it->second.texture->surface.image);
                delete it->second.texture;
            }
            it = textures.erase(it);
        }
        else {
            if (it->second.texture) bytes += it->second.texture->surface.imageSize;
            it++;
        }
    }
    HB_SET(HB_GPU_TEXTURES, textures.size());
    HB_SET(HB_GPU_TEXTURE_KB, bytes >> 10);
    (void)bytes;
}

GX2Texture *Gpu3DWiiU::getTexture(Gpu3DRenderer *renderer, _Polygon *polygon) {
    // Look up the texture by everything that affects how it decodes; sizes are powers of 2 from 8 to 1024
    uint64_t key = polygon->textureAddr | (uint64_t(polygon->paletteAddr) << 19) |
        (uint64_t(__builtin_ctz(polygon->sizeS) - 3) << 36) | (uint64_t(__builtin_ctz(polygon->sizeT) - 3) << 39) |
        (uint64_t(polygon->textureFmt) << 42) | (uint64_t(polygon->transparent0) << 45);

    // Polygons in a row often share a texture, so the lookup is only needed when it changes
    if (key != lastTextureKey) {
        lastTexture = lookUpTexture(renderer, polygon, key);
        lastTextureKey = key;
    }
    return lastTexture;
}

GX2Texture *Gpu3DWiiU::lookUpTexture(Gpu3DRenderer *renderer, _Polygon *polygon, uint64_t key) {
    CachedTexture &cached = textures[key];
    if (cached.texture && cached.frame == frameCount) return cached.texture;

    // Check the texture once per frame, which only takes hashing its data if the texture mappings changed, and keep the
    // decoded copy if the data is the same; the count is read first, so a change during this gets checked next frame
    cached.frame = frameCount;
    uint32_t mapChanges = renderer->getCore()->memory.textureMapChanges;
    if (cached.texture && cached.mapChanges == mapChanges) return cached.texture;
    cached.mapChanges = mapChanges;
    uint64_t hash = renderer->hashTexture(polygon);
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
    HB_SET(HB_GPU_BUILD_POLYS, polygonCount);
    GPU_STAGE(2);
    HB_SET(HB_GPU_DISP3DCNT, disp3DCnt);
    HB_TIMER(hbBuild);
    frameCount++;
    lastTextureKey = 0;

    // High-res frames are drawn at twice the size, with vertices already at that scale, in the same buffers
    int shift = renderer->getResShift();
    int width = 256 << shift, height = 192 << shift;
    float xScale = 1.0f / (128 << shift), yScale = 1.0f / (96 << shift);
    freeTextures();
    updated.clear();
    draws.clear();

    // Anti-aliasing is done like the DS by default, from how much of each edge pixel its polygon covers, or optionally
    // by multisampling
    bool dsAntiAlias = (disp3DCnt & BIT(4)) && Settings::gpuAntiAlias == 0;

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

    // Edge marking, fog and anti-aliasing like the DS are done on the GPU after the polygons, from the attributes of
    // solid pixels
    bool gpuEdges = disp3DCnt & BIT(5);
    bool fog = disp3DCnt & BIT(7);

    // With anti-aliasing or edge marking, the DS draws the edges of polygons fully, covering more than the GPU does, so
    // it hides the gaps that rounding vertices to pixels leaves between polygons; those gaps are filled after drawing
#ifdef GPU_NO_CRACK_FILL
    // Debug builds can leave this out, to compare how long frames take without it
    bool fillCracks = false;
#else
    bool fillCracks = disp3DCnt & (BIT(4) | BIT(5));
#endif
    uint32_t edgeCount = 0;
    bool writeAttribs = gpuEdges || fog || dsAntiAlias || fillCracks;

    // Pixels are dropped if their alpha is 0, or with the alpha test, if their 5-bit alpha isn't greater than the
    // reference value; the GPU's alphas are 6-bit ones out of 63, so the cutoff is between the 6-bit values
    float alphaMin = (disp3DCnt & BIT(2)) ? (renderer->getAlphaTestRef() * 2 + 1.5f) / 63 : 0.0f;

    // Make the buffers for anti-aliasing by multisampling the first time it's enabled
    // With it, the polygons are first drawn multisampled and averaged down, and then again without color for the
    // attributes, since polygon IDs and depths can't be averaged; this isn't how the DS does it, but it smooths the
    // same edges, and more
    if (Settings::gpuAntiAlias == 1 && !msaaState)
        msaaState = initMultisampling() ? 1 : -1;
    bool msaa = (msaaState == 1) && Settings::gpuAntiAlias == 1 && (disp3DCnt & BIT(4));

    // Without edge marking, the attributes fit in 16 bits each: the ID and flags are scaled down, biased so they never
    // round below a whole number, and the depth is scaled down, or for W, which varies a lot more, its log is; the
    // shaders that write them get the encoding, and the ones that read them get how to undo it
    bool compact = writeAttribs && !gpuEdges && !msaa;
    GX2ColorBuffer &attribTarget1 = compact ? attribCompact : attribBuffer;
    GX2Texture &attribRead = compact ? attribCompactTexture : attribTexture;
    float attribEncode[8] = { 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    if (compact) {
        attribEncode[0] = 1.0f / 256;
        attribEncode[1] = 1.0f / 65535;
        attribEncode[2] = 1.0f / 0x1000000;
        attribEncode[3] = 1.0f / 32;
        attribEncode[4] = wBuffer ? 1.0f : 0.0f;
    }

    // Get the clear values; the clear color is 5-bit with a 5-bit alpha
    uint32_t clear = renderer->getClearColor();
    float clearR = ((clear >> 0) & 0x1F) / 31.0f, clearG = ((clear >> 5) & 0x1F) / 31.0f;
    float clearB = ((clear >> 10) & 0x1F) / 31.0f, clearA = ((clear >> 16) & 0x1F) / 31.0f;
    uint16_t clearDepth = renderer->getClearDepth();
    float depthClear = wBuffer ? 0.0f : (clearDepth == 0x7FFF) ? 1.0f : float(clearDepth << 9) / 0x1000000;
    uint32_t clearAttrib = ((clear & BIT(15)) >> 2) | ((clear & 0x3F000000) >> 18) | ((clear & 0x3F000000) >> 24) | (0x3F << 15);
    int32_t clearDepthValue = (clearDepth == 0x7FFF) ? 0xFFFFFF : (clearDepth << 9);
    float clearIdValue = float(clearAttrib & 0x3F) * 4 + ((clearAttrib & BIT(13)) ? 2 : 0);
    float clearDepthAttrib = clearDepthValue;
    if (compact) {
        clearIdValue = clearIdValue * attribEncode[0] + attribEncode[1];
        clearDepthAttrib = wBuffer ? (log2f(std::max(clearDepthAttrib, 1.0f)) / 32) : (clearDepthAttrib / 0x1000000);
    }

    // Set the toon colors, which toon and highlight shading look up by red level
    {
        uint32_t *texels = (uint32_t*)toonTexture.surface.image;
        for (int i = 0; i < 32; i++) {
            uint16_t color = renderer->getToon(i);
            texels[i] = ((((color >> 0) & 0x1F) * 255 / 31) << 24) | ((((color >> 5) & 0x1F) * 255 / 31) << 16) |
                ((((color >> 10) & 0x1F) * 255 / 31) << 8) | 0xFF;
        }
    }
    float toonMode[] = { (disp3DCnt & BIT(1)) ? 1.0f : 0.0f, (disp3DCnt & BIT(1)) ? 0.0f : 1.0f, 0, 0 };

    bool bitmap = disp3DCnt & BIT(14), clearChanged = false;
    if (bitmap) {
        // Convert the rear-plane bitmap from texture slots 2 and 3: colors are opaque or transparent by bit 15, and
        // depths are expanded like the clear depth, with the fog flag in bit 15
        // Only rows that changed since they were last converted are converted again, which usually is none
        uint8_t *colors = renderer->getCore()->memory.tex3D[2], *depths = renderer->getCore()->memory.tex3D[3];
        uint32_t *colorTexels = (uint32_t*)clearColorTexture.surface.image;
        float *depthTexels = (float*)clearDepthTexture.surface.image;
        static const uint8_t blank[512] = {};
        for (int y = 0; y < 256; y++) {
            const uint8_t *colorRow = colors ? &colors[y * 512] : blank, *depthRow = depths ? &depths[y * 512] : blank;
            if (clearConverted && !memcmp(colorRow, &clearImages[0][y * 512], 512) &&
                    !memcmp(depthRow, &clearImages[1][y * 512], 512))
                continue;
            memcpy(&clearImages[0][y * 512], colorRow, 512);
            memcpy(&clearImages[1][y * 512], depthRow, 512);
            clearChanged = true;
            for (int x = 0; x < 256; x++) {
                uint16_t c = U8TO16(colorRow, x * 2), d = U8TO16(depthRow, x * 2);
                colorTexels[y * clearColorTexture.surface.pitch + x] = ((((c >> 0) & 0x1F) * 255 / 31) << 24) |
                    ((((c >> 5) & 0x1F) * 255 / 31) << 16) | ((((c >> 10) & 0x1F) * 255 / 31) << 8) | ((c & BIT(15)) ? 0xFF : 0);
                float *texel = &depthTexels[(y * clearDepthTexture.surface.pitch + x) * 2];
                texel[0] = ((d & 0x7FFF) == 0x7FFF) ? 0xFFFFFF : ((d & 0x7FFF) << 9);
                texel[1] = (d & BIT(15)) ? 1.0f : 0.0f;
            }
        }
        clearConverted = true;
    }

    if (gpuEdges) {
        // Set the edge colors, blended in halfway with anti-aliasing, which softens marked edges
        uint32_t *texels = (uint32_t*)edgeTexture.surface.image;
        for (int i = 0; i < 8; i++) {
            uint16_t color = renderer->getEdgeColor(i);
            texels[i] = ((((color >> 0) & 0x1F) * 255 / 31) << 24) | ((((color >> 5) & 0x1F) * 255 / 31) << 16) |
                ((((color >> 10) & 0x1F) * 255 / 31) << 8) | ((disp3DCnt & BIT(4)) ? 0x80 : 0xFF);
        }
    }

    bool fogUpdated = false;
    if (fog) {
        // Make the fog densities again if their settings changed, with the renderer's formula for each depth step
        uint8_t settings[35];
        for (int i = 0; i < 32; i++)
            settings[i] = renderer->getFogTable(i);
        uint16_t fogOffset = renderer->getFogOffset();
        settings[32] = fogOffset >> 8;
        settings[33] = fogOffset & 0xFF;
        settings[34] = (disp3DCnt >> 8) & 0xF;
        if (memcmp(settings, fogSettings, sizeof(settings))) {
            memcpy(fogSettings, settings, sizeof(settings));
            int fogStep = 0x400 >> settings[34];
            uint8_t *texels = (uint8_t*)fogTexture.surface.image;
            for (int d = 0; d < 0x8000; d++) {
                int32_t offset = d - fogOffset;
                int n = (fogStep > 0) ? (offset / fogStep - 1) : ((offset > 0) ? 31 : 0);
                uint8_t density;
                if (n >= 31) {
                    density = settings[31];
                }
                else if (n < 0 || fogStep == 0) {
                    density = settings[0];
                }
                else {
                    int m = offset % fogStep;
                    density = (m >= 0) ? ((settings[n + 1] * m + settings[n] * (fogStep - m)) / fogStep) : settings[0];
                }
                texels[(d >> 8) * fogTexture.surface.pitch + (d & 0xFF)] = (density == 127) ? 128 : density;
            }
            fogUpdated = true;
        }
    }

    // Add a quad covering the screen after the room for polygons, for edge marking, fog, anti-aliasing, and clearing
    // shadow marks; it passes the coordinates of each pixel and its left and right, then down and up neighbors in the
    // attributes texture
    static const float corners[6][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    for (int i = 0; i < 6; i++) {
        float u = corners[i][0], v = corners[i][1];
        GpuVertex &out = vertices[QUAD + i];
        float tu = u * width / 512, tv = v * height / 384;
        out = { { u * 2 - 1, 1 - v * 2, 0, 1 }, { tu, tv }, { tu + 1.0f / 512, tv, tu - 1.0f / 512, tv },
            { tu, tv + 1.0f / 384, tu, tv - 1.0f / 384 } };
    }

    uint32_t count = 0; // Vertices built

    // How the polygons are drawn to the color, attributes and depth buffers for a pass, set up after clearing them
    GX2ChannelMask colorMask = GX2_CHANNEL_MASK_RGBA;
    bool attribs = writeAttribs;
    auto setupPass = [&](int pass) {
        GX2ColorBuffer &target = pass ? colorBuffer : msaaColor;
        GX2ColorBuffer &attribTarget = pass ? attribTarget1 : msaaAttribs;
        GX2DepthBuffer &depthTarget = pass ? depthBuffer : msaaDepth;
        colorMask = (pass == 0 || !msaa) ? GX2_CHANNEL_MASK_RGBA : NO_CHANNELS;
        attribs = writeAttribs && pass == 1;
        GX2SetContextState(state);
        if (colorMask)
            GX2ClearColor(&target, clearR, clearG, clearB, clearA);
        GX2ClearColor(&attribTarget, clearIdValue, clearDepthAttrib, 0, 0);
        GX2ClearDepthStencilEx(&depthTarget, depthClear, (clear >> 24) & 0x3F, GX2_CLEAR_FLAGS_BOTH); // Clear polygon ID
        GX2SetContextState(state);
        GX2SetColorBuffer(&target, GX2_RENDER_TARGET_0);
        GX2SetColorBuffer(&attribTarget, GX2_RENDER_TARGET_1);
        GX2SetDepthBuffer(&depthTarget);
        GX2SetViewport(0, 0, width, height, 0, 1);
        GX2SetScissor(0, 0, width, height);
        GX2SetRasterizerClipControlHalfZ(TRUE, FALSE, TRUE);
        GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, FALSE);
        GX2SetLineWidth(1.0f);
        GX2SetAlphaTest(TRUE, GX2_COMPARE_FUNC_GREATER, alphaMin);
        GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_INV_SRC_ALPHA,
            GX2_BLEND_COMBINE_MODE_ADD, TRUE, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_ONE, GX2_BLEND_COMBINE_MODE_MAX);
        GX2SetAttribBuffer(0, (QUAD + 6) * sizeof(GpuVertex), sizeof(GpuVertex), vertices);
        GX2SetPixelUniformReg(8, 8, attribEncode);

        if (bitmap) {
            // Draw the rear-plane bitmap over the cleared buffers, with its depths written by the shader; the stencil
            // keeps the clear polygon ID it was cleared to
            WHBGfxShaderGroup &group = groups[9];
            GX2SetFetchShader(&group.fetchShader);
            GX2SetVertexShader(group.vertexShader);
            GX2SetPixelShader(group.pixelShader);
            GX2SetPixelTexture(&clearColorTexture, 0);
            GX2SetPixelSampler(&samplers[4], 0);
            GX2SetPixelTexture(&clearDepthTexture, 1);
            GX2SetPixelSampler(&samplers[4], 1);
            uint16_t offset = renderer->getClearImageOffset();
#ifdef FRAME_CAPTURE
            FrameCapture::renders[renderer->captureId % FrameCapture::RENDERS].clearImageOffset = offset | 0x10000;
#endif
            float scale = 1 << shift;
            // Pixel centers are half a pixel past whole numbers, so rounding them down is safe from small errors
            float params[] = { 512 / scale, 384 / scale, 0, 0, float(offset & 0xFF) + 0.5f,
                float(offset >> 8) + 0.5f, float(clearAttrib & 0x3F) * 4, wBuffer ? 1.0f : 0.0f };
            GX2SetPixelUniformReg(0, 8, params);
            float depthScale[] = { 1.0f / 0x1000000, minW, 0, 0 };
            GX2SetPixelUniformReg(16, 4, depthScale);
            GX2SetDepthStencilControl(TRUE, TRUE, GX2_COMPARE_FUNC_ALWAYS, FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS,
                GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP, GX2_COMPARE_FUNC_ALWAYS,
                GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP);
            GX2SetAlphaTest(FALSE, GX2_COMPARE_FUNC_ALWAYS, 0.0f);
            GX2SetColorControl(GX2_LOGIC_OP_COPY, 0x00, FALSE, TRUE);
            GX2SetTargetChannelMasks(colorMask, attribs ? GX2_CHANNEL_MASK_RGBA : NO_CHANNELS, NO_CHANNELS, NO_CHANNELS,
                NO_CHANNELS, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS);
            GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, 6, QUAD, 1);
            GX2SetAlphaTest(TRUE, GX2_COMPARE_FUNC_GREATER, alphaMin);
        }
    };

    // Draw a group of triangles
    auto submitDraw = [&](GpuDraw &draw) {
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
        if (draw.shader == 7) {
            // Toon and highlight shading also get the toon colors and which of them it is
            GX2SetPixelTexture(&toonTexture, 1);
            GX2SetPixelSampler(&samplers[0], 1);
            GX2SetPixelUniformReg(4, 4, toonMode);
        }
        GX2PrimitiveMode primitive = draw.lines ? GX2_PRIMITIVE_MODE_LINES : GX2_PRIMITIVE_MODE_TRIANGLES;
        GX2CompareFunction func = wBuffer ? (draw.depthEqual ? GX2_COMPARE_FUNC_GEQUAL : GX2_COMPARE_FUNC_GREATER) :
            (draw.depthEqual ? GX2_COMPARE_FUNC_LEQUAL : GX2_COMPARE_FUNC_LESS);
        GX2SetColorControl(GX2_LOGIC_OP_COPY, draw.translucent ? 0x01 : 0x00, FALSE, TRUE);

        // The stencil buffer's low bits track the polygon ID of each pixel, with a flag if it was translucent,
        // since translucent pixels aren't drawn over ones from the same ID; the top bit marks where shadow masks
        // were hidden, which is where shadows are drawn
        if (draw.shadow == 1) {
            if (draw.clearMask) {
                // Clear the marks over the whole screen at the start of a group of masks
                GX2SetDepthStencilControl(TRUE, FALSE, GX2_COMPARE_FUNC_ALWAYS, TRUE, FALSE,
                    GX2_COMPARE_FUNC_ALWAYS, GX2_STENCIL_FUNCTION_REPLACE, GX2_STENCIL_FUNCTION_KEEP,
                    GX2_STENCIL_FUNCTION_KEEP, GX2_COMPARE_FUNC_ALWAYS, GX2_STENCIL_FUNCTION_KEEP,
                    GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP);
                GX2SetStencilMask(0x80, 0x80, 0, 0x80, 0x80, 0);
                GX2SetAlphaTest(FALSE, GX2_COMPARE_FUNC_ALWAYS, 0.0f);
                GX2SetTargetChannelMasks(NO_CHANNELS, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS,
                    NO_CHANNELS, NO_CHANNELS, NO_CHANNELS);
                GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, 6, QUAD, 1);
            }

            // Masks draw nothing, but mark the pixels where they fail the depth test
            GX2SetDepthStencilControl(TRUE, FALSE, func, TRUE, FALSE,
                GX2_COMPARE_FUNC_ALWAYS, GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_REPLACE,
                GX2_STENCIL_FUNCTION_KEEP, GX2_COMPARE_FUNC_ALWAYS, GX2_STENCIL_FUNCTION_KEEP,
                GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP);
            GX2SetStencilMask(0x80, 0x80, 0x80, 0x80, 0x80, 0x80);
            GX2SetAlphaTest(FALSE, GX2_COMPARE_FUNC_ALWAYS, 0.0f);
            GX2SetTargetChannelMasks(NO_CHANNELS, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS,
                NO_CHANNELS, NO_CHANNELS, NO_CHANNELS);
            GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, draw.count, draw.first, 1);
            GX2SetAlphaTest(TRUE, GX2_COMPARE_FUNC_GREATER, alphaMin);
            return;
        }
        if (draw.shadow == 2) {
            // Shadows aren't drawn over pixels with their own ID, so first remove the marks under those
            GX2SetDepthStencilControl(TRUE, FALSE, GX2_COMPARE_FUNC_ALWAYS, TRUE, FALSE,
                GX2_COMPARE_FUNC_EQUAL, GX2_STENCIL_FUNCTION_REPLACE, GX2_STENCIL_FUNCTION_KEEP,
                GX2_STENCIL_FUNCTION_KEEP, GX2_COMPARE_FUNC_ALWAYS, GX2_STENCIL_FUNCTION_KEEP,
                GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP);
            GX2SetStencilMask(0x3F, 0x80, draw.stencil & 0x3F, 0x3F, 0x80, draw.stencil & 0x3F);
            GX2SetAlphaTest(FALSE, GX2_COMPARE_FUNC_ALWAYS, 0.0f);
            GX2SetTargetChannelMasks(NO_CHANNELS, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS,
                NO_CHANNELS, NO_CHANNELS, NO_CHANNELS);
            GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, draw.count, draw.first, 1);
            GX2SetAlphaTest(TRUE, GX2_COMPARE_FUNC_GREATER, alphaMin);

            // Then draw them where the marks are left, like translucent pixels, keeping the marks
            GX2SetDepthStencilControl(TRUE, draw.depthWrite, func, TRUE, FALSE,
                GX2_COMPARE_FUNC_EQUAL, GX2_STENCIL_FUNCTION_REPLACE, GX2_STENCIL_FUNCTION_KEEP,
                GX2_STENCIL_FUNCTION_KEEP, GX2_COMPARE_FUNC_ALWAYS, GX2_STENCIL_FUNCTION_KEEP,
                GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP);
            GX2SetStencilMask(0x80, 0x7F, 0xC0 | draw.stencil, 0x80, 0x7F, 0xC0 | draw.stencil);
            GX2SetTargetChannelMasks(colorMask, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS,
                NO_CHANNELS, NO_CHANNELS, NO_CHANNELS);
            GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, draw.count, draw.first, 1);
            return;
        }
        if (draw.split) {
            // Draw the opaque pixels first, then the translucent ones with a shader that drops opaque pixels
            // Only modulated textures can give both, since other modes use the polygon's alpha
            GX2SetAlphaTest(TRUE, GX2_COMPARE_FUNC_GREATER, std::max(0.99f, alphaMin));
            GX2SetDepthStencilControl(TRUE, TRUE, func, TRUE, FALSE,
                GX2_COMPARE_FUNC_ALWAYS, GX2_STENCIL_FUNCTION_REPLACE, GX2_STENCIL_FUNCTION_KEEP,
                GX2_STENCIL_FUNCTION_KEEP, GX2_COMPARE_FUNC_ALWAYS, GX2_STENCIL_FUNCTION_KEEP,
                GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP);
            GX2SetStencilMask(0x7F, 0x7F, draw.stencil & 0x3F, 0x7F, 0x7F, draw.stencil & 0x3F);
            GX2SetTargetChannelMasks(colorMask, attribs ? GX2_CHANNEL_MASK_RGBA : NO_CHANNELS,
                NO_CHANNELS, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS);
            GX2DrawEx(primitive, draw.count, draw.first, 1);
            GX2SetAlphaTest(TRUE, GX2_COMPARE_FUNC_GREATER, alphaMin);
            if (draw.shader != 1) return;
            WHBGfxShaderGroup &trans = groups[3];
            GX2SetFetchShader(&trans.fetchShader);
            GX2SetVertexShader(trans.vertexShader);
            GX2SetPixelShader(trans.pixelShader);
        }
        GX2SetDepthStencilControl(TRUE, draw.depthWrite, func, TRUE, FALSE,
            draw.solid ? GX2_COMPARE_FUNC_ALWAYS : GX2_COMPARE_FUNC_NOT_EQUAL, GX2_STENCIL_FUNCTION_REPLACE,
            GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP, GX2_COMPARE_FUNC_ALWAYS,
            GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP);
        GX2SetStencilMask(0x7F, 0x7F, draw.stencil, 0x7F, 0x7F, draw.stencil);
        // Only solid pixels set the attributes, since translucent ones keep the solid polygon's ID underneath
        GX2SetTargetChannelMasks(colorMask,
            (attribs && draw.solid) ? GX2_CHANNEL_MASK_RGBA : NO_CHANNELS,
            NO_CHANNELS, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS);
        GX2DrawEx(primitive, draw.count, draw.first, 1);
    };

    // On heavy frames, the polygons built so far are sent in pieces while the rest are built, so the GPU draws them
    // in the meantime; GX2 is only taken if it's free, and the context state is set again each time, since the UI can
    // draw in between; draws that were sent can't be extended
    bool chunked = Settings::gpuOverlap && !msaa && polygonCount >= CHUNK_POLYGONS;
#ifdef GPU_DUMP
    chunked = false;
#endif
    bool started = false;
    size_t sentDraws = 0, sentTextures = 0;
    uint32_t sentVertices = 0;
    std::unique_lock<std::mutex> guard(gx2Mutex, std::defer_lock);
    HB_TIMER(hbDraw);
#if defined(HEARTBEAT) && defined(GPU_TIMELINE)
    OSTime hbStart = 0;
    auto hbUs = [&]() { return (uint32_t)OSTicksToMicroseconds(OSGetSystemTime() - hbStart); };
    // Wait for the GPU to finish a pass, adding the time since the last one to its slot
    OSTime hbLapStart = 0;
    auto hbLap = [&](HeartbeatSlot slot) {
        GX2DrawDone();
        OSTime now = OSGetSystemTime();
        Heartbeat::values[slot] += (uint32_t)OSTicksToMicroseconds(now - hbLapStart);
        hbLapStart = now;
    };
#define HB_LAP(slot) hbLap(slot)
#else
#define HB_LAP(slot) do {} while (0)
#endif

    // Start the frame on the GPU once GX2 is held, making what the CPU wrote so far visible to it; this adds GPU
    // commands, so it has to be done with GX2 held
    auto startFrame = [&]() {
#ifdef HEARTBEAT
        hbDraw = std::chrono::steady_clock::now();
#endif
#if defined(HEARTBEAT) && defined(GPU_TIMELINE)
        // Time each part of the frame on the GPU by waiting for it to go idle between them, which serializes the frame
        // with the UI's drawing, but shows exactly where the GPU's time goes
        hbStart = OSGetSystemTime();
        GX2DrawDone();
        Heartbeat::values[HB_GPU_PRIOR_US] += hbUs();
        hbLapStart = OSGetSystemTime();
#endif
        if (gpuEdges)
            GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, edgeTexture.surface.image, edgeTexture.surface.imageSize);
        GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, toonTexture.surface.image, toonTexture.surface.imageSize);
        if (clearChanged) {
            GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, clearColorTexture.surface.image, clearColorTexture.surface.imageSize);
            GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, clearDepthTexture.surface.image, clearDepthTexture.surface.imageSize);
        }
        if (fogUpdated)
            GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, fogTexture.surface.image, fogTexture.surface.imageSize);
        GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, &vertices[QUAD], 6 * sizeof(GpuVertex));
        started = true;
    };
    auto showBuilt = [&]() {
        for (; sentTextures < updated.size(); sentTextures++)
            GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, updated[sentTextures]->surface.image,
                updated[sentTextures]->surface.imageSize);
        if (count > sentVertices)
            GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, &vertices[sentVertices],
                (count - sentVertices) * sizeof(GpuVertex));
        sentVertices = count;
    };
    auto sendBuilt = [&]() {
        showBuilt();
        for (; sentDraws < draws.size(); sentDraws++)
            submitDraw(draws[sentDraws]);
    };
    auto sendChunk = [&]() {
#if defined(HEARTBEAT) && defined(GPU_TIMELINE)
        // Draw all polygons at once after building, so their GPU time can be told apart from the rest
        return;
#endif
        if (!guard.try_lock()) return;
        if (!started) {
            startFrame();
            setupPass(1);
        }
        else {
            GX2SetContextState(state);
        }
        sendBuilt();
        GX2Flush();
        guard.unlock();
    };

    HB_ADD_US(HB_GPU_PRELOOP_US, hbBuild);
    bool inMasks = false;
#ifdef GPU_DUMP
    std::vector<uint32_t> dumpMap; // Pairs of polygon index and first vertex
#endif
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < polygonCount; i++) {
            _Polygon *polygon = &polygons[i];
            // Wireframe polygons have alpha 0, and their edges are drawn solid
            bool wireframe = (polygon->alpha == 0);
            bool translucent = ((polygon->alpha < 0x3F && !wireframe) || polygon->textureFmt == 1 ||
                polygon->textureFmt == 6);
            if (translucent != (pass == 1)) continue;
            if (polygon->size < 3) continue;
            if (count + (wireframe ? (polygon->size * 2) : ((polygon->size - 2) * 3)) > MAX_VERTICES) break;

            // Pick the shader and texture; toon and highlight polygons use a white texel if they aren't textured
            GpuDraw draw = {};
            bool textured = texturing && polygon->textureFmt != 0;
            HB_TICK_TIMER(hbTexture);
            draw.texture = textured ? getTexture(renderer, polygon) : nullptr;
            HB_ADD_TICKS(HB_GPU_TEXTURE_US, hbTexture);
            draw.lines = wireframe;
            draw.shader = !draw.texture ? 0 : (polygon->mode == 1 || polygon->mode == 3) ? 2 : 1;
            if (polygon->mode == 2) {
                draw.shader = 7;
                if (!draw.texture) draw.texture = &whiteTexture;
            }
            draw.sampler = (polygon->repeatS ? (polygon->flipS ? 2 : 1) : 0) + (polygon->repeatT ? (polygon->flipT ? 2 : 1) : 0) * 3;
            draw.translucent = translucent && blending;
            draw.depthEqual = polygon->depthTestEqual;
            draw.depthWrite = !translucent || polygon->transNewDepth;
            // Pixels are translucent if their final alpha isn't full, which is all of them for translucent polygons,
            // while full-alpha polygons with translucent textures can also give opaque pixels, which act like solid ones
            draw.solid = !translucent;
            draw.stencil = translucent ? (0x40 | polygon->id) : polygon->id;
            draw.split = translucent && polygon->alpha == 0x3F;

            // Shadow polygons with ID 0 are masks, which only mark where they're hidden; the first in a row clears
            // the marks of the last group, and their color doesn't matter
            if (polygon->mode == 3) {
                draw.shadow = polygon->id ? 2 : 1;
                draw.split = false;
                if (draw.shadow == 1) {
                    draw.texture = nullptr;
                    draw.shader = 0;
                    draw.depthWrite = false;
                    draw.clearMask = !inMasks;
                }
            }
            inMasks = (draw.shadow == 1);
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
            // Wireframe edges are opaque; toon and highlight colors come from the shader, by the red level
            float alpha = wireframe ? 1.0f : (polygon->alpha / 63.0f);
            auto convert = [&](GpuVertex &out, int k) {
                Vertex &v = vertex(k);
                float w = (v.w > 0) ? float(v.w) : 1.0f;
                // The DS passes the equal depth test within a margin, so those polygons are drawn that much closer,
                // which shadows and other decals on the ground rely on, since their depth isn't exactly the ground's
                float depth;
                if (polygon->depthTestEqual)
                    depth = wBuffer ? (minW / std::max(w - 0xFF, minW)) : (float(std::max(v.z - 0x200, 0)) / 0x1000000);
                else
                    depth = wBuffer ? (minW / w) : (float(v.z) / 0x1000000);
                out.pos[0] = ((float(v.x) + 0.5f - EDGE_BIAS) * xScale - 1) * w;
                out.pos[1] = (1 - (float(v.y) + 0.5f - EDGE_BIAS) * yScale) * w;
                out.pos[2] = depth * w;
                out.pos[3] = w;
                out.tex[0] = v.s / 16.0f - shiftS;
                out.tex[1] = v.t / 16.0f - shiftT;
                out.color[0] = ((v.color >> 0) & 0x3F) / 63.0f;
                out.color[1] = ((v.color >> 6) & 0x3F) / 63.0f;
                out.color[2] = ((v.color >> 12) & 0x3F) / 63.0f;
                out.color[3] = alpha;
                out.attrib[0] = polygon->id * 4 + 1 + (polygon->fog ? 2 : 0);
                out.attrib[1] = 0.0f;
                out.attrib[2] = wBuffer ? 1.0f : w;
                out.attrib[3] = wBuffer ? w : float(v.z) * w;
            };

            // Add the polygon as a fan of triangles
            // For anti-aliasing, each vertex also gets its distance to each of the triangle's edges that are edges of
            // the polygon, measured across rows for mostly horizontal edges and across columns for others like the DS
            // does, and from the pixel center; inner edges are far away, so the nearest one gives the coverage
            // They're interpolated with perspective like the other values, which is close enough within a pixel
            // Only solid polygons give coverage, and only when anti-aliasing like the DS, so others skip the work
            bool distances = dsAntiAlias && !translucent;
            HB_TICK_TIMER(hbVertex);
            GpuVertex converted[10]; // Each polygon vertex is converted once, since the fan shares the first one
            for (int k = 0; k < polygon->size; k++)
                convert(converted[k], k);
            HB_ADD_TICKS(HB_GPU_VERTEX_US, hbVertex);
            HB_TICK_TIMER(hbFan);

            // Wireframe polygons are drawn as lines along their edges instead, which aren't anti-aliased
            for (int k = 0; wireframe && k < polygon->size; k++) {
                for (int c = 0; c < 2; c++) {
                    GpuVertex &out = vertices[count++];
                    out = converted[(k + c) % polygon->size];
                    out.tex[2] = out.tex[3] = out.attrib[1] = 1000.0f;
                }
            }
            for (int j = 1; !wireframe && j + 1 < polygon->size; j++) {
                int tri[3] = { 0, j, j + 1 };
                float dist[3][3] = {};
                if (distances) {
                    bool outer[3] = { true, j + 2 == polygon->size, j == 1 }; // The edges opposite each vertex
                    for (int e = 0; e < 3; e++) {
                        // Write the edge's distance as a line along its major axis, with the vertex opposite on the
                        // positive side; it only needs one division
                        Vertex &a = vertex(tri[(e + 1) % 3]), &b = vertex(tri[(e + 2) % 3]), &o = vertex(tri[e]);
                        float dx = b.x - a.x, dy = b.y - a.y;
                        bool xMajor = fabsf(dx) > fabsf(dy);
                        if (!outer[e] || (!dx && !dy)) {
                            dist[0][e] = dist[1][e] = dist[2][e] = 1000.0f;
                            continue;
                        }
                        float k = xMajor ? (dy / dx) : (dx / dy);
                        float c0 = xMajor ? (k * a.x - a.y) : (k * a.y - a.x);
                        auto across = [&](float x, float y) { return xMajor ? (y - k * x + c0) : (x - k * y + c0); };
                        float side = across(o.x, o.y);
                        if (!side) {
                            dist[0][e] = dist[1][e] = dist[2][e] = 1000.0f;
                            continue;
                        }
                        float sign = (side > 0) ? 1.0f : -1.0f;
                        for (int c = 0; c < 3; c++) {
                            Vertex &v = vertex(tri[c]);
                            dist[c][e] = across(v.x + 0.5f - EDGE_BIAS, v.y + 0.5f - EDGE_BIAS) * sign;
                        }
                    }
                }
                for (int c = 0; c < 3; c++) {
                    // Claim the vertex's cache blocks first, so writing it doesn't read the old contents from memory
                    GpuVertex &out = vertices[count++];
                    claimBlock(&out);
                    claimBlock(&out.color);
                    out = converted[tri[c]];
                    out.tex[2] = dist[c][0];
                    out.tex[3] = dist[c][1];
                    out.attrib[1] = dist[c][2];
                }
            }

            HB_ADD_TICKS(HB_GPU_FAN_US, hbFan);

            // List the polygon's edges, using the fan's vertices: the first triangle has the first 3 polygon vertices,
            // and the last vertex of each later one is the next polygon vertex
            if (fillCracks && draw.solid && !wireframe && edgeCount + polygon->size * 2 <= MAX_EDGE_INDICES) {
                auto fanVertex = [&](int k) { return draw.first + ((k < 2) ? k : ((k - 2) * 3 + 2)); };
                for (int k = 0; k < polygon->size; k++) {
                    edgeIndices[edgeCount++] = fanVertex(k);
                    edgeIndices[edgeCount++] = fanVertex((k + 1) % polygon->size);
                }
            }

            // Merge the triangles into the last draw if the state is the same
            draw.count = count - draw.first;
            GpuDraw *last = draws.empty() ? nullptr : &draws.back();
            if (last && last->shader == draw.shader && last->texture == draw.texture && last->sampler == draw.sampler &&
                    last->translucent == draw.translucent && last->depthEqual == draw.depthEqual &&
                    last->depthWrite == draw.depthWrite && last->stencil == draw.stencil && last->split == draw.split &&
                    last->shadow == draw.shadow && !draw.clearMask && last->lines == draw.lines &&
                    last->first + last->count == draw.first && draws.size() > sentDraws)
                last->count += draw.count;
            else
                draws.push_back(draw);
            if (chunked && count - sentVertices >= (started ? CHUNK_VERTICES : FIRST_CHUNK_VERTICES))
                sendChunk();
        }
    }
    HB_TIMER(hbPostLoop);

    bool packed = false; // Whether the finished frame has been packed as the 2D engine's 3D pixels yet
    uint32_t *copy = nullptr; // Where the high-res frame was copied for the UI, if it was
    {
        // Draw the frame while holding GX2, setting this context's state up again after clearing
        HB_ADD_US(HB_GPU_POSTLOOP_US, hbPostLoop);
        HB_ADD_US(HB_GPU_BUILD_US, hbBuild);
        GPU_STAGE(3);
        HB_TIMER(hbLock);
        guard.lock();
        HB_ADD_US(HB_GPU_LOCK_US, hbLock);
        GPU_STAGE(4);

        // Make what the CPU wrote visible to the GPU and draw the polygons, or the rest of them if some were sent
        if (!started) {
            startFrame();
            showBuilt();
            for (int pass = msaa ? 0 : 1; pass < 2; pass++) {
                if (pass == 1 && msaa && !writeAttribs) break;
                setupPass(pass);
                HB_LAP(HB_GPU_PASS_CLEAR_US);
                for (GpuDraw &draw : draws) {
                    submitDraw(draw);
#if defined(HEARTBEAT) && defined(GPU_TIMELINE)
                    // Time each draw by its shader, with shadows on their own
                    int kind = draw.shadow ? 5 : (draw.shader == 7) ? 4 : std::min<int>(draw.shader, 3);
                    hbLap(HeartbeatSlot(HB_GPU_DRAW_COLOR_US + kind));
                    HB_ADD(HeartbeatSlot(HB_GPU_VTX_COLOR + kind), draw.count);
#endif
                }

                // Average the multisampled colors into the color buffer, through a tiled surface the GPU can resolve to
                if (pass == 0) {
                    GX2ResolveAAColorBuffer(&msaaColor, &resolved, 0, 0);
                    GX2CopySurface(&resolved, 0, 0, &colorBuffer.surface, 0, 0);
                    GX2SetContextState(state);
                    HB_LAP(HB_GPU_PASS_MSAA_US);
                }
            }
        }
        else {
            GX2SetContextState(state);
            sendBuilt();
        }
        HB_ADD(HB_GPU_DRAWS, draws.size());
        HB_ADD(HB_GPU_VERTICES, count);
#if defined(HEARTBEAT) && defined(GPU_TIMELINE)
        Heartbeat::values[HB_GPU_MAIN_SUBMIT_US] += hbUs();
        hbLap(HB_GPU_PASS_POLY_US);
        Heartbeat::values[HB_GPU_MAIN_DONE_US] += hbUs();
        if (msaa) HB_INC(HB_GPU_MSAA_FRAMES);
        if (height == 384) HB_INC(HB_GPU_HIRES_FRAMES);
#endif

        if (edgeCount) {
            // Mark the pixels along solid polygons' edges in the stencil's top bit, after clearing what shadows left
            // Lines are wider than a pixel so they surely reach cracks, which the fill below still checks for
            GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, edgeIndices, edgeCount * sizeof(uint16_t));
            WHBGfxShaderGroup &mark = groups[0];
            GX2SetFetchShader(&mark.fetchShader);
            GX2SetVertexShader(mark.vertexShader);
            GX2SetPixelShader(mark.pixelShader);
            GX2SetDepthStencilControl(TRUE, FALSE, GX2_COMPARE_FUNC_ALWAYS, TRUE, FALSE,
                GX2_COMPARE_FUNC_ALWAYS, GX2_STENCIL_FUNCTION_REPLACE, GX2_STENCIL_FUNCTION_KEEP,
                GX2_STENCIL_FUNCTION_KEEP, GX2_COMPARE_FUNC_ALWAYS, GX2_STENCIL_FUNCTION_KEEP,
                GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP);
            GX2SetStencilMask(0x80, 0x80, 0, 0x80, 0x80, 0);
            GX2SetAlphaTest(FALSE, GX2_COMPARE_FUNC_ALWAYS, 0.0f);
            GX2SetTargetChannelMasks(NO_CHANNELS, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS,
                NO_CHANNELS, NO_CHANNELS);
            GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, 6, QUAD, 1);
            GX2SetStencilMask(0x80, 0x80, 0x80, 0x80, 0x80, 0x80);
            GX2SetLineWidth(2.0f);
            GX2DrawIndexedEx(GX2_PRIMITIVE_MODE_LINES, edgeCount, GX2_INDEX_TYPE_U16, edgeIndices, 0, 1);
            HB_LAP(HB_GPU_PASS_MARK_US);

            // Fill the marked pixels that are cracks from a neighbor, in both the colors and the attributes, so edge
            // marking and anti-aliasing don't treat them as edges; only cracks are written, which nothing else reads
            GX2Invalidate((GX2InvalidateMode)(GX2_INVALIDATE_MODE_COLOR_BUFFER | GX2_INVALIDATE_MODE_TEXTURE),
                colorBuffer.surface.image, colorBuffer.surface.imageSize);
            GX2Invalidate((GX2InvalidateMode)(GX2_INVALIDATE_MODE_COLOR_BUFFER | GX2_INVALIDATE_MODE_TEXTURE),
                attribTarget1.surface.image, attribTarget1.surface.imageSize);
            WHBGfxShaderGroup &group = groups[10];
            GX2SetFetchShader(&group.fetchShader);
            GX2SetVertexShader(group.vertexShader);
            GX2SetPixelShader(group.pixelShader);
            GX2SetPixelTexture(&attribRead, 0);
            GX2SetPixelSampler(&samplers[0], 0);
            GX2SetPixelTexture(&colorTexture, 1);
            GX2SetPixelSampler(&samplers[0], 1);
            // How much farther than both neighbors up and down makes a pixel a gap: W by a ratio, which adds to its log,
            // and Z by an amount
            float scale[] = { compact ? 256.0f : 1.0f, (wBuffer && !compact) ? 1.25f : 1.0f,
                !wBuffer ? (compact ? float(0x10000) / 0x1000000 : 0x10000) : (compact ? log2f(1.25f) / 32 : 0.0f), 0 };
            GX2SetPixelUniformReg(0, 4, scale);
            GX2SetDepthStencilControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS, TRUE, FALSE,
                GX2_COMPARE_FUNC_EQUAL, GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP,
                GX2_STENCIL_FUNCTION_KEEP, GX2_COMPARE_FUNC_ALWAYS, GX2_STENCIL_FUNCTION_KEEP,
                GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP);
            GX2SetStencilMask(0x80, 0, 0x80, 0x80, 0, 0x80);
            GX2SetColorControl(GX2_LOGIC_OP_COPY, 0x00, FALSE, TRUE);
            GX2SetTargetChannelMasks(GX2_CHANNEL_MASK_RGBA, GX2_CHANNEL_MASK_RGBA, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS,
                NO_CHANNELS, NO_CHANNELS, NO_CHANNELS);
            GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, 6, QUAD, 1);
            HB_LAP(HB_GPU_PASS_FILL_US);
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

            // A neighbor has to be further by a margin, since the GPU's depths for surfaces that touch can differ
            // slightly where the DS's are equal; W values are relative, while Z values crowd near the far end, so
            // those get a small fixed margin like the DS's depth test for equal values
            float margin[] = { wBuffer ? (1.0f + 1.0f / 256) : 1.0f, wBuffer ? 0.0f : 0x200, 0, 0 };
            GX2SetPixelUniformReg(0, 4, margin);
            GX2SetDepthStencilControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS, FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS,
                GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP, GX2_COMPARE_FUNC_ALWAYS,
                GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP);
            GX2SetAlphaTest(FALSE, GX2_COMPARE_FUNC_ALWAYS, 0.0f);
            GX2SetColorControl(GX2_LOGIC_OP_COPY, 0x01, FALSE, TRUE);
            GX2SetTargetChannelMasks(GX2_CHANNEL_MASK_RGBA, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS,
                NO_CHANNELS, NO_CHANNELS);
            GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, 6, QUAD, 1);
            HB_LAP(HB_GPU_PASS_EDGE_US);
        }

        if (fog) {
            // Mix in the fog color by each pixel's density, given in every channel of the output, while the blend
            // factor holds the fog color; it only changes the alpha if set to
            if (!gpuEdges)
                GX2Invalidate((GX2InvalidateMode)(GX2_INVALIDATE_MODE_COLOR_BUFFER | GX2_INVALIDATE_MODE_TEXTURE),
                    attribTarget1.surface.image, attribTarget1.surface.imageSize);
            WHBGfxShaderGroup &group = groups[5];
            GX2SetFetchShader(&group.fetchShader);
            GX2SetVertexShader(group.vertexShader);
            GX2SetPixelShader(group.pixelShader);
            GX2SetPixelTexture(&attribRead, 0);
            GX2SetPixelSampler(&samplers[0], 0);
            GX2SetPixelTexture(&fogTexture, 1);
            GX2SetPixelSampler(&samplers[0], 1);
            // Undo the attributes' encoding: the scale of the ID and flags to give a quarter of them, and how to get the
            // depth over 0x200
            float decode[] = { compact ? (wBuffer ? 0.0f : 0x1000000 / 0x200) : 1.0f / 0x200, (compact && wBuffer) ? 1.0f : 0.0f,
                32.0f, compact ? 64.0f : 0.25f };
            GX2SetPixelUniformReg(0, 4, decode);
            uint32_t fogColor = renderer->getFogColor();
            GX2SetBlendConstantColor(((fogColor >> 0) & 0x1F) / 31.0f, ((fogColor >> 5) & 0x1F) / 31.0f,
                ((fogColor >> 10) & 0x1F) / 31.0f, ((fogColor >> 16) & 0x1F) / 31.0f);
            GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_BLEND_FACTOR, GX2_BLEND_MODE_INV_SRC_COLOR,
                GX2_BLEND_COMBINE_MODE_ADD, TRUE, GX2_BLEND_MODE_BLEND_FACTOR, GX2_BLEND_MODE_INV_SRC_ALPHA,
                GX2_BLEND_COMBINE_MODE_ADD);
            GX2SetDepthStencilControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS, FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS,
                GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP, GX2_COMPARE_FUNC_ALWAYS,
                GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP);
            GX2SetAlphaTest(FALSE, GX2_COMPARE_FUNC_ALWAYS, 0.0f);
            GX2SetColorControl(GX2_LOGIC_OP_COPY, 0x01, FALSE, TRUE);
            GX2SetTargetChannelMasks((disp3DCnt & BIT(6)) ? GX2_CHANNEL_MASK_A : GX2_CHANNEL_MASK_RGBA, NO_CHANNELS,
                NO_CHANNELS, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS);
            GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, 6, QUAD, 1);
            HB_LAP(HB_GPU_PASS_FOG_US);
        }

        if (dsAntiAlias && !msaa) {
            // Blend partly covered pixels with the color behind them, after edge marking and fog like the DS, and
            // pack the results straight into the packed buffer, since neighbors are read
            GX2Invalidate((GX2InvalidateMode)(GX2_INVALIDATE_MODE_COLOR_BUFFER | GX2_INVALIDATE_MODE_TEXTURE),
                colorBuffer.surface.image, colorBuffer.surface.imageSize);
            if (!gpuEdges && !fog)
                GX2Invalidate((GX2InvalidateMode)(GX2_INVALIDATE_MODE_COLOR_BUFFER | GX2_INVALIDATE_MODE_TEXTURE),
                    attribTarget1.surface.image, attribTarget1.surface.imageSize);
            packed = true;
            GX2SetColorBuffer(&packedBuffer, GX2_RENDER_TARGET_0);
            WHBGfxShaderGroup &group = groups[6];
            GX2SetFetchShader(&group.fetchShader);
            GX2SetVertexShader(group.vertexShader);
            GX2SetPixelShader(group.pixelShader);
            GX2SetPixelTexture(&attribRead, 0);
            GX2SetPixelSampler(&samplers[0], 0);
            GX2SetPixelTexture(&colorTexture, 1);
            GX2SetPixelSampler(&samplers[0], 1);
            // Encoded depths compare the same way with the margin encoded too, which adds to the log of W
            float margin[] = { wBuffer ? (1.0f + 1.0f / 256) : 1.0f, wBuffer ? 0.0f : 0x200, 0, 0, compact ? 256.0f : 1.0f, 0, 0, 0 };
            if (compact) {
                margin[0] = 1.0f;
                margin[1] = wBuffer ? (log2f(1.0f + 1.0f / 256) / 32) : (float(0x200) / 0x1000000);
            }
            GX2SetPixelUniformReg(0, 8, margin);
            GX2SetDepthStencilControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS, FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS,
                GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP, GX2_COMPARE_FUNC_ALWAYS,
                GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP);
            GX2SetAlphaTest(FALSE, GX2_COMPARE_FUNC_ALWAYS, 0.0f);
            GX2SetColorControl(GX2_LOGIC_OP_COPY, 0x00, FALSE, TRUE);
            GX2SetTargetChannelMasks(GX2_CHANNEL_MASK_RGBA, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS,
                NO_CHANNELS, NO_CHANNELS);
            GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, 6, QUAD, 1);
            HB_LAP(HB_GPU_PASS_AA_US);
        }

        // Pack the finished colors as the 2D engine's 3D pixels, so the CPU only has to copy them
        if (!packed) {
            GX2Invalidate((GX2InvalidateMode)(GX2_INVALIDATE_MODE_COLOR_BUFFER | GX2_INVALIDATE_MODE_TEXTURE),
                colorBuffer.surface.image, colorBuffer.surface.imageSize);
            GX2SetColorBuffer(&packedBuffer, GX2_RENDER_TARGET_0);
            WHBGfxShaderGroup &group = groups[8];
            GX2SetFetchShader(&group.fetchShader);
            GX2SetVertexShader(group.vertexShader);
            GX2SetPixelShader(group.pixelShader);
            GX2SetPixelTexture(&colorTexture, 0);
            GX2SetPixelSampler(&samplers[0], 0);
            GX2SetDepthStencilControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS, FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS,
                GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP, GX2_COMPARE_FUNC_ALWAYS,
                GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP);
            GX2SetAlphaTest(FALSE, GX2_COMPARE_FUNC_ALWAYS, 0.0f);
            GX2SetColorControl(GX2_LOGIC_OP_COPY, 0x00, FALSE, TRUE);
            GX2SetTargetChannelMasks(GX2_CHANNEL_MASK_RGBA, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS, NO_CHANNELS,
                NO_CHANNELS, NO_CHANNELS);
            GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, 6, QUAD, 1);
        }

        // Write out the packed frame, then let the UI submit while the GPU finishes this frame
        GPU_STAGE(6);
        GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, packedBuffer.surface.image, packedBuffer.surface.imageSize);

        // Copy high-res frames for the UI too, which is much faster on the GPU than reading them with the CPU
        copy = renderer->getHardwareCopy();
        if (copy && height == 384 && !((uintptr_t)copy & (packedBuffer.surface.alignment - 1))) {
            GX2Surface surface = packedBuffer.surface;
            surface.image = copy;
            DCFlushRange(copy, surface.imageSize);
            GX2CopySurface(&packedBuffer.surface, 0, 0, &surface, 0, 0);
            GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, copy, surface.imageSize);

            // Copying sets up GPU state of its own, so set this context's state again, like after resolving
            GX2SetContextState(state);
        }
        else {
            copy = nullptr;
        }
        GX2Flush();
        OSTime submitted = GX2GetLastSubmittedTimeStamp();
#if defined(HEARTBEAT) && defined(GPU_TIMELINE)
        Heartbeat::values[HB_GPU_SUBMIT_US] += hbUs();
        hbLap(HB_GPU_PASS_PACK_US);
#endif
        guard.unlock();
        // Wait for the GPU, but show an error screen if it stops responding, which would otherwise freeze silently
        GPU_STAGE(7);
        OSTime start = OSGetSystemTime();
        while (GX2GetRetiredTimeStamp() < submitted) {
            if (OSTicksToMilliseconds(OSGetSystemTime() - start) > 2000) {
                GPU_STAGE(8);
#ifdef HEARTBEAT
                // Heartbeat builds keep waiting instead of stopping the system, so the log shows the GPU stuck
                OSSleepTicks(OSMillisecondsToTicks(10));
                continue;
#endif
                // Display the timeout without depending on C runtime locks or working file access
                char message[256];
                __os_snprintf(message, sizeof(message), "The GPU stopped responding to the GPU 3D renderer\n"
                    "Frame %u: %u draws, %u vertices, 3D display control %04X, anti-aliasing setting %d",
                    frameCount, (uint32_t)draws.size(), count, disp3DCnt, Settings::gpuAntiAlias);
                // Write it to the hang report first, with the frame's draws, since the error screen might not show
                // with the GPU stuck
                if (FILE *file = openHangReport(message)) {
                    fprintf(file, "multisample %d, edges %d, fog %d, DS anti-aliasing %d, attributes %d, compact %d, "
                        "high-res %d, overlapped %d, alpha cutoff %f\n", msaa, gpuEdges, fog, dsAntiAlias, writeAttribs,
                        compact, shift, chunked, alphaMin);
                    for (GpuDraw &draw : draws)
                        fprintf(file, "shader %d texture %ux%u sampler %d translucent %d depthEqual %d depthWrite %d solid %d "
                            "stencil %02X shadow %d clearMask %d split %d first %u count %u\n", draw.shader,
                            draw.texture ? draw.texture->surface.width : 0, draw.texture ? draw.texture->surface.height : 0,
                            draw.sampler, draw.translucent, draw.depthEqual, draw.depthWrite, draw.solid, draw.stencil,
                            draw.shadow, draw.clearMask, draw.split, draw.first, draw.count);
                    fclose(file);
                }
                OSFatal(message);
            }
            OSSleepTicks(OSMicrosecondsToTicks(100));
        }
        HB_ADD_US(HB_GPU_DRAW_US, hbDraw);
#ifdef FRAME_CAPTURE
        FrameCapture::renders[renderer->captureId % FrameCapture::RENDERS].endTime = OSGetSystemTime();
#endif
    }

#ifdef GPU_DUMP
    // Debug builds also draw dumped frames in software, before marking any scanline done so the frame can't change
    std::vector<uint32_t> software;
    if (frameCount % 15 == 0) {
        software.resize(256 * 192);
        renderer->softwareFrame(software.data());
    }
#endif

    // The scanlines are already packed in the renderer's format, so let the 2D engine read them where they are,
    // since copying them was slow with every read missing the cache; they're only drawn over by the next frame,
    // which starts after the 2D engine is done with this one
    GPU_STAGE(5);
    HB_TIMER(hbConvert);
    DCInvalidateRange(packedBuffer.surface.image, packedBuffer.surface.imageSize);
    if (copy) {
        DCInvalidateRange(copy, packedBuffer.surface.imageSize);
        renderer->hardwareCopyDone();
    }
    bool direct = (packedBuffer.surface.pitch == 512);
    renderer->setHardwareBuffer(direct ? (uint32_t*)packedBuffer.surface.image : nullptr);
    for (int y = 0; y < height; y++) {
        if (uint32_t *line = direct ? nullptr : renderer->hardwareLine(y))
            memcpy(line, (uint32_t*)packedBuffer.surface.image + y * packedBuffer.surface.pitch, width * sizeof(uint32_t));
        renderer->hardwareDone(y);
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
                if (!draw.solid || draw.shadow) continue;
                GX2CompareFunction func = wBuffer ? (draw.depthEqual ? GX2_COMPARE_FUNC_GEQUAL : GX2_COMPARE_FUNC_GREATER) :
                    (draw.depthEqual ? GX2_COMPARE_FUNC_LEQUAL : GX2_COMPARE_FUNC_LESS);
                GX2SetDepthOnlyControl(TRUE, draw.depthWrite, func);
                GX2DrawEx(draw.lines ? GX2_PRIMITIVE_MODE_LINES : GX2_PRIMITIVE_MODE_TRIANGLES, draw.count, draw.first, 1);
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
                fwrite((uint32_t*)packedBuffer.surface.image + y * packedBuffer.surface.pitch, 4, 256, file);
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

