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

#include <coreinit/foreground.h>
#include <coreinit/memdefaultheap.h>
#include <gx2/display.h>
#include <gx2/draw.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2r/draw.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <padscore/kpad.h>
#include <padscore/wpad.h>
#include <proc_ui/procui.h>
#include <sysapp/launch.h>
#include <vpad/input.h>
#include <whb/sdcard.h>
#include <whb/gfx.h>
#include <SDL2/SDL.h>
#include <atomic>
#include <condition_variable>
#include <mutex>

#include "console_ui.h"
#include "../core/settings.h"
#include "../core/worker_thread.h"
#include "../core/heartbeat.h"

#define MAX_DRAWS 1024
#define NO_INVALIDATE GX2RResourceFlags(GX2R_RESOURCE_DISABLE_CPU_INVALIDATE | GX2R_RESOURCE_DISABLE_GPU_INVALIDATE)
extern const uint8_t shader_wiiu_gsh[];

ScreenLayout gpLayout;
GX2Texture *gpTexture;
int tvWidth, tvHeight;
int bufOffset;
bool firstScreen;

VPADStatus vpad;
VPADTouchData touch;
uint32_t extHeld[4];
bool scanned;

WHBGfxShaderGroup group;
GX2RBuffer posBuffer, texBuffer, colBuffer;
GX2Sampler samplers[2];

// State shared between the main thread, which handles system messages, and the UI thread
// When the system asks for the foreground, the UI thread stops between frames until it gets it back
std::mutex procMutex;
std::condition_variable procCond;
std::atomic<bool> releaseRequested;
bool uiParked, uiDone;

uint32_t ConsoleUI::defaultKeys[] {
    VPAD_BUTTON_A, VPAD_BUTTON_B, VPAD_BUTTON_MINUS, VPAD_BUTTON_PLUS,
    VPAD_BUTTON_RIGHT | VPAD_STICK_R_EMULATION_RIGHT | VPAD_STICK_L_EMULATION_RIGHT,
    VPAD_BUTTON_LEFT | VPAD_STICK_R_EMULATION_LEFT | VPAD_STICK_L_EMULATION_LEFT,
    VPAD_BUTTON_UP | VPAD_STICK_R_EMULATION_UP | VPAD_STICK_L_EMULATION_UP,
    VPAD_BUTTON_DOWN | VPAD_STICK_R_EMULATION_DOWN | VPAD_STICK_L_EMULATION_DOWN,
    VPAD_BUTTON_ZR, VPAD_BUTTON_ZL, VPAD_BUTTON_X, VPAD_BUTTON_Y,
    VPAD_BUTTON_L | VPAD_BUTTON_R
};

const char *ConsoleUI::keyNames[] {
    "Sync", "Home", "Minus", "Plus", "R", "L", "ZR", "ZL",
    "Down", "Up", "Right", "Left", "Y", "X", "B", "A",
    "TV", "R Stick", "L Stick", "", "", "", "",
    "RS Down", "RS Up", "RS Right", "RS Left",
    "LS Down", "LS Up", "LS Right", "LS Left"
};

void releaseForeground() {
    // Stop the emulator, since threads on cores 0 and 1 are suspended in the background
    bool resume = ConsoleUI::running;
    ConsoleUI::stopCore();

    // Let the main thread release the foreground, and wait until it's back
    std::unique_lock<std::mutex> lock(procMutex);
    uiParked = true;
    procCond.notify_all();
    procCond.wait(lock, [] { return !releaseRequested; });
    uiParked = false;
    lock.unlock();
    if (resume) ConsoleUI::startCore();
}

void ConsoleUI::startFrame(uint32_t color) {
    // Stop drawing while the system needs the foreground, like for the HOME menu
    if (releaseRequested)
        releaseForeground();

    // Convert the clear color to floats
    float r = float(color & 0xFF) / 0xFF;
    float g = float((color >> 8) & 0xFF) / 0xFF;
    float b = float((color >> 16) & 0xFF) / 0xFF;
    float a = float(color >> 24) / 0xFF;

    // Clear the TV and gamepad screens
    WHBGfxBeginRender();
    WHBGfxBeginRenderTV();
    WHBGfxClearColor(r, g, b, a);
    WHBGfxBeginRenderDRC();
    WHBGfxClearColor(r, g, b, a);

}

