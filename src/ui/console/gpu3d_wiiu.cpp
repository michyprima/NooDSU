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

extern const uint8_t gpu3d_color_gsh[], gpu3d_modulate_gsh[], gpu3d_decal_gsh[];

std::mutex gx2Mutex;

namespace {

// The vertex layout the shaders read: clip-space position, texture coordinates and color
struct GpuVertex {
    float pos[4];
    float tex[2];
    float color[4];
};

// A run of triangles drawn with the same state
struct GpuDraw {
    int shader; // 0 = vertex color, 1 = modulate, 2 = decal
    GX2Texture *texture; // Null for untextured polygons
    int sampler;
    bool translucent;
    bool depthEqual;
    bool depthWrite;
    uint32_t first, count;
};

class Gpu3DWiiU: public Gpu3DHardware {
public:
    bool init();
    void render(Gpu3DRenderer *renderer) override;

private:
    static const int MAX_VERTICES = 2048 * 8 * 3;

    WHBGfxShaderGroup groups[3];
    GX2Sampler samplers[9];
    GX2ContextState *state = nullptr;
    GX2ColorBuffer colorBuffer;
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
    const uint8_t *shaders[] = { gpu3d_color_gsh, gpu3d_modulate_gsh, gpu3d_decal_gsh };
    for (int i = 0; i < 3; i++) {
        if (!WHBGfxLoadGFDShaderGroup(&groups[i], 0, shaders[i])) return false;
        WHBGfxInitShaderAttribute(&groups[i], "position", 0, offsetof(GpuVertex, pos), GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32);
        WHBGfxInitShaderAttribute(&groups[i], "tex_coords", 0, offsetof(GpuVertex, tex), GX2_ATTRIB_FORMAT_FLOAT_32_32);
        WHBGfxInitShaderAttribute(&groups[i], "vtx_color", 0, offsetof(GpuVertex, color), GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32);
        if (!WHBGfxInitFetchShader(&groups[i])) return false;
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

    memset(&depthBuffer, 0, sizeof(depthBuffer));
    depthBuffer.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    depthBuffer.surface.width = 256;
    depthBuffer.surface.height = 192;
    depthBuffer.surface.depth = 1;
    depthBuffer.surface.mipLevels = 1;
    depthBuffer.surface.format = GX2_SURFACE_FORMAT_FLOAT_D24_S8;
    depthBuffer.surface.aa = GX2_AA_MODE1X;
    depthBuffer.surface.use = GX2_SURFACE_USE_DEPTH_BUFFER;
    depthBuffer.surface.tileMode = GX2_TILE_MODE_DEFAULT;
    depthBuffer.viewNumSlices = 1;
    depthBuffer.depthClear = 1.0f;
    GX2CalcSurfaceSizeAndAlignment(&depthBuffer.surface);
    GX2InitDepthBufferRegs(&depthBuffer);
    depthBuffer.surface.image = MEMAllocFromDefaultHeapEx(depthBuffer.surface.imageSize, depthBuffer.surface.alignment);

    // Allocate the vertex buffer and a context state of its own, so the UI's state isn't disturbed
    vertices = (GpuVertex*)MEMAllocFromDefaultHeapEx(MAX_VERTICES * sizeof(GpuVertex), GX2_VERTEX_BUFFER_ALIGNMENT);
    state = (GX2ContextState*)MEMAllocFromDefaultHeapEx(sizeof(GX2ContextState), GX2_CONTEXT_STATE_ALIGNMENT);
    if (!colorBuffer.surface.image || !depthBuffer.surface.image || !vertices || !state) return false;
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
    while (busy.load()) std::this_thread::yield();
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
    frameCount++;
    freeTextures();
    updated.clear();
    draws.clear();

    // Build the triangles, with solid polygons first and then translucent ones, in order like the software renderer
    // W-buffered depth is 1/W, which interpolates linearly across the screen like Z, so the GPU compares it exactly;
    // it's reversed so precision is best up close, and compared with greater instead of less
    uint32_t count = 0;
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
            draw.texture = textured ? getTexture(renderer, polygon) : nullptr;
            draw.shader = !draw.texture ? 0 : (polygon->mode == 1) ? 2 : 1;
            draw.sampler = (polygon->repeatS ? (polygon->flipS ? 2 : 1) : 0) + (polygon->repeatT ? (polygon->flipT ? 2 : 1) : 0) * 3;
            draw.translucent = translucent && blending;
            draw.depthEqual = polygon->depthTestEqual;
            draw.depthWrite = !translucent || polygon->transNewDepth;
            draw.first = count;

            // Add the polygon as a fan of triangles
            float scaleS = textured ? 1.0f / (polygon->sizeS * 16) : 0, scaleT = textured ? 1.0f / (polygon->sizeT * 16) : 0;
            float alpha = polygon->alpha / 63.0f;
            bool toon = (polygon->mode == 2 && !(disp3DCnt & BIT(1)));
            // Unclipped quad strip polygons have their last 2 vertices swapped, so they're swapped back
            for (int j = 1; j + 1 < polygon->size; j++) {
                int corners[] = { 0, j, j + 1 };
                for (int k : corners) {
                    if (polygon->crossed && k >= 2) k ^= 1;
                    Vertex &v = verts[polygon->vertices + k];
                    GpuVertex &out = vertices[count++];
                    float w = (v.w > 0) ? float(v.w) : 1.0f;
                    float depth = wBuffer ? (1.0f / w) : (float(v.z) / 0x1000000);
                    out.pos[0] = (float(v.x) / 128 - 1) * w;
                    out.pos[1] = (1 - float(v.y) / 96) * w;
                    out.pos[2] = depth * w;
                    out.pos[3] = w;
                    out.tex[0] = v.s * scaleS;
                    out.tex[1] = v.t * scaleT;
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
                }
            }

            // Merge the triangles into the last draw if the state is the same
            draw.count = count - draw.first;
            GpuDraw *last = draws.empty() ? nullptr : &draws.back();
            if (last && last->shader == draw.shader && last->texture == draw.texture && last->sampler == draw.sampler &&
                    last->translucent == draw.translucent && last->depthEqual == draw.depthEqual &&
                    last->depthWrite == draw.depthWrite && last->first + last->count == draw.first)
                last->count += draw.count;
            else
                draws.push_back(draw);
        }
    }
    // Get the clear values; the clear color is 5-bit with a 5-bit alpha
    uint32_t clear = renderer->getClearColor();
    float clearR = ((clear >> 0) & 0x1F) / 31.0f, clearG = ((clear >> 5) & 0x1F) / 31.0f;
    float clearB = ((clear >> 10) & 0x1F) / 31.0f, clearA = ((clear >> 16) & 0x1F) / 31.0f;
    uint16_t clearDepth = renderer->getClearDepth();
    float depthClear = wBuffer ? 0.0f : (clearDepth == 0x7FFF) ? 1.0f : float(clearDepth << 9) / 0x1000000;