void ConsoleUI::endFrame() {
    // Finish and display a frame
    WHBGfxFinishRenderTV();
    WHBGfxFinishRenderDRC();
    WHBGfxFinishRender();

    // Free the gamepad screen texture if it was overridden
    if (gpTexture) {
        destroyTexture(gpTexture);
        gpTexture = nullptr;
    }

    // Reset the frame status
    bufOffset = 0;
    firstScreen = true;
    scanned = false;
}

void *ConsoleUI::createTexture(uint32_t *data, int width, int height) {
    // Create a new texture with the given dimensions
    GX2Texture *texture = new GX2Texture;
    memset(texture, 0, sizeof(GX2Texture));
    texture->surface.width = width;
    texture->surface.height = height;
    texture->surface.depth = 1;
    texture->surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    texture->surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    texture->surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    texture->viewNumSlices = 1;
    texture->compMap = 0x03020100;
    GX2CalcSurfaceSizeAndAlignment(&texture->surface);
    GX2InitTextureRegs(texture);
    texture->surface.image = MEMAllocFromDefaultHeapEx(texture->surface.imageSize, texture->surface.alignment);

    // Copy data to the texture
    uint32_t *dst = (uint32_t*)texture->surface.image;
    for (int y = 0; y < height; y++) {
        memcpy(dst, &data[y * width], width * sizeof(uint32_t));
        dst += texture->surface.pitch;
    }
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, texture->surface.image, texture->surface.imageSize);
    return texture;
}

void ConsoleUI::destroyTexture(void *texture) {
    // Clean up a texture
    MEMFreeToDefaultHeap(((GX2Texture*)texture)->surface.image);
    delete (GX2Texture*)texture;
}

void ConsoleUI::drawTexture(void *texture, float tx, float ty, float tw, float th,
    float x, float y, float w, float h, bool filter, int rotation, uint32_t color) {
    // Convert position values to floats for the TV
    float x1 = (x / (tvWidth / 2) - 1.0f);
    float y1 = -(y / (tvHeight / 2) - 1.0f);
    float x2 = ((x + w) / (tvWidth / 2) - 1.0f);
    float y2 = -((y + h) / (tvHeight / 2) - 1.0f);

    // Copy positions for the gamepad, but ensure lines are at least one pixel thick
    float x1a = x1, y1a = y1, x2a = x2;
    float y2a = (y1 - y2 < 1.0f / 240) ? (y1 - 1.0f / 240) : y2;

    // Detect and override gamepad screen layout positions
    if (running && tw >= 240) {
        if (firstScreen) {
            x1a = (float(gpLayout.topX) / 427 - 1.0f);
            y1a = -(float(gpLayout.topY) / 240 - 1.0f);
            x2a = (float(gpLayout.topX + gpLayout.topWidth) / 427 - 1.0f);
            y2a = -(float(gpLayout.topY + gpLayout.topHeight) / 240 - 1.0f);
            firstScreen = false;
        }
        else {
            x1a = (float(gpLayout.botX) / 427 - 1.0f);
            y1a = -(float(gpLayout.botY) / 240 - 1.0f);
            x2a = (float(gpLayout.botX + gpLayout.botWidth) / 427 - 1.0f);
            y2a = -(float(gpLayout.botY + gpLayout.botHeight) / 240 - 1.0f);
        }
    }

    // Convert texture coordinates to floats
    float s1 = tx / ((GX2Texture*)texture)->surface.width;
    float t1 = ty / ((GX2Texture*)texture)->surface.height;
    float s2 = (tx + tw) / ((GX2Texture*)texture)->surface.width;
    float t2 = (ty + th) / ((GX2Texture*)texture)->surface.height;

    // Convert the surface color to floats
    float r = float((color >> 0) & 0xFF) / 0xFF;
    float g = float((color >> 8) & 0xFF) / 0xFF;
    float b = float((color >> 16) & 0xFF) / 0xFF;
    float a = float((color >> 24) & 0xFF) / 0xFF;

    // Define arrays for position and color values
    float posCoords[] = { x1, y1, x2, y1, x2, y2, x1, y2, x1a, y1a, x2a, y1a, x2a, y2a, x1a, y2a };
    float vtxColors[] = { r, g, b, a, r, g, b, a, r, g, b, a, r, g, b, a };

    // Define texture coordinates based on rotation
    float texCoords[] = {
        s1, t1, s2, t1, s2, t2, s1, t2, // None
        s1, t2, s1, t1, s2, t1, s2, t2, // Clockwise
        s2, t1, s2, t2, s1, t2, s1, t1, // Counter-clockwise
    };

    // Upload positions to the buffer
    // Only the written range is flushed, since locking and unlocking normally flush the whole buffer
    uint8_t *buffer = (uint8_t*)GX2RLockBufferEx(&posBuffer, NO_INVALIDATE);
    memcpy(&buffer[bufOffset * 2], posCoords, posBuffer.elemSize * 8);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, &buffer[bufOffset * 2], posBuffer.elemSize * 8);
    GX2RUnlockBufferEx(&posBuffer, NO_INVALIDATE);

    // Upload texture coordinates to the buffer
    buffer = (uint8_t*)GX2RLockBufferEx(&texBuffer, NO_INVALIDATE);
    memcpy(&buffer[bufOffset], &texCoords[rotation * 8], texBuffer.elemSize * 4);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, &buffer[bufOffset], texBuffer.elemSize * 4);
    GX2RUnlockBufferEx(&texBuffer, NO_INVALIDATE);

    // Upload vertex colors to the buffer
    buffer = (uint8_t*)GX2RLockBufferEx(&colBuffer, NO_INVALIDATE);
    memcpy(&buffer[bufOffset * 2], vtxColors, colBuffer.elemSize * 4);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, &buffer[bufOffset * 2], colBuffer.elemSize * 4);
    GX2RUnlockBufferEx(&colBuffer, NO_INVALIDATE);

    // Draw a texture on the TV
    WHBGfxBeginRenderTV();
    GX2RSetAttributeBuffer(&posBuffer, 0, posBuffer.elemSize, bufOffset * 2);
    GX2RSetAttributeBuffer(&texBuffer, 1, texBuffer.elemSize, bufOffset);
    GX2RSetAttributeBuffer(&colBuffer, 2, colBuffer.elemSize, bufOffset * 2);
    GX2SetPixelTexture((GX2Texture*)texture, group.pixelShader->samplerVars[0].location);
    GX2SetPixelSampler(&samplers[filter], group.pixelShader->samplerVars[0].location);
    GX2DrawEx(GX2_PRIMITIVE_MODE_QUADS, 4, 0, 1);

    // Override the gamepad screen texture with the other screen in single screen mode
    GX2Texture *tempTexture = nullptr;
    if (running && tw >= 240 && !(ConsoleUI::gbaMode && ScreenLayout::gbaCrop) && ScreenLayout::screenArrangement == 3) {
        bool shift = (Settings::highRes3D || Settings::screenFilter == 1);
        uint32_t *data = &ConsoleUI::framebuffer[(256 * 192 * (ScreenLayout::screenSizing < 2)) << (shift * 2)];
        tempTexture = gpTexture = (GX2Texture*)createTexture(data, 256 << shift, 192 << shift);
    }

    // Draw a texture on the gamepad
    WHBGfxBeginRenderDRC();
    GX2RSetAttributeBuffer(&posBuffer, 0, posBuffer.elemSize, bufOffset * 2 + 8 * sizeof(float));
    GX2RSetAttributeBuffer(&texBuffer, 1, texBuffer.elemSize, bufOffset);
    GX2RSetAttributeBuffer(&colBuffer, 2, colBuffer.elemSize, bufOffset * 2);
    GX2SetPixelTexture(tempTexture ? tempTexture : (GX2Texture*)texture, group.pixelShader->samplerVars[0].location);
    GX2SetPixelSampler(&samplers[filter], group.pixelShader->samplerVars[0].location);
    GX2DrawEx(GX2_PRIMITIVE_MODE_QUADS, 4, 0, 1);

    // Adjust the buffer offset if there's still room
    if (bufOffset < 8 * sizeof(float) * MAX_DRAWS)
        bufOffset += 8 * sizeof(float);
}