    {
        // Draw the frame while holding GX2, setting this context's state up again after clearing
        HB_SET(HB_GPU_STAGE, 3);
        std::lock_guard<std::mutex> guard(gx2Mutex);
        HB_SET(HB_GPU_STAGE, 4);

        // Make what the CPU wrote visible to the GPU; this adds GPU commands, so it has to be done with GX2 held
        for (GX2Texture *texture : updated)
            GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, texture->surface.image, texture->surface.imageSize);
        GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, vertices, count * sizeof(GpuVertex));
        GX2SetContextState(state);
        GX2ClearColor(&colorBuffer, clearR, clearG, clearB, clearA);
        GX2ClearDepthStencilEx(&depthBuffer, depthClear, 0, GX2_CLEAR_FLAGS_BOTH);
        GX2SetContextState(state);
        GX2SetColorBuffer(&colorBuffer, GX2_RENDER_TARGET_0);
        GX2SetDepthBuffer(&depthBuffer);
        GX2SetViewport(0, 0, 256, 192, 0, 1);
        GX2SetScissor(0, 0, 256, 192);
        GX2SetRasterizerClipControlHalfZ(TRUE, FALSE, TRUE);
        GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, FALSE);
        GX2SetAlphaTest(TRUE, GX2_COMPARE_FUNC_GREATER, 0.0f);
        GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_INV_SRC_ALPHA,
            GX2_BLEND_COMBINE_MODE_ADD, TRUE, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_ONE, GX2_BLEND_COMBINE_MODE_MAX);
        GX2SetAttribBuffer(0, count * sizeof(GpuVertex), sizeof(GpuVertex), vertices);

        for (GpuDraw &draw : draws) {
            WHBGfxShaderGroup &group = groups[draw.shader];
            GX2SetFetchShader(&group.fetchShader);
            GX2SetVertexShader(group.vertexShader);
            GX2SetPixelShader(group.pixelShader);
            if (draw.texture) {
                GX2SetPixelTexture(draw.texture, 0);
                GX2SetPixelSampler(&samplers[draw.sampler], 0);
            }
            GX2CompareFunction func = wBuffer ? (draw.depthEqual ? GX2_COMPARE_FUNC_GEQUAL : GX2_COMPARE_FUNC_GREATER) :
                (draw.depthEqual ? GX2_COMPARE_FUNC_LEQUAL : GX2_COMPARE_FUNC_LESS);
            GX2SetDepthOnlyControl(TRUE, draw.depthWrite, func);
            GX2SetColorControl(GX2_LOGIC_OP_COPY, draw.translucent ? 0x01 : 0x00, FALSE, TRUE);
            GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, draw.count, draw.first, 1);
        }

        // Write the color buffer out to memory and wait for the GPU to finish
        GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, colorBuffer.surface.image, colorBuffer.surface.imageSize);
        GX2DrawDone();
    }

    // Convert each scanline to the renderer's format, with 6-bit channels and the 3D pixel bit, and mark it done
    HB_SET(HB_GPU_STAGE, 5);
    DCInvalidateRange(colorBuffer.surface.image, colorBuffer.surface.imageSize);
    for (int y = 0; y < 192; y++) {
        const uint32_t *src = (uint32_t*)colorBuffer.surface.image + y * colorBuffer.surface.pitch;
        uint32_t *dst = renderer->hardwareLine(y);
        for (int x = 0; x < 256; x++) {
            uint32_t p = src[x];
            dst[x] = BIT(26) | (((p >> 2) & 0x3F) << 18) | (((p >> 10) & 0x3F) << 12) | (((p >> 18) & 0x3F) << 6) | ((p >> 26) & 0x3F);
        }
        renderer->hardwareDone(y);
    }
}

} // namespace

void initGpu3D() {
    // Make the GPU renderer available to the core if it could be set up
    Gpu3DWiiU *gpu3D = new Gpu3DWiiU();
    if (gpu3D->init())
        Gpu3DRenderer::hardware = gpu3D;
}

#endif // __WIIU__