uint32_t extToVpad(uint32_t hold, bool pro) {
    // Convert Pro Controller or Classic Controller buttons to GamePad buttons, so key binds apply to both
    // The face buttons, shoulders, d-pad, plus, minus and home use the same bits on both controllers
    static const uint32_t buttons[][2] = {
        { WPAD_PRO_BUTTON_A, VPAD_BUTTON_A }, { WPAD_PRO_BUTTON_B, VPAD_BUTTON_B },
        { WPAD_PRO_BUTTON_X, VPAD_BUTTON_X }, { WPAD_PRO_BUTTON_Y, VPAD_BUTTON_Y },
        { WPAD_PRO_BUTTON_L, VPAD_BUTTON_L }, { WPAD_PRO_BUTTON_R, VPAD_BUTTON_R },
        { WPAD_PRO_BUTTON_ZL, VPAD_BUTTON_ZL }, { WPAD_PRO_BUTTON_ZR, VPAD_BUTTON_ZR },
        { WPAD_PRO_BUTTON_PLUS, VPAD_BUTTON_PLUS }, { WPAD_PRO_BUTTON_MINUS, VPAD_BUTTON_MINUS },
        { WPAD_PRO_BUTTON_HOME, VPAD_BUTTON_HOME }, { WPAD_PRO_BUTTON_UP, VPAD_BUTTON_UP },
        { WPAD_PRO_BUTTON_DOWN, VPAD_BUTTON_DOWN }, { WPAD_PRO_BUTTON_LEFT, VPAD_BUTTON_LEFT },
        { WPAD_PRO_BUTTON_RIGHT, VPAD_BUTTON_RIGHT }
    };
    static const uint32_t proSticks[][2] = {
        { WPAD_PRO_BUTTON_STICK_L, VPAD_BUTTON_STICK_L }, { WPAD_PRO_BUTTON_STICK_R, VPAD_BUTTON_STICK_R },
        { WPAD_PRO_STICK_L_EMULATION_UP, VPAD_STICK_L_EMULATION_UP },
        { WPAD_PRO_STICK_L_EMULATION_DOWN, VPAD_STICK_L_EMULATION_DOWN },
        { WPAD_PRO_STICK_L_EMULATION_LEFT, VPAD_STICK_L_EMULATION_LEFT },
        { WPAD_PRO_STICK_L_EMULATION_RIGHT, VPAD_STICK_L_EMULATION_RIGHT },
        { WPAD_PRO_STICK_R_EMULATION_UP, VPAD_STICK_R_EMULATION_UP },
        { WPAD_PRO_STICK_R_EMULATION_DOWN, VPAD_STICK_R_EMULATION_DOWN },
        { WPAD_PRO_STICK_R_EMULATION_LEFT, VPAD_STICK_R_EMULATION_LEFT },
        { WPAD_PRO_STICK_R_EMULATION_RIGHT, VPAD_STICK_R_EMULATION_RIGHT }
    };
    static const uint32_t classicSticks[][2] = {
        { WPAD_CLASSIC_STICK_L_EMULATION_UP, VPAD_STICK_L_EMULATION_UP },
        { WPAD_CLASSIC_STICK_L_EMULATION_DOWN, VPAD_STICK_L_EMULATION_DOWN },
        { WPAD_CLASSIC_STICK_L_EMULATION_LEFT, VPAD_STICK_L_EMULATION_LEFT },
        { WPAD_CLASSIC_STICK_L_EMULATION_RIGHT, VPAD_STICK_L_EMULATION_RIGHT },
        { WPAD_CLASSIC_STICK_R_EMULATION_UP, VPAD_STICK_R_EMULATION_UP },
        { WPAD_CLASSIC_STICK_R_EMULATION_DOWN, VPAD_STICK_R_EMULATION_DOWN },
        { WPAD_CLASSIC_STICK_R_EMULATION_LEFT, VPAD_STICK_R_EMULATION_LEFT },
        { WPAD_CLASSIC_STICK_R_EMULATION_RIGHT, VPAD_STICK_R_EMULATION_RIGHT }
    };

    uint32_t result = 0;
    for (auto &b : buttons)
        if (hold & b[0]) result |= b[1];
    if (pro) {
        for (auto &b : proSticks)
            if (hold & b[0]) result |= b[1];
    }
    else {
        for (auto &b : classicSticks)
            if (hold & b[0]) result |= b[1];
    }
    return result;
}

void scanInput() {
    // Scan for input once per frame
    if (scanned) return;
    scanned = true;
    VPADRead(VPAD_CHAN_0, &vpad, 1, nullptr);
    VPADGetTPCalibratedPoint(VPAD_CHAN_0, &touch, &vpad.tpNormal);

    // Read Pro Controllers and Classic Controllers on every channel
    // Channels without new data keep their last state, and disconnected ones are cleared
    for (int i = 0; i < 4; i++) {
        KPADStatus status;
        KPADError error;
        if (!KPADReadEx((KPADChan)i, &status, 1, &error)) {
            if (error == KPAD_ERROR_INVALID_CONTROLLER)
                extHeld[i] = 0;
            continue;
        }
        if (status.extensionType == WPAD_EXT_PRO_CONTROLLER)
            extHeld[i] = extToVpad(status.pro.hold, true);
        else if (status.extensionType == WPAD_EXT_CLASSIC || status.extensionType == WPAD_EXT_MPLUS_CLASSIC)
            extHeld[i] = extToVpad(status.classic.hold, false);
        else
            extHeld[i] = 0;
    }
}

uint32_t ConsoleUI::getInputHeld() {
    // Return a mask of mappable keys from the GamePad and any other controllers
    scanInput();
    uint32_t held = vpad.hold;
    for (int i = 0; i < 4; i++)
        held |= extHeld[i];
    return held & 0x7FFFFFFF;
}

MenuTouch ConsoleUI::getInputTouch() {
    // Return touch data from the GamePad
    scanInput();
    return MenuTouch(touch.touched, touch.x, touch.y);
}

void outputAudio(void *data, uint8_t *buffer, int length) {
    // Refill the audio buffer as a callback
    ConsoleUI::fillAudioBuffer((uint32_t*)buffer, length / sizeof(uint32_t), 32768);
}

int runUi(int argc, const char **argv) {
    // Initialize various things
    WHBGfxInit();
    VPADInit();
    KPADInit();
    WPADEnableURCC(TRUE);
    WHBMountSdCard();
    gpLayout.update(854, 480, false);

    // Get the current TV render dimensions
    switch (GX2GetSystemTVScanMode()) {
    case GX2_TV_SCAN_MODE_480I:
    case GX2_TV_SCAN_MODE_480P:
        tvWidth = 854;
        tvHeight = 480;
        break;

    case GX2_TV_SCAN_MODE_1080I:
    case GX2_TV_SCAN_MODE_1080P:
        tvWidth = 1920;
        tvHeight = 1080;
        break;

    default:
        tvWidth = 1280;
        tvHeight = 720;
        break;
    }

    // Initialize the shader
    WHBGfxLoadGFDShaderGroup(&group, 0, shader_wiiu_gsh);
    WHBGfxInitShaderAttribute(&group, "position", 0, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32);
    WHBGfxInitShaderAttribute(&group, "tex_coords", 1, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32);
    WHBGfxInitShaderAttribute(&group, "vtx_color", 2, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32);
    WHBGfxInitFetchShader(&group);

    // Initialize the position buffer
    posBuffer.flags = GX2R_RESOURCE_BIND_VERTEX_BUFFER | GX2R_RESOURCE_USAGE_CPU_READ |
        GX2R_RESOURCE_USAGE_CPU_WRITE | GX2R_RESOURCE_USAGE_GPU_READ;
    posBuffer.elemSize = 2 * sizeof(float);
    posBuffer.elemCount = 8 * MAX_DRAWS;
    GX2RCreateBuffer(&posBuffer);

    // Initialize the texture coordinate buffer
    texBuffer.flags = GX2R_RESOURCE_BIND_VERTEX_BUFFER | GX2R_RESOURCE_USAGE_CPU_READ |
        GX2R_RESOURCE_USAGE_CPU_WRITE | GX2R_RESOURCE_USAGE_GPU_READ;
    texBuffer.elemSize = 2 * sizeof(float);
    texBuffer.elemCount = 4 * MAX_DRAWS;
    GX2RCreateBuffer(&texBuffer);

    // Initialize the vertex color buffer
    colBuffer.flags = GX2R_RESOURCE_BIND_VERTEX_BUFFER | GX2R_RESOURCE_USAGE_CPU_READ |
        GX2R_RESOURCE_USAGE_CPU_WRITE | GX2R_RESOURCE_USAGE_GPU_READ;
    colBuffer.elemSize = 4 * sizeof(float);
    colBuffer.elemCount = 4 * MAX_DRAWS;
    GX2RCreateBuffer(&colBuffer);

    // Initialize samplers for each texture filter mode
    GX2InitSampler(&samplers[0], GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_POINT);
    GX2InitSampler(&samplers[1], GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_LINEAR);

    // Configure shading and blending for the TV
    WHBGfxBeginRenderTV();
    GX2SetFetchShader(&group.fetchShader);
    GX2SetVertexShader(group.vertexShader);
    GX2SetPixelShader(group.pixelShader);
    GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_NEVER);
    GX2SetAlphaTest(TRUE, GX2_COMPARE_FUNC_GREATER, 0.0f);
    GX2SetColorControl(GX2_LOGIC_OP_COPY, 0xFF, FALSE, TRUE);
    GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_INV_SRC_ALPHA,
        GX2_BLEND_COMBINE_MODE_ADD, TRUE, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_ZERO, GX2_BLEND_COMBINE_MODE_ADD);

    // Configure shading and blending for the gamepad
    WHBGfxBeginRenderDRC();
    GX2SetFetchShader(&group.fetchShader);
    GX2SetVertexShader(group.vertexShader);
    GX2SetPixelShader(group.pixelShader);
    GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_NEVER);
    GX2SetAlphaTest(TRUE, GX2_COMPARE_FUNC_GREATER, 0.0f);
    GX2SetColorControl(GX2_LOGIC_OP_COPY, 0xFF, FALSE, TRUE);
    GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_INV_SRC_ALPHA,
        GX2_BLEND_COMBINE_MODE_ADD, TRUE, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_ZERO, GX2_BLEND_COMBINE_MODE_ADD);

    // Initialize audio output
    SDL_Init(SDL_INIT_AUDIO);
    SDL_AudioSpec AudioSettings, ObtainedSettings;
    AudioSettings.freq = 32768;
    AudioSettings.format = AUDIO_S16MSB;
    AudioSettings.channels = 2;
    AudioSettings.samples = 1024;
    AudioSettings.callback = &outputAudio;
    AudioSettings.userdata = nullptr;
    SDL_AudioDeviceID id = SDL_OpenAudioDevice(nullptr, 0, &AudioSettings, &ObtainedSettings, 0);
    SDL_PauseAudioDevice(id, 0);

    // Initialize the UI and open the file browser
    std::string base = WHBGetSdCardMountPath();
    ConsoleUI::initialize(tvWidth, tvHeight, base, base + "/wiiu/apps/noods/");
    ConsoleUI::fileBrowser();

    // Run the emulator until it exits
    ConsoleUI::mainLoop(nullptr, &gpLayout);
    return 0;
}

#ifdef HEARTBEAT
int logHeartbeat(int argc, const char **argv) {
    // Append the progress counters to a file a few times a second, closing it every time so it survives a freeze
    std::string path = std::string(WHBGetSdCardMountPath()) + "/wiiu/apps/noods/heartbeat.log";
    FILE *file = fopen(path.c_str(), "w");
    if (file) fclose(file);
    for (uint32_t tick = 0;; tick++) {
        OSSleepTicks(OSMillisecondsToTicks(250));
        volatile uint32_t *v = Heartbeat::values;
        if (!(file = fopen(path.c_str(), "a"))) continue;
        fprintf(file, "%u: frames %u task %u wait %u | jit %08X in %u entries %u | 2D %u 3D %u | ui %u stage %u | audio %u\n",
            tick, v[HB_EMU_FRAMES], v[HB_EMU_TASK], v[HB_EMU_WAIT], v[HB_JIT_BLOCK], v[HB_JIT_INSIDE],
            v[HB_JIT_ENTRIES], v[HB_2D_LINES], v[HB_3D_LINES], v[HB_UI_FRAMES], v[HB_UI_STAGE], v[HB_AUDIO]);
        fclose(file);
    }
    return 0;
}
#endif

int main() {
    ProcUIInit(OSSavesDone_ReadyToRelease);

    // Run the UI on core 2 so it doesn't compete with the JIT's emulator thread, which has to use core 1
    // The main thread can't be moved there, since running threads can't change cores
#ifdef HEARTBEAT
    WHBMountSdCard();
    WorkerThread::createThread(logHeartbeat, nullptr, 0x10000, 20, OS_THREAD_ATTRIB_AFFINITY_CPU0);
#endif
    OSThreadEntryPointFn entry = [](int argc, const char **argv) -> int {
        runUi(argc, argv);
        std::lock_guard<std::mutex> guard(procMutex);
        uiDone = true;
        procCond.notify_all();
        return 0;
    };

    if (WorkerThread::createThread(entry, nullptr, 0x100000,
            OSGetThreadPriority(OSGetCurrentThread()), OS_THREAD_ATTRIB_AFFINITY_CPU2)) {
        // Handle system messages while the UI runs, which ProcUI only allows on this core
        bool launched = false;
        while (true) {
            switch (ProcUIProcessMessages(true)) {
            case PROCUI_STATUS_EXITING:
                // Exit, leaving the UI stopped if it's still running; save data was written when it stopped
                ProcUIShutdown();
                return 0;

            case PROCUI_STATUS_RELEASE_FOREGROUND: {
                // Wait for the UI to stop drawing before releasing the foreground
                std::unique_lock<std::mutex> lock(procMutex);
                releaseRequested = true;
                procCond.wait(lock, [] { return uiParked || uiDone; });
                lock.unlock();
                ProcUIDrawDoneRelease();
                break;
            }

            default: {
                // Let the UI continue if the foreground came back
                std::unique_lock<std::mutex> lock(procMutex);
                if (releaseRequested) {
                    releaseRequested = false;
                    procCond.notify_all();
                }

                // Return to the system menu once the UI has finished
                if (uiDone && !launched) {
                    launched = true;
                    lock.unlock();
                    SYSLaunchMenu();
                    break;
                }
                lock.unlock();
                OSSleepTicks(OSMillisecondsToTicks(20));
                break;
            }
            }
        }
    }

    runUi(0, nullptr);
    SYSLaunchMenu();

    // Respond to system messages appropriately to allow exiting
    while (true) {
        switch (ProcUIProcessMessages(true)) {
        case PROCUI_STATUS_EXITING:
            ProcUIShutdown();
            return 0;

        case PROCUI_STATUS_RELEASE_FOREGROUND:
            ProcUIDrawDoneRelease();
            break;
        }
    }
}

#endif // __WIIU__
