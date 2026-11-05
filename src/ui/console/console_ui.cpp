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
#include <atomic>
#include <functional>
#include <cstring>
#include <new>
#include <chrono>
#include <cmath>
#include <dirent.h>
#include <sys/stat.h>

#include "console_ui.h"
#include "screen_frame.h"
#include "version.h"
#include <coreinit/core.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <coreinit/userconfig.h>

// The emulator runs on its own thread, bound to the core that can run generated code
static OSThread *emuThread = nullptr;
#include "../nds_icon.h"
#include "../../core/settings.h"
#include "../../core/file_lock.h"
#include "../../core/heartbeat.h"
#include "../../core/worker_thread.h"
#include "../../core/emu_profile.h"
#include "../../core/frame_capture.h"
#ifdef ROM_TEST
#include "../../core/rom_test.h"
#endif
#ifdef STATE_DEBUG
#include "gpu3d_wiiu.h"
#endif

#ifdef PROFILE_SAMPLER
#include "sampler.h"
#endif
#if defined(PROFILE_EMU) && defined(HEARTBEAT)
#include "frame_trace.h"
#endif
#ifdef FRAME_LOG
#include "frame_log.h"
#endif
#ifdef AUDIO_CAPTURE
static uint32_t audioMarks = 0;
#endif

#define SCALEH(x, h) (((x) * (h)) / 720)
#define SCALE(x) SCALEH(x, uiHeight)

extern uint8_t _binary_src_ui_console_images_file_dark_bmp_start;
extern uint8_t _binary_src_ui_console_images_file_light_bmp_start;
extern uint8_t _binary_src_ui_console_images_folder_dark_bmp_start;
extern uint8_t _binary_src_ui_console_images_folder_light_bmp_start;
extern uint8_t _binary_src_ui_console_images_font_bmp_start;
extern uint8_t _binary_src_ui_console_images_pointer_bmp_start;

void *ConsoleUI::fileTextures[2];
void *ConsoleUI::folderTextures[2];
void *ConsoleUI::pointerTexture;
void *ConsoleUI::fontTexture;

const uint32_t *ConsoleUI::palette;
uint32_t ConsoleUI::uiWidth, ConsoleUI::uiHeight;
uint32_t ConsoleUI::lineHeight;
bool ConsoleUI::touchMode;

Core *ConsoleUI::core;
bool ConsoleUI::running;
std::string ConsoleUI::ndsPath, ConsoleUI::gbaPath;
std::string ConsoleUI::basePath, ConsoleUI::curPath, ConsoleUI::defaultFolder;

alignas(4096) uint32_t ConsoleUI::framebuffer[256 * 192 * 8];
ScreenLayout ConsoleUI::layout;
bool ConsoleUI::gbaMode;
bool ConsoleUI::screenShift;
uint32_t *ConsoleUI::hiResData;
void *ConsoleUI::hiResTexture;
void *ConsoleUI::blankHiRes;
bool ConsoleUI::hiResTop;
void *ConsoleUI::composed[2];
void *ConsoleUI::composedLast[2];
bool ConsoleUI::screensComposed;
uint32_t *ConsoleUI::screenFrame = ConsoleUI::framebuffer;
uint32_t *ConsoleUI::lastFrame;
void *ConsoleUI::drawLastScreen;
bool ConsoleUI::changed;

std::thread *ConsoleUI::coreThread;
WorkerThread *ConsoleUI::saveThread;
std::condition_variable ConsoleUI::cond;
std::mutex ConsoleUI::mutex;

int ConsoleUI::fpsLimiterBackup = 0;
std::mutex ConsoleUI::displayMutex;
std::condition_variable ConsoleUI::displayCond;
int ConsoleUI::displayTicks = 0;

#ifdef STATE_DEBUG
// State debugging: snapshots of the state that decides whether the CPUs run go to statelog.txt, before saving a state,
// right after loading one, every 30 frames for 10 seconds after that, and when both CPUs stay halted without waking
// for 2 seconds, which also writes every thread's state to hang.txt; the emulator thread makes them at the end of
// frames, where the state is consistent, and the UI thread writes them
static std::mutex debugMutex;
static std::vector<std::string> debugPending;
static bool debugThreads = false;
static std::atomic<int> debugAfterLoad(0);
static uint32_t debugWakes[2] = {}, debugStuckFrames = 0;

static void debugWrite(const std::string &text) {
    {
        std::lock_guard<FileLock> fileGuard(fileLock());
        if (FILE *file = fopen((Settings::basePath + "/statelog.txt").c_str(), "a")) {
            fputs(text.c_str(), file);
            fclose(file);
        }
    }
}

static void debugFrameEnd(Core *core) {
    std::vector<std::string> snapshots;
    int left = debugAfterLoad.load();
    if (left > 0) {
        if (left % 30 == 0)
            snapshots.push_back(core->debugSnapshot(("after load, " + std::to_string(600 - left) + " frames in").c_str()));
        debugAfterLoad.store(left - 1);
    }
    uint32_t wakes[] = { core->interpreter[0].wakes, core->interpreter[1].wakes };
    bool halted = core->interpreter[0].halted && core->interpreter[1].halted;
    if (halted && wakes[0] == debugWakes[0] && wakes[1] == debugWakes[1]) {
        if (++debugStuckFrames == 120)
            snapshots.push_back(core->debugSnapshot("frozen: both CPUs halted without waking for 120 frames"));
    }
    else {
        if (debugStuckFrames >= 120)
            snapshots.push_back(core->debugSnapshot(("woke up after " + std::to_string(debugStuckFrames) + " frames").c_str()));
        debugStuckFrames = 0;
    }
    debugWakes[0] = wakes[0];
    debugWakes[1] = wakes[1];
    if (snapshots.empty()) return;
    std::lock_guard<std::mutex> guard(debugMutex);
    for (std::string &snapshot : snapshots) debugPending.push_back(snapshot);
    if (debugStuckFrames == 120) debugThreads = true;
}
#endif
int ConsoleUI::showFpsCounter = 0;
int ConsoleUI::menuTheme = 0;
int ConsoleUI::frameColor = 1;
int ConsoleUI::gameLanguage = 0;
std::string ConsoleUI::nickname;
int ConsoleUI::micInput = 1;
int ConsoleUI::stickPointer = 0;
float ConsoleUI::pointerX = 128, ConsoleUI::pointerY = 96;
std::chrono::steady_clock::time_point ConsoleUI::pointerHide;
void *ConsoleUI::frameTextures[2];
int ConsoleUI::keyBinds[] = {};

const uint32_t ConsoleUI::themeColors[] = {
    0xFF2D2D2D, 0xFFFFFFFF, 0xFF4B4B4B, 0xFF232323, 0xFFE1B955, 0xFFC8FF00, 0xFFA2A2A2, // Dark
    0xFFEBEBEB, 0xFF2D2D2D, 0xFFCDCDCD, 0xFFFFFFFF, 0xFFD2D732, 0xFFF05032, 0xFF727272 // Light
};

const uint8_t ConsoleUI::charWidths[] = {
    11, 9, 11, 20, 18, 28, 24, 7, 12, 12,
    14, 24, 9, 12, 9, 16, 21, 21, 21, 21,
    21, 21, 21, 21, 21, 21, 9, 9, 26, 24,
    26, 18, 28, 24, 21, 24, 26, 20, 20, 27,
    23, 9, 17, 21, 16, 31, 27, 29, 19, 29,
    20, 18, 21, 26, 24, 37, 21, 21, 24, 12,
    16, 12, 18, 16, 9, 20, 21, 18, 21, 20,
    10, 20, 20, 8, 12, 19, 9, 30, 20, 21,
    21, 21, 12, 16, 12, 20, 17, 29, 17, 17,
    16, 9, 8, 9, 12, 40, 40, 40, 40, 40
};

void ConsoleUI::drawRectangle(float x, float y, float w, float h, uint32_t color) {
    // Draw a rectangle using a blank texture
    static uint32_t data = 0xFFFFFFFF;
    static void *texture = createTexture(&data, 1, 1);
    drawTexture(texture, 0, 0, 1, 1, x, y, w, h, false, 0, color);
}

void ConsoleUI::drawString(std::string string, float x, float y, float size, uint32_t color, bool alignRight) {
    // Set the initial offset based on alignment
    float offset = alignRight ? -stringWidth(string) : 0;

    // Move along a string and draw each character
    for (uint32_t i = 0; i < string.size(); i++) {
        float x1 = x + offset * size / 48;
        float tx = 48.0f * (((uint8_t)string[i] - 32) % 10);
        float ty = 48.0f * (((uint8_t)string[i] - 32) / 10);
        drawTexture(fontTexture, tx, ty, 47, 47, x1, y, size, size, true, 0, color);
        offset += charWidths[(uint8_t)string[i] - 32];
    }
}

void ConsoleUI::fillAudioBuffer(uint32_t *buffer, int count, int rate) {
    HB_INC(HB_AUDIO);
    // Fill the buffer with the last played sample if not running
    static uint32_t lastSample = 0;
    if (!running) {
        for (int i = 0; i < count; i++)
            buffer[i] = lastSample;
        return;
    }

    // Fill the buffer with resampled output from the core
    int scaled = count * 32768 / rate;
    uint32_t *original = core->spu.getSamples(scaled);
#ifdef FRAME_LOG
    FrameLog::audioBuffer(core->spu.ringDry, core->spu.ringFill);
#endif
    lastSample = original[scaled - 1];
    for (int i = 0; i < count; i++)
        buffer[i] = original[i * scaled / count];
#ifdef AUDIO_CAPTURE
    FrameLog::audioOutput(buffer, count);
#endif
    delete[] original;
}

uint32_t ConsoleUI::getInputPress() {
    // Scan for newly-pressed buttons
    static uint32_t buttons = 0;
    uint32_t held = getInputHeld();
    uint32_t pressed = held & ~buttons;
    buttons = held;
    return pressed;
}

void *ConsoleUI::bmpToTexture(uint8_t *bmp) {
    // Allocate data based on bitmap measurements
    int width = U8TO32(bmp, 0x12);
    int height = U8TO32(bmp, 0x16);
    uint32_t *data = new uint32_t[width * height];

    // Convert the bitmap to RGBA8 texture data
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint8_t *color = &bmp[0x46 + (((height - y - 1) * width + x) << 2)];
            data[y * width + x] = (color[3] << 24) | (color[0] << 16) | (color[1] << 8) | color[2];
        }
    }

    // Create a texture from the data
    void *texture = createTexture(data, width, height);
    delete[] data;
    return texture;
}

int ConsoleUI::stringWidth(std::string &string) {
    // Add the widths of each character in a string
    int width = 0;
    for (uint32_t i = 0; i < string.size(); i++)
        width += charWidths[(uint8_t)string[i] - 32];
    return width;
}

void ConsoleUI::initialize(int width, int height, std::string root, std::string prefix) {
    // Initialize bitmap textures
    fileTextures[0] = bmpToTexture(&_binary_src_ui_console_images_file_dark_bmp_start);
    fileTextures[1] = bmpToTexture(&_binary_src_ui_console_images_file_light_bmp_start);
    folderTextures[0] = bmpToTexture(&_binary_src_ui_console_images_folder_dark_bmp_start);
    folderTextures[1] = bmpToTexture(&_binary_src_ui_console_images_folder_light_bmp_start);
    fontTexture = bmpToTexture(&_binary_src_ui_console_images_font_bmp_start);
    pointerTexture = bmpToTexture(&_binary_src_ui_console_images_pointer_bmp_start);

    // Set the default input bindings
    for (int i = 0; i < INPUT_MAX; i++)
        keyBinds[i] = defaultKeys[i];

    // Define the platform settings
    std::vector<Setting> platformSettings = {
        Setting("showFpsCounter", &showFpsCounter, false),
        Setting("menuTheme", &menuTheme, false),
        Setting("screenFrame", &frameColor, false),
        Setting("gameLanguage", &gameLanguage, false),
        Setting("nickname", &nickname, true),
        Setting("micInput", &micInput, false),
        Setting("stickPointer", &stickPointer, false),
        Setting("defaultFolder", &defaultFolder, true),
        Setting("keyA", &keyBinds[INPUT_A], false),
        Setting("keyB", &keyBinds[INPUT_B], false),
        Setting("keySelect", &keyBinds[INPUT_SELECT], false),
        Setting("keyStart", &keyBinds[INPUT_START], false),
        Setting("keyRight", &keyBinds[INPUT_RIGHT], false),
        Setting("keyLeft", &keyBinds[INPUT_LEFT], false),
        Setting("keyUp", &keyBinds[INPUT_UP], false),
        Setting("keyDown", &keyBinds[INPUT_DOWN], false),
        Setting("keyR", &keyBinds[INPUT_R], false),
        Setting("keyL", &keyBinds[INPUT_L], false),
        Setting("keyX", &keyBinds[INPUT_X], false),
        Setting("keyY", &keyBinds[INPUT_Y], false),
        Setting("keyMenu", &keyBinds[INPUT_MENU], false),
        Setting("keyFastHold", &keyBinds[INPUT_FAST_HOLD], false),
        Setting("keyFastToggle", &keyBinds[INPUT_FAST_TOGG], false),
        Setting("keyScreenSwap", &keyBinds[INPUT_SCRN_SWAP], false),
        Setting("keyMicBlow", &keyBinds[INPUT_MIC_BLOW], false),
        Setting("keyPointerTouch", &keyBinds[INPUT_POINTER_TOUCH], false)
    };

    // Add the platform settings
    ScreenLayout::addSettings();
    Settings::add(platformSettings);

    // Load settings or set additional defaults
    if (!Settings::load(prefix)) {
        ScreenLayout::screenArrangement = 4;
        Settings::save();
    }

    // Settings that aren't in the menu can't be left in a bad state by an old settings file
    Settings::emulateAudio = 1;
    Settings::audio16Bit = 1;
    Settings::arm7Hle = 0;
    Settings::gpuOverlap = 1;

    // Initialize some values
    palette = &themeColors[menuTheme * 7];
    uiWidth = width;
    uiHeight = height;
    lineHeight = height / 480;
    basePath = curPath = root;
    changed = true;

    // Start in the default folder if it's set and still there
    struct stat folder;
    if (defaultFolder.rfind(root, 0) == 0 && !stat(defaultFolder.c_str(), &folder) && S_ISDIR(folder.st_mode))
        curPath = defaultFolder;
}

void ConsoleUI::mainLoop(MenuTouch (*specialTouch)(), ScreenLayout *touchLayout) {
    // Keep frame textures between iterations, and only recreate them when the frame changes
    void *gbaTexture = nullptr, *topTexture = nullptr, *botTexture = nullptr;
    void *lastTopTexture = nullptr, *lastBotTexture = nullptr, *lastGbaTexture = nullptr;
    bool texShift = false, texGba = false;

    while (running) {
        // Check if GBA mode changed
        if (gbaMode != (core->gbaMode && ScreenLayout::gbaCrop)) {
            gbaMode = !gbaMode;
            changed = true;
        }

        // Update the screen layout if it changed
        if (changed) {
            layout.update(uiWidth, uiHeight, gbaMode);
            if (touchLayout)
                touchLayout->update(touchLayout->winWidth, touchLayout->winHeight, gbaMode);
            if (touchLayout) updateFrames(touchLayout);
            changed = false;
        }

        // Update the framebuffer and start rendering
        // On Wii U, frames always come at native size, and the GPU builds the screens at twice the size when needed:
        // DS screens with high-res 3D, drawing it over them, or with the upscaled filter, and the GBA screen with the
        // upscaled filter, which looks like the core's upscaling since it's drawn filtered from there; it also does
        // ghosting, averaging frames with the last ones
        bool hiRes3D = Settings::highRes3D && !gbaMode;
        bool compose = !gbaMode && (Settings::highRes3D || Settings::screenFilter == 1);
        bool composeGba = gbaMode && Settings::screenFilter == 1;
        bool shift = false;
        bool ghost = Settings::screenGhost;
        screenShift = shift;
        core->gpu.frontendUpscale = shift;
        core->gpu.frontendGhosting = ghost;
        HB_SET(HB_UI_STAGE, 1);
        HB_TIMER(hbGetFrame);
        uint32_t *hiRes = nullptr;
        bool top3D = false;
        bool alternate = ghost && !compose && !composeGba; // Screens built on the GPU keep their last ones themselves
        uint32_t *out = (alternate && screenFrame == framebuffer) ? &framebuffer[256 * 192 * 4] : framebuffer;
        HB_TIMER(hbConvert);
        bool newFrame = core->gpu.getFrame(out, gbaMode, hiRes3D ? &hiRes : nullptr, &top3D);
        HB_ADD_US(HB_UI_CONVERT_US, hbConvert);
#ifdef FRAME_LOG
        FrameLog::uiFrame(newFrame ? core->gpu.lastFrameId : 0);
#endif
        if (newFrame) {
            lastFrame = alternate ? screenFrame : nullptr;
            screenFrame = out;
        }
        if (!alternate) {
            screenFrame = framebuffer;
            lastFrame = nullptr;
        }
        bool update = newFrame || shift != texShift || gbaMode != texGba;
#ifdef HEARTBEAT
        // Count frames the UI skipped over, and UI frames that had no new frame to show
        static uint32_t lastId = 0;
        if (newFrame) {
            uint32_t id = core->gpu.lastFrameId;
            if (id > lastId + 1 && id - lastId < 1000) HB_ADD(HB_UI_SKIPPED, id - lastId - 1);
            lastId = id;
        }
        else {
            HB_INC(HB_UI_REPEATS);
        }
#endif
#ifdef STATE_DEBUG
        {
            // Write the snapshots the emulator thread made, and every thread's state when it froze
            std::vector<std::string> snapshots;
            bool threads;
            {
                std::lock_guard<std::mutex> guard(debugMutex);
                snapshots.swap(debugPending);
                threads = debugThreads;
                debugThreads = false;
            }
            for (std::string &snapshot : snapshots) debugWrite(snapshot);
            if (threads)
                if (FILE *file = openHangReport("State debug: both CPUs halted without waking for 120 frames"))
                    fclose(file);
        }
#endif
#ifdef FRAME_CAPTURE
        // Record the screens as the UI got them, and write the capture to capture.bin once it's done, with a header
        // of the format, frame count, first frame and sizes, then the frames and the 3D frames
        static uint32_t uiFrame = 0;
        uiFrame++;
        if (newFrame && !shift) {
            if (FrameCapture::Frame *capture = FrameCapture::slot(core->gpu.lastFrameId)) {
                memcpy(capture->ui, framebuffer, sizeof(capture->ui));
                capture->uiFrame = uiFrame;
            }
        }
        if (FrameCapture::state.load() == 1 && FrameCapture::emuFrame >= FrameCapture::firstFrame + FrameCapture::FRAMES + 6)
            FrameCapture::state.store(2);
        if (FrameCapture::state.load() == 2) {
            {
                std::lock_guard<FileLock> fileGuard(fileLock());
                if (FILE *file = fopen((Settings::basePath + "/capture.bin").c_str(), "wb")) {
                    uint32_t header[] = { 0x4E434150, (uint32_t)FrameCapture::FRAMES, FrameCapture::firstFrame,
                        (uint32_t)sizeof(FrameCapture::Frame), (uint32_t)FrameCapture::RENDERS, (uint32_t)sizeof(FrameCapture::Render) };
                    fwrite(header, sizeof(header), 1, file);
                    fwrite(FrameCapture::frames, sizeof(FrameCapture::Frame), FrameCapture::FRAMES, file);
                    fwrite(FrameCapture::renders, sizeof(FrameCapture::Render), FrameCapture::RENDERS, file);
                    fclose(file);
                }
            }
            delete[] FrameCapture::frames;
            FrameCapture::frames = nullptr;
            FrameCapture::state.store(3);
        }
#endif
        // Replace the high-res 3D frame with a new one, or drop it when it's not used
        if (newFrame || !hiRes3D) {
            if (hiResTexture) destroyHiResTexture(hiResTexture);
            Gpu::freeHiRes3D(hiResData);
            hiResData = hiRes;
            hiResTop = top3D;
            hiResTexture = hiRes3D ? createHiResTexture(hiRes) : nullptr;
        }
        HB_ADD_US(HB_UI_GETFRAME_US, hbGetFrame);
        if (update) HB_INC(HB_UI_NEW_FRAMES);
        HB_TIMER(hbTexture);
        texShift = shift;
        texGba = gbaMode;
        // Update the screen textures before starting the frame, since copying them doesn't need to hold up the GPU
        // renderer's access to the GPU; building high-res screens needs both, whichever are shown
        bool showTop = (ScreenLayout::arrangement() != 3 || ScreenLayout::screenSizing < 2);
        bool showBot = (ScreenLayout::arrangement() != 3 || ScreenLayout::screenSizing == 2);
        screensComposed = compose;
        if (gbaMode) {
            if (update || !gbaTexture) {
                if (gbaTexture) destroyTexture(gbaTexture);
                gbaTexture = createTexture(&screenFrame[0], 240 << shift, 160 << shift);
            }
            if (update || !lastFrame) {
                if (lastGbaTexture) destroyTexture(lastGbaTexture);
                lastGbaTexture = lastFrame ? createTexture(&lastFrame[0], 240, 160) : nullptr;
            }
        }
        else {
            if ((showTop || screensComposed) && (update || !topTexture)) {
                if (topTexture) destroyTexture(topTexture);
                topTexture = createTexture(&screenFrame[0], 256 << shift, 192 << shift);
            }
            if ((showBot || screensComposed) && (update || !botTexture)) {
                if (botTexture) destroyTexture(botTexture);
                botTexture = createTexture(&screenFrame[(256 * 192) << (shift * 2)], 256 << shift, 192 << shift);
            }
            if (update || !lastFrame) {
                if (lastTopTexture) destroyTexture(lastTopTexture);
                if (lastBotTexture) destroyTexture(lastBotTexture);
                lastTopTexture = (lastFrame && showTop) ? createTexture(&lastFrame[0], 256, 192) : nullptr;
                lastBotTexture = (lastFrame && showBot) ? createTexture(&lastFrame[256 * 192], 256, 192) : nullptr;
            }
        }

        HB_ADD_US(HB_UI_TEXTURE_US, hbTexture);
        HB_SET(HB_UI_STAGE, 2);
        HB_TIMER(hbDraw);
        startFrame(0);
        HB_SET(HB_UI_STAGE, 3);
        // Build the screens at twice the size once for each new frame
        if (compose && (update || !composed[0]))
            composeScreens(topTexture, botTexture, hiResTexture, hiResTop);
        if (composeGba && (update || !composed[0]))
            composeGbaScreen(gbaTexture);

        // Draw the screen frames first, so the screens go over them
        if (frameTextures[0] || frameTextures[1])
            drawFrames(frameTextures[0], frameTextures[1]);

        if (gbaMode) {
            // Draw the GBA screen, or the one built from it at twice the size, with the last one for ghosting
            drawLastScreen = composeGba ? (ghost ? composedLast[0] : nullptr) : lastGbaTexture;
            drawTexture(composeGba ? composed[0] : gbaTexture, 0, 0, (composeGba ? 480 : 240) << shift,
                (composeGba ? 320 : 160) << shift, layout.topX, layout.topY, layout.topWidth, layout.topHeight,
                Settings::screenFilter, ScreenLayout::screenRotation);
            drawLastScreen = nullptr;
        }
        else { // DS mode
            // Draw the DS screens, or the high-res ones built from them, with the last ones for ghosting
            int width = screensComposed ? 512 : (256 << shift), height = screensComposed ? 384 : (192 << shift);
            if (showTop) {
                drawLastScreen = screensComposed ? (ghost ? composedLast[0] : nullptr) : lastTopTexture;
                drawTexture(screensComposed ? composed[0] : topTexture, 0, 0, width, height, layout.topX, layout.topY,
                    layout.topWidth, layout.topHeight, Settings::screenFilter, ScreenLayout::screenRotation);
            }
            if (showBot) {
                drawLastScreen = screensComposed ? (ghost ? composedLast[1] : nullptr) : lastBotTexture;
                drawTexture(screensComposed ? composed[1] : botTexture, 0, 0, width, height, layout.botX, layout.botY,
                    layout.botWidth, layout.botHeight, Settings::screenFilter, ScreenLayout::screenRotation);
            }
            drawLastScreen = nullptr;
        }

#ifdef DRC_DEBUG
        {
            // Show what's known about the GamePad's display, a line at a time
            std::string text = displayDebugText();
            for (size_t start = 0, line = 0; start < text.size(); line++) {
                size_t end = text.find('\n', start);
                if (end == std::string::npos) end = text.size();
                drawString(text.substr(start, end - start), SCALE(5), SCALE(48) + SCALE(26) * line, SCALE(24));
                start = end + 1;
            }
        }
#endif
#ifdef AUDIO_CAPTURE
        // Show how many times the audio capture was marked
        if (audioMarks) drawString("Marks: " + std::to_string(audioMarks), SCALE(5), SCALE(48), SCALE(36));
#endif

#ifdef FRAME_CAPTURE
        // Show how frame capture is going
        static const char *captureStates[] = { "Frame capture: hold ZL + ZR + Y", "Capturing frames...",
            "Writing capture...", "Capture written to capture.bin" };
        drawString(captureStates[FrameCapture::state.load()], SCALE(5), SCALE(48), SCALE(36));
#endif

        // Draw the FPS counter if enabled
        if (showFpsCounter) {
            // While a ROM is streamed into RAM, also show how much of it is loaded
            std::string text = std::to_string(core->fps) + " FPS";
            int percent = Cartridge::loadPercent.load();
            if (Settings::romInRam == 2 && percent >= 0 && percent < 100)
                text += "  ROM " + std::to_string(percent) + "%";
            drawString(text, SCALE(5), 0, SCALE(48));
        }

#if defined(PROFILE_EMU) && !defined(HEARTBEAT)
        // With PROFILE_EMU, show where the emulator thread spends its time, as a share of each second and per frame
        // Heartbeat builds log it instead, since drawing this much text slows the GPU enough to change the results
        if (showFpsCounter) {
            static uint64_t last[EP_COUNT];
            static OSTime lastTime = OSGetSystemTime();
            static std::string lines[10];
            OSTime now = OSGetSystemTime();
            if (OSTicksToMilliseconds(now - lastTime) >= 1000) {
                double wall = double(now - lastTime), frames = std::max(core->fps, 1);
                double pct[EP_COUNT], ms[EP_COUNT];
                for (int i = 0; i < EP_COUNT; i++) {
                    uint64_t t = EmuProfile::ticks[i];
                    pct[i] = (t - last[i]) * 100.0 / wall;
                    ms[i] = (t - last[i]) * 1000.0 / OSTimerClockSpeed / frames;
                    last[i] = t;
                }
                double arm9 = pct[EP_ARM9] + pct[EP_BOTH] - pct[EP_BOTH_ARM7];
                double arm7 = pct[EP_ARM7] + pct[EP_BOTH_ARM7];
                double arm9ms = ms[EP_ARM9] + ms[EP_BOTH] - ms[EP_BOTH_ARM7];
                double arm7ms = ms[EP_ARM7] + ms[EP_BOTH_ARM7];
                double other = 100 - pct[EP_ARM9] - pct[EP_ARM7] - pct[EP_BOTH] - pct[EP_TASKS];
                char text[128];
                snprintf(text, sizeof(text), "ARM9 %.0f%% %.1fms/f (alone %.0f%%, with ARM7 %.0f%%)", arm9, arm9ms,
                    pct[EP_ARM9], pct[EP_BOTH] - pct[EP_BOTH_ARM7]);
                lines[0] = text;
                snprintf(text, sizeof(text), "ARM7 %.0f%% %.1fms/f, tasks %.0f%% %.1fms/f, other %.0f%%",
                    arm7, arm7ms, pct[EP_TASKS], ms[EP_TASKS], other);
                lines[1] = text;
                snprintf(text, sizeof(text), "2D wait %.0f%% %.1fms/f, 2D draw %.0f%%, compile %.0f%%",
                    pct[EP_WAIT2D], ms[EP_WAIT2D], pct[EP_DRAW2D], pct[EP_COMPILE]);
                lines[2] = text;
                snprintf(text, sizeof(text), "3D join %.1fms/f, 2D join %.1fms/f, geometry %.1fms/f, audio %.1fms/f",
                    ms[EP_JOIN3D], ms[EP_JOIN2D], ms[EP_GEOM], ms[EP_SPU]);
                lines[3] = text;
                snprintf(text, sizeof(text), "Geometry ms/f: vertex %.1f (clip %.1f), matrix %.1f, texcoord %.1f, other %.1f, "
                    "FIFO writes %.1f, worker wait %.1f", ms[EP_GEOM_VTX], ms[EP_GEOM_CLIP], ms[EP_GEOM_MTX], ms[EP_GEOM_TEX],
                    ms[EP_GEOM_OTHER], ms[EP_GXFIFO], ms[EP_GEOM_SYNC]);
                lines[5] = text;
                static uint32_t lastCount[EC_COUNT];
                double lineCounts[EC_COUNT];
                for (int i = 0; i < EC_COUNT; i++) {
                    lineCounts[i] = (EmuProfile::counts[i] - lastCount[i]) / frames;
                    lastCount[i] = EmuProfile::counts[i];
                }
                snprintf(text, sizeof(text), "2D lines/f: emu A %.0f, emu B %.0f, thread B %.0f; 3D help %.1fms/f, "
                    "3D wait %.1fms/f, capture %.1fms/f", lineCounts[EC_LINES_EMU], lineCounts[EC_LINES_HALF],
                    lineCounts[EC_LINES_THREAD], ms[EP_3D_HELP], ms[EP_3D_WAIT], ms[EP_CAPTURE]);
                lines[6] = text;
                snprintf(text, sizeof(text), "2D ms/f: clear %.1f, objects %.1f, BGs %.1f, blend %.1f, output %.1f; "
                    "3D swap %.1f, frame copy %.1f", ms[EP_2D_CLEAR], ms[EP_2D_OBJ], ms[EP_2D_BG], ms[EP_2D_BLEND],
                    ms[EP_2D_OUT], ms[EP_SWAP3D], ms[EP_FRAMECOPY]);
                lines[7] = text;
                static uint64_t lastRender[4][RP_COUNT];
                double renderMs[RP_COUNT] = {};
                for (int t = 0; t < 4; t++) {
                    for (int i = 0; i < RP_COUNT; i++) {
                        renderMs[i] += (EmuProfile::renderTicks[t][i] - lastRender[t][i]) * 1000.0 / OSTimerClockSpeed / frames;
                        lastRender[t][i] = EmuProfile::renderTicks[t][i];
                    }
                }
                snprintf(text, sizeof(text), "3D threads ms/f (all threads): draw %.1f, finish %.1f, wait %.1f",
                    renderMs[RP_DRAW], renderMs[RP_FINISH], renderMs[RP_WAIT]);
                lines[8] = text;
                static uint64_t lastPixel[3][PX_COUNT];
                double pixelMs[PX_COUNT] = {};
                for (int c = 0; c < 3; c++) {
                    for (int i = 0; i < PX_COUNT; i++) {
                        pixelMs[i] += (EmuProfile::pixelTicks[c][i] - lastPixel[c][i]) * 1000.0 / OSTimerClockSpeed / frames;
                        lastPixel[c][i] = EmuProfile::pixelTicks[c][i];
                    }
                }
                snprintf(text, sizeof(text), "3D draw ms/f: clear %.1f, span setup %.1f, pixel depth %.1f, interp %.1f, texel %.1f, "
                    "write %.1f", pixelMs[PX_CLEAR], pixelMs[PX_SPAN], pixelMs[PX_DEPTH], pixelMs[PX_INTERP], pixelMs[PX_TEXEL],
                    pixelMs[PX_WRITE]);
                lines[9] = text;

                // List the most expensive scheduled tasks
                static const char *names[] = { "run", "reset", "cart9", "cart7", "dma9.0", "dma9.1", "dma9.2", "dma9.3",
                    "dma7.0", "dma7.1", "dma7.2", "dma7.3", "line256", "line355", "gba240", "gba308", "gx", "irq9",
                    "irq7", "spu", "gbaspu", "tm9.0", "tm9.1", "tm9.2", "tm9.3", "tm7.0", "tm7.1", "tm7.2", "tm7.3",
                    "wifi", "wifi2", "wifi3", "aes", "ndma9", "ndma7", "sd1", "sd2" };
                static uint64_t lastTask[64];
                double taskMs[MAX_TASKS];
                for (int i = 0; i < MAX_TASKS; i++) {
                    taskMs[i] = (EmuProfile::taskTicks[i] - lastTask[i]) * 1000.0 / OSTimerClockSpeed / frames;
                    lastTask[i] = EmuProfile::taskTicks[i];
                }
                std::string top;
                for (int j = 0; j < 5; j++) {
                    int best = std::max_element(taskMs, taskMs + MAX_TASKS) - taskMs;
                    snprintf(text, sizeof(text), "%s %.1f  ", names[best], taskMs[best]);
                    top += text;
                    taskMs[best] = -1;
                }
                lines[4] = "Tasks ms/f: " + top;
                lastTime = now;
            }
            for (int i = 0; i < 10; i++)
                drawString(lines[i], SCALE(5), SCALE(48) + SCALE(30) * i, SCALE(30));
        }
#endif

        HB_SET(HB_UI_STAGE, 5);
        HB_INC(HB_UI_FRAMES);
        // Scan for key input
        uint32_t pressed = getInputPress();
        uint32_t held = getInputHeld();

        // Move and draw the right stick's pointer, which takes the stick and its touch key from the game while it's on
        bool pointerTouch = false;
        if (stickPointer && !gbaMode) {
            pointerTouch = updatePointer(held, touchLayout);
            pressed &= ~(pointerKeys | keyBinds[INPUT_POINTER_TOUCH]);
            held &= ~(pointerKeys | keyBinds[INPUT_POINTER_TOUCH]);
        }
#ifdef AUDIO_CAPTURE
        // Mark the audio capture when L, R and Y are pressed, which are ZL, ZR and Y on the GamePad by default
        uint32_t markCombo = keyBinds[INPUT_L] | keyBinds[INPUT_R] | keyBinds[INPUT_Y];
        static bool markHeld = false;
        if ((held & markCombo) == markCombo && !markHeld) {
            FrameLog::mark();
            audioMarks++;
        }
        markHeld = (held & markCombo) == markCombo;
#endif

#ifdef FRAME_CAPTURE
        // Start capturing frames when L, R and Y are held, which are ZL, ZR and Y on the GamePad by default
        uint32_t combo = keyBinds[INPUT_L] | keyBinds[INPUT_R] | keyBinds[INPUT_Y];
        if ((held & combo) == combo && FrameCapture::state.load() != 1 && FrameCapture::state.load() != 2) {
            delete[] FrameCapture::frames;
            FrameCapture::frames = new FrameCapture::Frame[FrameCapture::FRAMES]();
            FrameCapture::firstFrame = FrameCapture::emuFrame + 2;
            FrameCapture::state.store(1);
        }
#endif

        // Send input to the core
        for (int i = INPUT_A; i < INPUT_MENU; i++) {
            if (pressed & keyBinds[i])
                core->input.pressKey(i);
            else if (!(held & keyBinds[i]))
                core->input.releaseKey(i);
        }

        // Scan for touch input, falling back to a special function if provided
        MenuTouch touch = getInputTouch();
        if (!touch.pressed && specialTouch)
            touch = (*specialTouch)();

        if (touch.pressed) {
            // Determine the touch position relative to the emulated touch screen
            ScreenLayout *sl = touchLayout ? touchLayout : &layout;
            int touchX = sl->getTouchX(SCALEH(touch.x, sl->winHeight), SCALEH(touch.y, sl->winHeight));
            int touchY = sl->getTouchY(SCALEH(touch.x, sl->winHeight), SCALEH(touch.y, sl->winHeight));

            // Send the touch coordinates to the core
            core->input.pressScreen();
            core->spi.setTouch(touchX, touchY);
        }
        else if (pointerTouch) {
            // Touch where the pointer is
            core->input.pressScreen();
            core->spi.setTouch(pointerX, pointerY);
        }
        else { // Released
            // Release the touch screen press
            core->input.releaseScreen();
            core->spi.clearTouch();
        }

        // Send microphone input to the core, which hears noise instead while blowing is held
        core->spi.setMicBlow(held & keyBinds[INPUT_MIC_BLOW]);
        if (micInput) {
            int16_t samples[1024];
            while (size_t count = getMicSamples(samples, 1024))
                core->spi.sendMicData(samples, count, micRate);
        }
        else {
            closeMic();
        }

        // Finish drawing
        HB_ADD_US(HB_UI_DRAW_US, hbDraw);
        HB_SET(HB_UI_STAGE, 4);
        HB_TIMER(hbEnd);
        endFrame();
        HB_ADD_US(HB_UI_END_US, hbEnd);

        // Restore the FPS limiter when pausing or releasing fast-forward hold
        if ((fpsLimiterBackup && (pressed & keyBinds[INPUT_MENU])) ||
            (uint32_t(fpsLimiterBackup - 1) < 0x100 && !(held & keyBinds[INPUT_FAST_HOLD]))) {
            Settings::fpsLimiter = fpsLimiterBackup & 0xFF;
            fpsLimiterBackup = 0;
        }

        // Handle pressing special hotkeys
        if (pressed & keyBinds[INPUT_MENU]) {
            // Open the pause menu
            pauseMenu();
        }
        else if (pressed & keyBinds[INPUT_FAST_HOLD]) {
            // Disable the FPS limiter
            if (Settings::fpsLimiter != 0) {
                fpsLimiterBackup = Settings::fpsLimiter;
                Settings::fpsLimiter = 0;
            }
        }
        else if (pressed & keyBinds[INPUT_FAST_TOGG]) {
            // Toggle between disabling and restoring the FPS limiter
            if (Settings::fpsLimiter != 0) {
                fpsLimiterBackup = Settings::fpsLimiter | 0x100;
                Settings::fpsLimiter = 0;
            }
            else if (fpsLimiterBackup != 0) {
                Settings::fpsLimiter = fpsLimiterBackup & 0xFF;
                fpsLimiterBackup = 0;
            }
        }
        else if (pressed & keyBinds[INPUT_SCRN_SWAP]) {
            // Toggle between favoring the top or bottom screen
            ScreenLayout::screenSizing = (ScreenLayout::screenSizing == 1) ? 2 : 1;
            changed = true;
        }
    }

    // Free the frame textures
    if (gbaTexture) destroyTexture(gbaTexture);
    if (topTexture) destroyTexture(topTexture);
    if (botTexture) destroyTexture(botTexture);
    if (lastTopTexture) destroyTexture(lastTopTexture);
    if (lastBotTexture) destroyTexture(lastBotTexture);
    if (lastGbaTexture) destroyTexture(lastGbaTexture);
    if (hiResTexture) destroyHiResTexture(hiResTexture);
    Gpu::freeHiRes3D(hiResData);
    hiResTexture = nullptr;
    hiResData = nullptr;
    screenFrame = framebuffer;
    lastFrame = nullptr;
}

bool ConsoleUI::updatePointer(uint32_t held, ScreenLayout *gamepad) {
    // Move the pointer with the right stick past a dead zone, faster the further it's pushed
    // The stick moves it the way it looks on the display, so it's turned with the screens
    auto now = std::chrono::steady_clock::now();
    float x, y;
    getInputStick(x, y);
    float length = sqrtf(x * x + y * y);
    if (length > 0.15f) {
        float push = (std::min(length, 1.0f) - 0.15f) / 0.85f;
        float speed = 4.5f * push * push / length;
        float dx = x * speed, dy = -y * speed;
        switch (ScreenLayout::screenRotation) {
            case 0: pointerX += dx; pointerY += dy; break; // None
            case 1: pointerX += dy; pointerY -= dx; break; // Clockwise
            default: pointerX -= dy; pointerY += dx; break; // Counter-clockwise
        }
        pointerX = std::max(0.0f, std::min(255.0f, pointerX));
        pointerY = std::max(0.0f, std::min(191.0f, pointerY));
    }

    // Touch for as long as the touch key is held, which drags if the pointer moves meanwhile
    // Show the pointer while it's used, and hide it once it's left alone for long enough unless it's always shown
    bool touching = held & keyBinds[INPUT_POINTER_TOUCH];
    if (length > 0.15f || touching)
        pointerHide = now + std::chrono::seconds(stickPointer == 1 ? 15 : 30);
    if (stickPointer != 3 && now >= pointerHide)
        return touching;

    // Draw the hand with its fingertip on the pointer, on each display that shows the touch screen
    // Each display shows both screens, or one with single screen arrangement, the TV's as set by screen sizing
    ScreenLayout *layouts[] = { &layout, gamepad };
    for (int i = 0; i < 2; i++) {
        if (!layouts[i]) continue;
        if (ScreenLayout::arrangement() == 3 && (ScreenLayout::screenSizing == 2) == (i == 1)) continue;
        ScreenLayout &l = *layouts[i];
        float px, py;
        switch (ScreenLayout::screenRotation) {
            case 0: // None
                px = l.botX + (pointerX + 0.5f) * l.botWidth / 256;
                py = l.botY + (pointerY + 0.5f) * l.botHeight / 192;
                break;
            case 1: // Clockwise
                px = l.botX + (191.5f - pointerY) * l.botWidth / 192;
                py = l.botY + (pointerX + 0.5f) * l.botHeight / 256;
                break;
            default: // Counter-clockwise
                px = l.botX + (pointerY + 0.5f) * l.botWidth / 192;
                py = l.botY + (255.5f - pointerX) * l.botHeight / 256;
                break;
        }
        float size = std::min(l.botWidth, l.botHeight) * 0.13f;
        drawOverlay(pointerTexture, px - size * 0.188f, py - size * 0.047f, size, size, i == 1);
    }
    return touching;
}

void ConsoleUI::updateFrames(ScreenLayout *gamepad) {
    // Work out what each display shows, which is one screen each when split or with the GBA screen, or both otherwise
    // A split display shows the same half of the system as its screen, and GBA mode splits them like the top and bottom
    ScreenFrame::Screen screens[2][2];
    ScreenLayout *layouts[] = { &layout, gamepad };
    int count = (gbaMode || ScreenLayout::arrangement() == 3) ? 1 : 2;
    for (int i = 0; i < 2; i++) {
        ScreenLayout &l = *layouts[i];
        bool bottom = (count == 2) ? false : gbaMode ? (i == 1) : ((ScreenLayout::screenSizing == 2) != (i == 1));
        screens[i][0] = { float(l.topX), float(l.topY), float(l.topWidth), float(l.topHeight), bottom };
        screens[i][1] = { float(l.botX), float(l.botY), float(l.botWidth), float(l.botHeight), true };
    }

    // Only draw them again if something changed, since this also runs for changes that don't affect them
    static float last[1 + 2 * 2 * 5];
    float next[1 + 2 * 2 * 5] = { float(frameColor) };
    for (int i = 0; i < 4; i++) {
        const ScreenFrame::Screen &screen = screens[i / 2][i & 1];
        const float values[] = { screen.x, screen.y, screen.width, screen.height, float(screen.bottom + count * 2) };
        memcpy(&next[1 + i * 5], values, sizeof(values));
    }
    if (!memcmp(last, next, sizeof(next))) return;
    memcpy(last, next, sizeof(next));

    for (int i = 0; i < 2; i++) {
        releaseTexture(frameTextures[i]);
        frameTextures[i] = nullptr;
    }
    if (!frameColor) return;

    // Draw each display's frame in memory, leaving black bars if there isn't enough for it
#ifdef FRAME_ART_TEST
    uint64_t start = OSGetSystemTime();
#endif
    for (int i = 0; i < 2; i++) {
        int width = (i == 0) ? uiWidth : 854, height = (i == 0) ? uiHeight : 480;
        uint32_t *pixels = new (std::nothrow) uint32_t[width * height];
        if (!pixels) continue;
        ScreenFrame::draw(pixels, width, height, frameColor - 1, screens[i], count, i == 0,
            &_binary_src_ui_console_images_font_bmp_start, charWidths);
        frameTextures[i] = createTexture(pixels, width, height);
        delete[] pixels;
    }
#ifdef FRAME_ART_TEST
    // Log how long that took
    {
        std::lock_guard<FileLock> fileGuard(fileLock());
        if (FILE *log = fopen((Settings::basePath + "/frameart.txt").c_str(), "a")) {
            fprintf(log, "Color %d, %dx%d, %d screens: %.1f ms\n", frameColor, uiWidth, uiHeight, count,
                OSTicksToMicroseconds(OSGetSystemTime() - start) / 1000.0);
            fclose(log);
        }
    }
#endif
}

int ConsoleUI::setPath(std::string path) {
    // Set the ROM path if the extension matches
    if (path.find(".nds", path.length() - 4) != std::string::npos) { // NDS ROM
        // If a GBA path is set, allow clearing it
        if (gbaPath != "") {
            if (!message("Loading NDS ROM", "Load the previous GBA ROM alongside this ROM?", 1))
                gbaPath = "";
        }

        // Set the NDS ROM path
        ndsPath = path;

#ifdef JIT_SELFTEST
        // Test the JIT against the interpreter instead of playing
        jitSelfTest();
        ndsPath = "";
        return 1;
#endif
#ifdef ROM_TEST
        // Run tests/rom's ROM instead of playing
        romTest();
        ndsPath = "";
        return 1;
#endif

        // Attempt to boot the core with the set ROMs
        if (createCore()) {
            startCore();
            return 2;
        }

        // Clear the NDS ROM path if booting failed
        ndsPath = "";
        return 1;
    }
    else if (path.find(".gba", path.length() - 4) != std::string::npos) { // GBA ROM
        // If an NDS path is set, allow clearing it
        if (ndsPath != "") {
            if (!message("Loading GBA ROM", "Load the previous NDS ROM alongside this ROM?", 1))
                ndsPath = "";
        }

        // Set the GBA ROM path
        gbaPath = path;

        // Attempt to boot the core with the set ROMs
        if (createCore()) {
            startCore();
            return 2;
        }

        // Clear the GBA ROM path if booting failed
        gbaPath = "";
        return 1;
    }
    return 0;
}

uint32_t ConsoleUI::menu(std::string title, std::vector<MenuItem> &items,
    int &index, std::string actionX, std::string actionPlus, bool actionOk, std::string actionY, uint32_t hiddenKeys) {
    // Define the action strings
    if (actionY != "") actionY = "\x7F " + actionY + "     ";
    if (actionPlus != "") actionPlus = "\x83 " + actionPlus + "     ";
    if (actionX != "") actionX = "\x82 " + actionX + "     ";
    std::string actionB = actionOk ? "\x81 Back     " : "\x81 Back";
    std::string actionA = actionOk ? "\x80 OK" : "";

    // Calculate touch bounds for the action buttons, with Back reaching the edge if it's the last one
    int boundsAB = 1218 - (stringWidth(actionA) + 2.5f * charWidths[0]) * 34 / 48;
    int boundsBX = boundsAB - stringWidth(actionB) * 34 / 48;
    if (!actionOk) boundsAB = 1280;
    int boundsX = boundsBX - stringWidth(actionX) * 34 / 48;
    int boundsY = boundsX - stringWidth(actionY) * 34 / 48;
    int boundsPlus = boundsY - stringWidth(actionPlus) * 34 / 48;

    // Define variables for button input
    bool upHeld = false;
    bool downHeld = false;
    bool scroll = false;
    std::chrono::steady_clock::time_point timeHeld;

    // Define variables for touch input
    int touchIndex = 0;
    bool touchStarted = false;
    bool touchScroll = false;
    MenuTouch touchStart(false, 0, 0);

    // Ensure a header isn't selected
    index += items.empty() ? 0 : items[index].header;
    int min = items.empty() ? 0 : items[0].header;

    // A list starting with a header starts right under the title, since a header has room above its text, which makes
    // room for another row if they're slightly shorter, ending as far above the bottom bar as other lists do
    bool fill = !items.empty() && items[0].header;
    const int top = fill ? 88 : 124, rows = fill ? 8 : 7, rowHeight = fill ? 66 : 70;

    while (true) {
        // Draw the borders
        startFrame(palette[0]);
        drawString(title, SCALE(72), SCALE(30), SCALE(42), palette[1]);
        drawRectangle(SCALE(30), SCALE(88), SCALE(1220), lineHeight, palette[1]);
        drawRectangle(SCALE(30), SCALE(648), SCALE(1220), lineHeight, palette[1]);
        drawString(NOODSU_VERSION, SCALE(72), SCALE(667), SCALE(34), palette[1]);
        drawString(actionPlus + actionY + actionX + actionB + actionA, SCALE(1218), SCALE(667), SCALE(34), palette[1], true);

        // Scan for key input
        uint32_t pressed = getInputPress();
        uint32_t held = getInputHeld();

        // Handle up input presses
        if ((pressed & defaultKeys[INPUT_UP]) && !(pressed & defaultKeys[INPUT_DOWN])) {
            // Disable touch mode or move the selection box up, skipping headers
            if (touchMode)
                touchMode = false;
            else if (index > min)
                index--;
            index -= items.empty() ? 0 : items[index].header;

            // Remember when the up input started
            upHeld = true;
            timeHeld = std::chrono::steady_clock::now();
        }

        // Handle down input presses
        if ((pressed & defaultKeys[INPUT_DOWN]) && !(pressed & defaultKeys[INPUT_UP])) {
            // Disable touch mode or move the selection box down, skipping headers
            if (touchMode)
                touchMode = false;
            else if (index < items.size() - 1)
                index++;
            index += items.empty() ? 0 : items[index].header;

            // Remember when the down input started
            downHeld = true;
            timeHeld = std::chrono::steady_clock::now();
        }

        // Return button presses so they can be handled externally
        if (((pressed & defaultKeys[INPUT_A]) && !touchMode) || (pressed & defaultKeys[INPUT_B]) || (actionX != ""
            && (pressed & defaultKeys[INPUT_X])) || (actionPlus != "" && (pressed & defaultKeys[INPUT_START])) ||
            (actionY != "" && (pressed & defaultKeys[INPUT_Y])) || (pressed & hiddenKeys)) {
            touchMode = false;
            return pressed;
        }

        // Disable touch mode before allowing A presses so the selector is visible
        if ((pressed & defaultKeys[INPUT_A]) && touchMode) {
            touchMode = false;
            index += items.empty() ? 0 : items[index].header;
        }

        // Cancel up input if it was released
        if (upHeld && !(held & defaultKeys[INPUT_UP])) {
            upHeld = false;
            scroll = false;
        }

        // Cancel down input if it was released
        if (downHeld && !(held & defaultKeys[INPUT_DOWN])) {
            downHeld = false;
            scroll = false;
        }

        // Scroll continuously while a directional input is held
        if ((upHeld && index > min) || (downHeld && index < items.size() - 1)) {
            // When the input starts, wait a bit before scrolling
            std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - timeHeld;
            if (!scroll && elapsed.count() > 0.5f)
                scroll = true;

            // Scroll up or down at a fixed time interval, skipping headers
            if (scroll && elapsed.count() > 0.1f) {
                int step = upHeld ? -1 : 1;
                int next = index + step;
                if (!items.empty() && items[next].header) next += step;
                if (next >= min && next < (int)items.size()) index = next;
                timeHeld = std::chrono::steady_clock::now();
            }
        }

        // Scan for touch input
        MenuTouch touch = getInputTouch();

        // Handle touch input
        if (touch.pressed) {
            // Remember where a touch started
            if (!touchStarted) {
                touchStart = touch;
                touchStarted = true;
                touchScroll = false;
                touchMode = true;
            }

            // Handle touch scrolling
            if (touchScroll) {
                // Scroll the list based on how far it's been dragged
                int newIndex = touchIndex + (touchStart.y - touch.y) / rowHeight;
                if ((int)items.size() > rows && newIndex != touchIndex)
                    index = std::max(3, std::min<int>(items.size() - rows + 3, newIndex));
            }
            else if (touch.x > touchStart.x + 25 || touch.x < touchStart.x - 25
                || touch.y > touchStart.y + 25 || touch.y < touchStart.y - 25) {
                // Start scrolling from the current index if a touch is dragged
                touchScroll = true;
                touchIndex = std::max(3, std::min<int>(items.size() - rows + 3, index));
            }
        }
        else { // Released
            // Simulate a button press if its action text was tapped
            if (!touchScroll && touchStart.y >= 650) {
                if (touchStart.x >= boundsBX && touchStart.x < boundsAB)
                    return defaultKeys[INPUT_B];
                else if (touchStart.x >= boundsX && touchStart.x < boundsBX)
                    return defaultKeys[INPUT_X];
                else if (touchStart.x >= boundsY && touchStart.x < boundsX)
                    return defaultKeys[INPUT_Y];
                else if (touchStart.x >= boundsPlus && touchStart.x < boundsY)
                    return defaultKeys[INPUT_START];
            }
            touchStarted = false;
        }

        // Draw the first item separator, which a list starting with a header doesn't need
        if (items.size() > 0 && !items[0].header)
            drawRectangle(SCALE(90), SCALE(top), SCALE(1100), lineHeight, palette[2]);

        // Draw the list items
        int size = std::min<int>(rows, items.size());
        for (int i = 0, offset; i < size; i++) {
            // Determine the scroll offset
            if (index < 4 || (int)items.size() <= rows)
                offset = i;
            else if (index > (int)items.size() - rows + 3)
                offset = items.size() - rows + i;
            else
                offset = i + index - 3;

            // Simulate an A press on a selection if it was tapped
            if (!items[offset].header && (!touchStarted && !touchScroll && touchStart.x >= 90 &&
                touchStart.x < 1190 && touchStart.y >= top + i * rowHeight && touchStart.y < top + (i + 1) * rowHeight)) {
                index = offset;
                return defaultKeys[INPUT_A];
            }

            // Draw UI elements around the list items
            if (!touchMode && offset == index) {
                // Draw a box behind the selected item if not in touch mode
                drawRectangle(SCALE(90), SCALE(top + 1 + i * rowHeight), SCALE(1100), SCALE(rowHeight - 1), palette[3]);
                drawRectangle(SCALE(89), SCALE(top - 3 + i * rowHeight), SCALE(1103), SCALE(5), palette[4]);
                drawRectangle(SCALE(89), SCALE(top - 3 + (i + 1) * rowHeight), SCALE(1103), SCALE(5), palette[4]);
                drawRectangle(SCALE(88), SCALE(top - 2 + i * rowHeight), SCALE(5), SCALE(rowHeight + 3), palette[4]);
                drawRectangle(SCALE(1188), SCALE(top - 2 + i * rowHeight), SCALE(5), SCALE(rowHeight + 3), palette[4]);
            }
            else {
                // Draw separators between the items
                drawRectangle(SCALE(90), SCALE(top + (i + 1) * rowHeight), SCALE(1100), lineHeight, palette[2]);
            }

            // Draw a header item's name and skip the rest
            if (items[offset].header) {
                drawString(items[offset].name, SCALE(105), SCALE(top + rowHeight - 34 + i * rowHeight), SCALE(28), palette[6]);
                continue;
            }

            // Draw the current item's name
            int x = (items[offset].iconSize > 0) ? 184 : 105;
            drawString(items[offset].name, SCALE(x), SCALE(top + (rowHeight - 38) / 2 + i * rowHeight), SCALE(38), palette[1]);

            // Draw the current item's icon if it has one
            if (items[offset].iconSize > 0)
                drawTexture(items[offset].iconTex, 0, 0, items[offset].iconSize,
                    items[offset].iconSize, SCALE(105), SCALE(top + (rowHeight - 64) / 2 + i * rowHeight), SCALE(64), SCALE(64));

            // Draw the current item's setting if it has one
            if (items[offset].setting != "")
                drawString(items[offset].setting, SCALE(1175), SCALE(top + (rowHeight - 32) / 2 + i * rowHeight), SCALE(32), palette[5], true);
        }

        // Finish drawing
        endFrame();
    }
}

uint32_t ConsoleUI::message(std::string title, std::string text, int type) {
    // Define the action strings
    std::string actionB = "\x81 Back     ";
    std::string actionA = "\x80 OK";

    // Calculate touch bounds for the action buttons
    int boundsA = 1218 + (2.5f * charWidths[0]) * 34 / 48;
    int boundsAB = 1218 - (stringWidth(actionA) + 2.5f * charWidths[0]) * 34 / 48;
    int boundsB = boundsAB - stringWidth(actionB) * 34 / 48;

    // Define variables for touch input
    bool touchStarted = false;
    bool touchScroll = false;
    MenuTouch touchStart(false, 0, 0);

    while (true) {
        // Draw the borders
        startFrame(palette[0]);
        drawString(title, SCALE(72), SCALE(30), SCALE(42), palette[1]);
        drawRectangle(SCALE(30), SCALE(88), SCALE(1220), lineHeight, palette[1]);
        drawRectangle(SCALE(30), SCALE(648), SCALE(1220), lineHeight, palette[1]);
        if (type < 2) drawString((type ? actionB : "") + actionA, SCALE(1218), SCALE(667), SCALE(34), palette[1], true);

        // Draw each line of text, separated by newline characters
        for (int i = 0, j = 0, y = 0; j != std::string::npos; y += 38) {
            j = text.find("\n", i);
            drawString(text.substr(i, j - i), SCALE(90), SCALE(124 + y), SCALE(38), palette[1]);
            i = j + 1;
        }

        // Scan for key input
        uint32_t pressed = getInputPress();

        // Dismiss the message and return the result if an action is pressed
        if (pressed && type == 2) // Input
            return pressed;
        else if (pressed & defaultKeys[INPUT_A]) // Default
            return 1;
        else if ((pressed & defaultKeys[INPUT_B]) && type == 1) // Cancel
            return 0;

        // Scan for touch input
        MenuTouch touch = getInputTouch();

        // Handle touch input
        if (touch.pressed) {
            // Remember where a touch started
            if (!touchStarted) {
                touchStart = touch;
                touchStarted = true;
                touchScroll = false;
                touchMode = true;
            }

            // Track if a touch starts dragging instead of tapping
            if (touch.x > touchStart.x + 25 || touch.x < touchStart.x - 25 ||
                touch.y > touchStart.y + 25 || touch.y < touchStart.y - 25)
                touchScroll = true;
        }
        else { // Released
            // Simulate a button press if its action text was tapped
            if (!touchScroll && touchStart.y >= 650) {
                if (touchStart.x >= boundsAB && touchStart.x < boundsA && type < 2)
                    return 1;
                else if (touchStart.x >= boundsB && touchStart.x < boundsAB && type == 1)
                    return 0;
            }
            touchStarted = false;
        }

        // Finish drawing
        endFrame();
    }
}

bool ConsoleUI::fileBrowser() {
    // Return whether the browser was closed for good, or false if going back to a paused game's menu
    int index = 0;
    while (true) {
        // Open the current directory to list files from, keeping other file access out until the list is built, like
        // a ROM still loading in the background
        std::vector<MenuItem> files;
        std::unique_lock<FileLock> listLock(fileLock());
        DIR *dir = opendir(curPath.c_str());
        dirent *entry;

        // Build a list of directories and ROMs
        while (dir && (entry = readdir(dir))) {
            // Get information on a file
            std::string name = entry->d_name;
            std::string subpath = curPath + "/" + name;
            struct stat substat;
            stat(subpath.c_str(), &substat);

            // Add a list entry if appropriate
            if (S_ISDIR(substat.st_mode)) {
                // Add a directory with a generic icon to the list
                files.push_back(MenuItem(name, "", folderTextures[menuTheme], 64));
            }
            else if (name.find(".nds", name.length() - 4) != std::string::npos) {
                // Add an NDS ROM with its decoded icon to the list
                void *texture = createTexture(NdsIcon(subpath).getIcon(), 32, 32);
                files.push_back(MenuItem(name, "", texture, 32));
            }
            else if (name.find(".gba", name.length() - 4) != std::string::npos) {
                // Add a GBA ROM with a generic icon to the list
                files.push_back(MenuItem(name, "", fileTextures[menuTheme], 64));
            }
        }

        // Sort the files alphabetically
        sort(files.begin(), files.end());
        if (dir) closedir(dir);
        listLock.unlock();

        // Create the file browser menu
        uint32_t pressed = menu("NooDS U", files, index, "Settings", "Exit", true,
            "Set Default", defaultKeys[INPUT_SELECT]);

        // Open the launch options of a selected ROM, and launch it if one was chosen
        bool launch = (pressed & defaultKeys[INPUT_A]);
        if ((pressed & defaultKeys[INPUT_SELECT]) && !files.empty()) {
            const std::string &name = files[index].name;
            bool gba = (name.find(".gba", name.length() - 4) != std::string::npos);
            if (gba || name.find(".nds", name.length() - 4) != std::string::npos) {
                int saveSize = launchOptions(gba);
                if (saveSize >= 0) {
                    Cartridge::setLaunchSave(curPath + "/" + name, saveSize);
                    launch = true;
                }
            }
        }

        // Handle menu input
        if (launch) {
            // Navigate to the selected file if any exist
            if (files.empty()) continue;
            curPath += "/" + files[index].name;
            index = 0;

            // Try to set a ROM path, with any launch option only applying to this attempt
            int result = setPath(curPath);
            Cartridge::setLaunchSave("", -1);
            switch (result) {
            case 1: // ROM failed to load
                // Remove the ROM from the path and continue browsing
                curPath = curPath.substr(0, curPath.rfind("/"));
                index = 0;
            case 0: // ROM not selected
                continue;

            case 2: // ROM loaded
                // Save the previous directory and close the file browser
                curPath = curPath.substr(0, curPath.rfind("/"));
                return true;
            }
        }
        else if (pressed & defaultKeys[INPUT_B]) {
            // Navigate to the previous directory, or back to the pause menu from the top if a game is paused
            if (curPath != basePath) {
                curPath = curPath.substr(0, curPath.rfind("/"));
                index = 0;
            }
            else if (core) {
                return false;
            }
        }
        else if (pressed & defaultKeys[INPUT_X]) {
            // Open the settings menu
            settingsMenu();
        }
        else if (pressed & defaultKeys[INPUT_START]) {
            // Close the file browser
            return true;
        }
        else if (pressed & defaultKeys[INPUT_Y]) {
            // Start in this folder from now on
            defaultFolder = curPath;
            Settings::save();
            std::string shown = curPath.substr(basePath.size());
            message("Default Folder", "The file browser will now start in:\n" + (shown.empty() ? "/" : shown));
        }
    }
}

static std::u16string utf8To16(const std::string &text) {
    // Convert UTF-8 to UTF-16, for characters that fit in one UTF-16 unit like the DS's
    // Anything longer becomes a question mark
    std::u16string result;
    for (size_t i = 0; i < text.size();) {
        uint8_t c = text[i];
        int extra = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
        uint32_t value = c & (0x7F >> (extra + (extra > 0)));
        for (int j = 1; j <= extra && i + j < text.size(); j++)
            value = (value << 6) | (text[i + j] & 0x3F);
        result += (extra < 3) ? char16_t(value) : u'?';
        i += extra + 1;
    }
    return result;
}

static std::string utf16To8(const std::u16string &text) {
    // Convert UTF-16 to UTF-8, for characters that fit in one UTF-16 unit like the DS's
    std::string result;
    for (char16_t c : text) {
        if (c < 0x80) {
            result += char(c);
        }
        else if (c < 0x800) {
            result += char(0xC0 | (c >> 6));
            result += char(0x80 | (c & 0x3F));
        }
        else {
            result += char(0xE0 | (c >> 12));
            result += char(0x80 | ((c >> 6) & 0x3F));
            result += char(0x80 | (c & 0x3F));
        }
    }
    return result;
}

static std::string menuText(const std::u16string &text) {
    // Show text in the menu font, which only has ASCII
    std::string result;
    for (char16_t c : text)
        result += (c >= 32 && c < 127) ? char(c) : '?';
    return result;
}

static uint32_t cafeLanguage() {
    // Read the Wii U's language, which numbers the ones shared with the DS the same way
    alignas(0x40) uint32_t value = LG_ENGLISH;
    alignas(0x40) UCSysConfig config = { "cafe.language", 0x777, UC_DATATYPE_UNSIGNED_INT, 0, sizeof(value), &value };
    UCHandle handle = UCOpen();
    if (handle >= 0) {
        if (UCReadSysConfig(handle, 1, &config) != 0) value = LG_ENGLISH;
        UCClose(handle);
    }
    return value;
}

void ConsoleUI::settingsMenu() {
    // Define possible values for settings
    const std::vector<std::string> toggle = { "Off", "On" };
    const std::vector<std::string> theme = { "Dark", "Light" };
    const std::vector<std::string> frames = { "None", "1 Frame", "2 Frames", "3 Frames", "4 Frames", "5 Frames" };
    const std::vector<std::string> threads = { "Disabled", "1 Thread", "2 Threads" };
    const std::vector<std::string> position = { "Center", "Top", "Bottom", "Left", "Right" };
    const std::vector<std::string> rotation = { "None", "Clockwise", "Counter-Clockwise" };
    const std::vector<std::string> arrangement = { "Automatic", "Vertical", "Horizontal", "Single Screen",
        "TV + GamePad (Horizontal)", "TV + GamePad (Vertical)" };
    const std::vector<std::string> sizing = { "Even", "Enlarge Top", "Enlarge Bottom" };
    const std::vector<std::string> gap = { "None", "Quarter", "Half", "Full" };
    const std::vector<std::string> filter = { "Nearest", "Upscaled", "Linear" };
    const std::vector<std::string> aspect = { "Default", "16:10", "16:9", "18:9" };
    std::vector<std::string> frame = { "Off" };
    frame.insert(frame.end(), ScreenFrame::colorNames, ScreenFrame::colorNames + ScreenFrame::COLOR_COUNT);
    const std::vector<std::string> antiAlias = { "DS-Style", "Multisample", "Off" };
    const std::vector<std::string> romIn = { "Off", "On", "Streaming" };
    const std::vector<std::string> directBoot = { "Off (broken)", "On" };
    const std::vector<std::string> dsiMode = { "Off", "On (slow)" };
    const std::vector<std::string> language = { "System Language", "Japanese", "English", "French", "German",
        "Italian", "Spanish" };
    const std::vector<std::string> mic = { "Off", "GamePad" };
    const std::vector<std::string> pointer = { "Off", "On (Hide After 15s)", "On (Hide After 30s)", "On (Always)" };
    if (Settings::gpuAntiAlias >= (int)antiAlias.size()) Settings::gpuAntiAlias = 0;

    // The Mii name is shown for the nickname when it isn't set
    std::u16string miiName = getMiiName();

    int index = 0;
    while (true) {
        // Create a list of settings and current values, remembering what each one changes
        std::vector<MenuItem> settings;
        std::vector<std::pair<int*, int>> values;
        auto header = [&](const char *name) {
            settings.push_back(MenuItem(name, true));
            values.push_back({ nullptr, 0 });
        };
        auto add = [&](const char *name, int &value, const std::vector<std::string> &choices) {
            settings.push_back(MenuItem(name, choices[value % choices.size()]));
            values.push_back({ &value, (int)choices.size() });
        };

        header("General Settings");
        add("Keep ROM in RAM", Settings::romInRam, romIn);
        add("JIT", Settings::jit, toggle);
        add("Fast CPU Timing", Settings::fastTiming, toggle);
        add("FPS Limiter", Settings::fpsLimiter, toggle);
        add("Sync to Display", Settings::syncToDisplay, toggle);
        add("Show FPS Counter", showFpsCounter, toggle);
        add("Menu Theme", menuTheme, theme);
        add("Screen Frame", frameColor, frame);
        add("Game Language", gameLanguage, language);
        int nicknameIndex = settings.size();
        settings.push_back(MenuItem("Nickname", nickname.empty() ?
            (miiName.empty() ? "Mii Name" : "Mii Name (" + menuText(miiName) + ")") :
            menuText(utf8To16(nickname))));
        values.push_back({ nullptr, 0 });
        add("Microphone", micInput, mic);
        add("Right Stick Pointer", stickPointer, pointer);

        header("Graphics Settings");
        add("GPU 3D Renderer", Settings::hardware3D, toggle);
        if (!Settings::hardware3D) {
            // These only do something for the software renderer
            add("Threaded 2D", Settings::threaded2D, toggle);
            add("Threaded 3D", Settings::threaded3D, threads);
        }
        add("Fast 2D Threads", Settings::fast2D, toggle);
        add("High-Resolution 3D", Settings::highRes3D, toggle);
        add("GPU 3D Anti-Aliasing", Settings::gpuAntiAlias, antiAlias);
        add("Skip Frames", Settings::frameskip, frames);
        add("Simulate Ghosting", Settings::screenGhost, toggle);

        header("Screen Layout");
        add("Screen Position", ScreenLayout::screenPosition, position);
        add("Screen Rotation", ScreenLayout::screenRotation, rotation);
        add("Screen Arrangement", ScreenLayout::screenArrangement, arrangement);
        add("Screen Sizing", ScreenLayout::screenSizing, sizing);
        add("Screen Gap", ScreenLayout::screenGap, gap);
        add("Screen Filter", Settings::screenFilter, filter);
        add("Aspect Ratio", ScreenLayout::aspectRatio, aspect);
        add("Integer Scale", ScreenLayout::integerScale, toggle);
        add("GBA Crop", ScreenLayout::gbaCrop, toggle);

        header("Experimental");
        add("Direct Boot", Settings::directBoot, directBoot);
        add("DSi Mode", Settings::dsiMode, dsiMode);
        add("Wi-Fi", Settings::wifi, toggle);
        int dnsIndex = -1;
        if (Settings::wifi) {
            // The DNS server only matters with Wi-Fi on
            dnsIndex = settings.size();
            settings.push_back(MenuItem("DNS Server", Settings::wfcDns));
            values.push_back({ nullptr, 0 });
        }

        header("About");
        settings.push_back(MenuItem("About NooDS U"));
        values.push_back({ nullptr, 0 });

        // Create the settings menu
        uint32_t pressed = menu("Settings", settings, index, "Controls");

        // Handle menu input
        if (pressed & defaultKeys[INPUT_A]) {
            // Change the chosen setting to its next value
            int *value = values[index].first;
            if (value) {
                *value = (*value + 1) % values[index].second;
            }
            else if (index == nicknameIndex) {
                // Edit the nickname with the system keyboard, following the Mii name if left empty or matching it
                std::u16string text = nickname.empty() ? miiName : utf8To16(nickname);
                if (editText(text, 10, cafeLanguage()))
                    nickname = (text.empty() || text == miiName) ? "" : utf16To8(text);
            }
            else if (index == dnsIndex) {
                // Edit the DNS server's address with the number pad, keeping the old one if the new one isn't valid
                // and going back to the default if left empty
                std::u16string text = utf8To16(Settings::wfcDns);
                if (editText(text, 15, cafeLanguage(), true)) {
                    std::string address = utf16To8(text);
                    unsigned int parts[4];
                    char end;
                    if (address.empty())
                        Settings::wfcDns = Settings::defaultWfcDns;
                    else if (sscanf(address.c_str(), "%u.%u.%u.%u%c", &parts[0], &parts[1], &parts[2], &parts[3],
                            &end) == 4 && parts[0] < 256 && parts[1] < 256 && parts[2] < 256 && parts[3] < 256)
                        Settings::wfcDns = std::to_string(parts[0]) + "." + std::to_string(parts[1]) + "." +
                            std::to_string(parts[2]) + "." + std::to_string(parts[3]);
                }
            }
            else {
                aboutMenu();
            }

            // Update the palette when changing themes
            palette = &themeColors[menuTheme * 7];
        }
        else if (pressed & defaultKeys[INPUT_B]) {
            // Close the settings menu
            changed = true;
            Settings::save();
            return;
        }
        else if (pressed & defaultKeys[INPUT_X]) {
            // Open the controls menu
            controlsMenu();
        }
    }
}

void ConsoleUI::aboutMenu() {
    // List who made this and what it's based on, staying until backing out
    std::vector<MenuItem> items = {
        MenuItem("NooDS U", true),
        MenuItem("Version", NOODSU_VERSION),
        MenuItem("Based on", "NooDS by Hydr8gon"),
        MenuItem("License", "GNU GPL v3 or later"),
        MenuItem("Authors", true),
        MenuItem("michyprima", "NooDS U"),
        MenuItem("Hydr8gon", "NooDS"),
        MenuItem("NooDS Contributors", true),
        MenuItem("Nadia Holmquist Pedersen"),
        MenuItem("Kaua Azevedo"),
        MenuItem("Adrian Siekierka"),
        MenuItem("Belle Aerni"),
        MenuItem("Gran PC"),
        MenuItem("Matteo Rizzo"),
        MenuItem("Built With", true),
        MenuItem("Wii U toolchain", "devkitPro and wut"),
        MenuItem("Audio", "SDL2"),
        MenuItem("Wi-Fi Network", "libslirp (BSD license)"),
        MenuItem("References", true),
        MenuItem("GBATEK", "DS and GBA hardware"),
        MenuItem("melonDS Blog", "3D quirks"),
        MenuItem("melonDS", "Wi-Fi access point"),
        MenuItem("DraStic BIOS", "DS HLE BIOS"),
        MenuItem("Cult of GBA BIOS", "GBA HLE BIOS"),
        MenuItem("Links", true),
        MenuItem("NooDS U", "github.com/michyprima/NooDSU"),
        MenuItem("NooDS", "github.com/Hydr8gon/NooDS")
    };
    int index = 0;
    while (!(menu("About", items, index, "", "", false) & defaultKeys[INPUT_B]));
}

void ConsoleUI::controlsMenu() {
    int index = 0;
    while (true) {
        // Define names for the bindable inputs
        const char *names[] = {
            "A Button", "B Button", "Select Button", "Start Button",
            "Right Button", "Left Button", "Up Button", "Down Button",
            "R Button", "L Button", "X Button", "Y Button", "Menu Button",
            "Fast Forward Hold", "Fast Forward Toggle", "Screen Swap Toggle", "Blow into Mic", "Pointer Touch"
        };

        // Build strings for the input bindings
        std::string bindings[INPUT_MAX];
        for (int i = 0; i < INPUT_MAX; i++) {
            // Add the input name with a comma (up to 8 entries)
            for (int j = 0, k = -1; j < 32 && k < 8; j++) {
                if (!(keyBinds[i] & (1 << j))) continue;
                if (bindings[i] != "") bindings[i] += ", ";
                bindings[i] += (++k < 8) ? keyNames[j] : "...";
            }

            // Replace empty strings with the word none
            if (bindings[i] == "")
                bindings[i] = "None";
        }

        // Create a list of inputs and current bindings
        std::vector<MenuItem> controls = {
            MenuItem("Buttons", true),
            MenuItem(names[INPUT_A], bindings[INPUT_A]),
            MenuItem(names[INPUT_B], bindings[INPUT_B]),
            MenuItem(names[INPUT_SELECT], bindings[INPUT_SELECT]),
            MenuItem(names[INPUT_START], bindings[INPUT_START]),
            MenuItem(names[INPUT_RIGHT], bindings[INPUT_RIGHT]),
            MenuItem(names[INPUT_LEFT], bindings[INPUT_LEFT]),
            MenuItem(names[INPUT_UP], bindings[INPUT_UP]),
            MenuItem(names[INPUT_DOWN], bindings[INPUT_DOWN]),
            MenuItem(names[INPUT_R], bindings[INPUT_R]),
            MenuItem(names[INPUT_L], bindings[INPUT_L]),
            MenuItem(names[INPUT_X], bindings[INPUT_X]),
            MenuItem(names[INPUT_Y], bindings[INPUT_Y]),
            MenuItem("Hotkeys", true),
            MenuItem(names[INPUT_MENU], bindings[INPUT_MENU]),
            MenuItem(names[INPUT_FAST_HOLD], bindings[INPUT_FAST_HOLD]),
            MenuItem(names[INPUT_FAST_TOGG], bindings[INPUT_FAST_TOGG]),
            MenuItem(names[INPUT_SCRN_SWAP], bindings[INPUT_SCRN_SWAP]),
            MenuItem(names[INPUT_MIC_BLOW], bindings[INPUT_MIC_BLOW]),
            MenuItem(names[INPUT_POINTER_TOUCH], bindings[INPUT_POINTER_TOUCH])
        };

        // Create the controls menu
        uint32_t pressed = menu("Controls", controls, index, "Clear");
        int i = index - ((index > 13) ? 2 : 1);

        // Handle menu input
        if (pressed & defaultKeys[INPUT_A]) {
            // Show a binding message and bind the pressed input
            keyBinds[i] |= message(std::string("Remap ") + names[i],
               "Press an input to add it as a binding.", 2);
        }
        else if (pressed & defaultKeys[INPUT_B]) {
            // Close the controls menu
            return;
        }
        else if (pressed & defaultKeys[INPUT_X]) {
            // Clear an input binding
            keyBinds[i] = 0;
        }
    }
}

void ConsoleUI::pauseMenu() {
    // Pause the emulator
    stopCore();

    // Define the pause menu list
    std::vector<MenuItem> items = {
        MenuItem("Resume"),
        MenuItem("Restart"),
        MenuItem("File Browser"),
        MenuItem("Save State"),
        MenuItem("Load State")
    };
#if defined(PROFILE_EMU) || defined(PROFILE_SAMPLER)
    items.push_back(MenuItem("3D Benchmark"));
#endif
#ifdef PROFILE_SAMPLER
    items.push_back(MenuItem("CPU Samples"));
#endif

    int index = 0;
    while (true) {
        // Create the pause menu
        uint32_t pressed = menu("NooDS U", items, index, "Settings", "Exit");

        // Handle menu input
        if (pressed & defaultKeys[INPUT_A]) {
            // Handle the selected item
            switch (index) {
            case 0: // Resume
                // Return to the emulator
                startCore();
                return;

            case 1: // Restart
                // Restart and return to the emulator if confirmed
                if (!message("Restart", "Do you want to restart the game? Any unsaved progress will be lost.", 1))
                    break;
                if (createCore()) startCore();
                else fileBrowser();
                return;

            case 2: // File Browser
                // Open the file browser and close the pause menu, unless going back to it
                if (fileBrowser()) return;
                break;

            case 3: // Save State
                // Show a confirmation message, with extra information if a state file doesn't exist yet
                if (!message("Save State", (core->saveStates.checkState() == STATE_FILE_FAIL) ? "Saving and "
                    "loading states is dangerous and can lead to data loss.\nStates are also not guaranteed to "
                    "be compatible across emulator versions.\nPlease rely on in-game saving to keep your progress, "
                    "and back up .sav files\nbefore using this feature. Do you want to save the current state?" :
                    "Do you want to overwrite the saved state with the current state? This can't be undone!", 1))
                    break;

                // Save the state and return to emulation if confirmed
#ifdef STATE_DEBUG
                debugWrite(core->debugSnapshot("before saving a state"));
#endif
                core->saveStates.saveState();
                startCore();
                return;

            case 4: { // Load State
                // Show a confirmation message, or an error if something went wrong
                bool error = true;
                std::string title, text;
                switch (core->saveStates.checkState()) {
                case STATE_SUCCESS:
                    error = false;
                    title = "Load State";
                    text = "Do you want to load the saved state and "
                        "lose the current state? This can't be undone!";
                    break;

                case STATE_FILE_FAIL:
                    title = "Error";
                    text = "The state file doesn't exist or couldn't be opened.";
                    break;

                case STATE_FORMAT_FAIL:
                    title = "Error";
                    text = "The state file doesn't have a valid format.";
                    break;

                case STATE_VERSION_FAIL:
                    title = "Error";
                    text = "The state file isn't compatible with this version of NooDS U.";
                    break;
                }

                // Load the state and return to emulation if confirmed
                if (!message(title, text, !error) || error) break;
                core->saveStates.loadState();
#ifdef STATE_DEBUG
                debugWrite(core->debugSnapshot("right after loading a state"));
                debugAfterLoad.store(600);
#endif
                startCore();
                return;
            }

#if defined(PROFILE_EMU) || defined(PROFILE_SAMPLER)
            case 5: { // 3D Benchmark
                // Re-render the paused frame's 3D with one thread on core 1, which nothing else uses while paused
                // With the sampler, it records where rendering spends its time, shown under CPU Samples afterwards
                struct Bench { Core *core; double ms; uint64_t px[PX_COUNT]; } bench = { core };
                WorkerThread::Entry entry = [](int argc, void *argv) -> int {
                    Bench *b = (Bench*)argv;
                    int saved = Settings::threaded3D;
                    Settings::threaded3D = 0;
#ifdef PROFILE_EMU
                    uint64_t before[PX_COUNT];
                    memcpy(before, EmuProfile::pixelTicks[1], sizeof(before));
#endif
#ifdef PROFILE_SAMPLER
                    Sampler::start();
                    const int frames = 100;
#else
                    const int frames = 20;
#endif
                    OSTime start = OSGetSystemTime();
                    for (int n = 0; n < frames; n++)
                        for (int line = 0; line < 192; line++)
                            b->core->gpu3DRenderer.drawScanline(line);
                    b->ms = OSTicksToMicroseconds(OSGetSystemTime() - start) / (frames * 1000.0);
#ifdef PROFILE_SAMPLER
                    Sampler::stop();
#endif
#ifdef PROFILE_EMU
                    for (int i = 0; i < PX_COUNT; i++)
                        b->px[i] = (EmuProfile::pixelTicks[1][i] - before[i]) / (frames / 20);
#endif
                    Settings::threaded3D = saved;
                    return 0;
                };
                if (OSThread *thread = WorkerThread::createThread(entry, &bench, 0x10000, 16, OS_THREAD_ATTRIB_AFFINITY_CPU1))
                    OSJoinThread(thread, nullptr);

                // Show the time per frame, and how it splits into drawing phases
                char text[256];
#ifdef PROFILE_EMU
                double ms[PX_COUNT];
                for (int i = 0; i < PX_COUNT; i++)
                    ms[i] = bench.px[i] * 1000.0 / OSTimerClockSpeed / 20;
                snprintf(text, sizeof(text), "%.2f ms per frame with one thread\nclear %.2f, span setup %.2f, pixel depth %.2f\n"
                    "interp %.2f, texel %.2f, write %.2f", bench.ms, ms[PX_CLEAR], ms[PX_SPAN], ms[PX_DEPTH], ms[PX_INTERP],
                    ms[PX_TEXEL], ms[PX_WRITE]);
#else
                snprintf(text, sizeof(text), "%.2f ms per frame with one thread\nSee CPU Samples for where the time went",
                    bench.ms);
#endif
                message("3D Benchmark", text);
                break;
            }
#endif
#ifdef PROFILE_SAMPLER
            case Sampler::MENU_INDEX: { // CPU Samples
                // Show where the emulator's core spent its time since emulation last resumed
                // The details are on a second page, shown after the first is dismissed
                // Both pages are also added to a file, which is easier to read than the screen
                std::string pages[] = { Sampler::report(0), Sampler::report(1) };
                {
                    std::lock_guard<FileLock> fileGuard(fileLock());
                    if (FILE *file = fopen((Settings::basePath + "/samples.txt").c_str(), "a")) {
                        fprintf(file, "%s\n%s\n\n", pages[0].c_str(), pages[1].c_str());
                        fclose(file);
                    }
                }
                if (message("CPU Samples", pages[0]))
                    message("CPU Samples: details", pages[1]);
                break;
            }
#endif
            }
        }
        else if (pressed & defaultKeys[INPUT_B]) {
            // Return to the emulator
            startCore();
            return;
        }
        else if (pressed & defaultKeys[INPUT_START]) {
            // Close the pause menu with the emulator stopped, which ends the main loop
            return;
        }
        else if (pressed & defaultKeys[INPUT_X]) {
            // Open the settings menu
            settingsMenu();
        }
    }
}

int ConsoleUI::launchOptions(bool gba) {
    // Show what a ROM can be launched with, returning the save size to use, or -1 to go back without launching
    std::vector<MenuItem> items = { MenuItem("Run with Save Type...") };
    int index = 0;
    while (true) {
        uint32_t pressed = menu("Launch Options", items, index);
        if (pressed & defaultKeys[INPUT_B])
            return -1;
        if (pressed & defaultKeys[INPUT_A]) {
            int saveSize = saveTypeMenu(gba);
            if (saveSize >= 0) return saveSize;
        }
    }
}

int ConsoleUI::saveTypeMenu(bool gba) {
    // Build a save type list for the ROM's system
    static const std::vector<std::pair<const char*, int>> gbaTypes = {
        { "None", 0x00000 }, { "EEPROM 0.5KB", 0x00200 }, { "EEPROM 8KB", 0x02000 },
        { "SRAM 32KB", 0x08000 }, { "FLASH 64KB", 0x10000 }, { "FLASH 128KB", 0x20000 }
    };
    static const std::vector<std::pair<const char*, int>> ndsTypes = {
        { "None", 0x000000 }, { "EEPROM 0.5KB", 0x000200 }, { "EEPROM 8KB", 0x002000 }, { "EEPROM 64KB", 0x010000 },
        { "EEPROM 128KB", 0x020000 }, { "FRAM 32KB", 0x008000 }, { "FLASH 256KB", 0x040000 },
        { "FLASH 512KB", 0x080000 }, { "FLASH 1024KB", 0x100000 }, { "FLASH 8192KB", 0x800000 }
    };
    const std::vector<std::pair<const char*, int>> &types = gba ? gbaTypes : ndsTypes;
    std::vector<MenuItem> items;
    for (size_t i = 0; i < types.size(); i++)
        items.push_back(MenuItem(types[i].first));

    int index = 0;
    while (true) {
        // Create the save type menu
        uint32_t pressed = menu("Save Type", items, index);

        // Handle menu input
        if (pressed & defaultKeys[INPUT_A]) {
            // Confirm the change because doing this accidentally could be bad, since it resizes the save file for good
            if (message("Changing Save Type", "Are you sure? This may result in data loss!", 1))
                return types[index].second;
        }
        else if (pressed & defaultKeys[INPUT_B]) {
            // Close the save type menu
            return -1;
        }
    }
}

#ifdef JIT_SELFTEST
void ConsoleUI::jitSelfTest() {
    // Run the interpreter and the JIT side by side with settings that keep them deterministic
    // Frames are skipped so most of the time goes to CPU emulation, which is what's being compared
    // The user's settings are restored afterwards, so they aren't saved with the test values
    // The test uses the current CPU timing; compiled code checks for events after every instruction while testing,
    // like the interpreter, since fast timing would otherwise let it check less often
    int saved[] = { Settings::threaded2D, Settings::threaded3D, Settings::emulateAudio, Settings::frameskip, Settings::jit,
        Settings::hardware3D };
    Jit::testing = true;
    Settings::threaded2D = 0;
    Settings::threaded3D = 0;
    Settings::hardware3D = 0;
    Settings::emulateAudio = 0;
    Settings::frameskip = 5;
    Rtc::fixedTime = std::time(nullptr); // Both cores must read the same clock
    Core *cores[2] = {};
    try {
        Settings::jit = 0;
        cores[0] = new Core(ndsPath);
        Settings::jit = 1;
        cores[1] = new Core(ndsPath);
    }
    catch (CoreError e) {
        delete cores[0];
        cores[0] = cores[1] = nullptr;
    }
    auto restore = [&]() {
        Settings::threaded2D = saved[0];
        Settings::threaded3D = saved[1];
        Settings::emulateAudio = saved[2];
        Settings::frameskip = saved[3];
        Settings::jit = saved[4];
        Settings::hardware3D = saved[5];
        Jit::testing = false;
        Rtc::fixedTime = 0;
    };
    if (!cores[1]) {
        restore();
        message("JIT Self-Test", "Couldn't load the game.");
        return;
    }
    Core &a = *cores[0], &b = *cores[1];
    std::string mode;

    // Note the start in the results file, so a test that never finished can be told apart from an old result
    std::string resultsPath = Settings::basePath + "/jittest.txt";
    {
        std::lock_guard<FileLock> fileGuard(fileLock());
        if (FILE *file = fopen(resultsPath.c_str(), "a")) {
            fprintf(file, "Started %s\n", ndsPath.c_str());
            fclose(file);
        }
    }

    // Compare CPU and scheduler state every time the cores stop, and the screens of every drawn frame
    static uint32_t frameA[256 * 192 * 8], frameB[256 * 192 * 8];
    const int targetFrames = 15000;
    std::string result;
    double timeA = 0, timeB = 0;
    long steps = 0;
    std::atomic<long> frames(0);
    std::atomic<bool> stop(false), done(false);

    // Run the comparison on its own thread, which has to be on core 1 on Wii U to use generated code
    std::function<void()> work = [&]() {
        while (result == "" && !stop.load()) {
            auto t0 = std::chrono::steady_clock::now();
            a.runCore();
            auto t1 = std::chrono::steady_clock::now();
            b.runCore();
            auto t2 = std::chrono::steady_clock::now();
            timeA += std::chrono::duration<double>(t1 - t0).count();
            timeB += std::chrono::duration<double>(t2 - t1).count();
            steps++;

            bool readyA = a.gpu.getFrame(frameA, false);
            bool readyB = b.gpu.getFrame(frameB, false);
            if (readyA != readyB)
                result = "Frame timing differs";
            else if (readyA && memcmp(frameA, frameB, 256 * 192 * 2 * sizeof(uint32_t)))
                result = "Screens differ";
            else if (a.globalCycles != b.globalCycles)
                result = "Global cycles " + std::to_string(a.globalCycles) + " vs " + std::to_string(b.globalCycles);
            else if (a.events.size() != b.events.size())
                result = "Scheduler event count differs";
            if (readyA) frames += 6;
            for (size_t i = 0; result == "" && i < a.events.size(); i++)
                if (a.events[i].task != b.events[i].task || a.events[i].cycles != b.events[i].cycles)
                    result = "Scheduler event " + std::to_string(i) + " differs";
            for (int i = 0; result == "" && i < 2; i++) {
                std::string diff = a.interpreter[i].compare(b.interpreter[i]);
                if (diff != "") result = std::string(i ? "ARM7 " : "ARM9 ") + diff;
            }
            if (result != "") {
                char pcs[96];
                snprintf(pcs, sizeof(pcs), "\nARM9 PC %08X/%08X, ARM7 PC %08X/%08X", a.interpreter[0].getPC(),
                    b.interpreter[0].getPC(), a.interpreter[1].getPC(), b.interpreter[1].getPC());
                result = "MISMATCH after " + std::to_string(steps) + " stops, around frame " +
                    std::to_string(frames.load()) + ":\n" + result + pcs;
            }
            else if (frames.load() >= targetFrames) {
                result = "PASSED";
            }
        }
        if (result == "") result = "Stopped early";
        done.store(true);
    };
    WorkerThread::Entry entry = [](int argc, void *argv) -> int {
        (*(std::function<void()>*)argv)();
        return 0;
    };
    OSThread *testThread = WorkerThread::createThread(entry, &work, 0x100000,
        OSGetThreadPriority(OSGetCurrentThread()), OS_THREAD_ATTRIB_AFFINITY_CPU1);

    // Show progress twice a second, and stop if B is pressed
    while (!done.load()) {
        if (getInputPress() & defaultKeys[INPUT_B])
            stop.store(true);
        mode = std::string(!b.jit.enabled ? "JIT off" : (b.jit.isNative() ? "native code" : "portable executor")) +
            (Settings::fastTiming ? ", fast timing" : "");
        startFrame(palette[0]);
        drawString("JIT Self-Test (" + mode + ")", SCALE(72), SCALE(30), SCALE(42), palette[1]);
        drawString("Frame " + std::to_string(frames.load()) + " of " + std::to_string(targetFrames) +
            ", no differences so far", SCALE(90), SCALE(124), SCALE(38), palette[1]);
        drawString("Interpreter: " + std::to_string(int(timeA)) + " s, JIT: " + std::to_string(int(timeB)) + " s",
            SCALE(90), SCALE(162), SCALE(38), palette[1]);
        drawString("Press B to stop", SCALE(90), SCALE(238), SCALE(38), palette[1]);
        endFrame();
    }
    OSJoinThread(testThread, nullptr);
    mode = std::string(!b.jit.enabled ? "JIT off" : (b.jit.isNative() ? "native code" : "portable executor")) +
        (Settings::fastTiming ? ", fast timing" : "");

    // Show the result with the time each side took
    char times[128];
    snprintf(times, sizeof(times), "\n\nInterpreter: %.1f s\nJIT: %.1f s (%.0f%% of the interpreter's time)",
        timeA, timeB, timeA > 0 ? timeB * 100 / timeA : 0);
    {
        std::lock_guard<FileLock> fileGuard(fileLock());
        if (FILE *file = fopen(resultsPath.c_str(), "a")) {
            fprintf(file, "%s, %ld frames: %s%s\n\n", mode.c_str(), frames.load(), result.c_str(), times);
            fclose(file);
        }
    }
    delete cores[1];
    delete cores[0];
    restore();
    message("JIT Self-Test (" + mode + ")", result + times);
}
#endif

#ifdef ROM_TEST
void ConsoleUI::romTest() {
    // Run the ROM with the interpreter, then the JIT with each kind of CPU timing, with nothing else taking time
    // The ROM writes its results and times to one file, and what differs is found from that afterwards
    // The user's settings are restored afterwards, so they aren't saved with the test values
    int saved[] = { Settings::threaded2D, Settings::threaded3D, Settings::emulateAudio, Settings::frameskip, Settings::jit,
        Settings::hardware3D, Settings::fastTiming, Settings::directBoot, Settings::fpsLimiter };
    Settings::threaded2D = 0;
    Settings::threaded3D = 0;
    Settings::hardware3D = 0;
    Settings::emulateAudio = 0;
    Settings::frameskip = 5;
    Settings::directBoot = 1;
    Settings::fpsLimiter = 0;
    RomTest::path = Settings::basePath + "/romtest.txt";
    {
        std::lock_guard<FileLock> fileGuard(fileLock());
        remove(RomTest::path.c_str());
    }

    struct Run { const char *name; int jit, fast; } runs[] = { { "interpreter", 0, 0 }, { "jit fast", 1, 1 }, { "jit", 1, 0 } };
    std::string result;
    for (Run &run: runs) {
        Settings::jit = run.jit;
        Settings::fastTiming = run.fast;
        {
            std::lock_guard<FileLock> fileGuard(fileLock());
            if (FILE *file = fopen(RomTest::path.c_str(), "a")) {
                fprintf(file, "RUN %s\n", run.name);
                fclose(file);
            }
        }
        Core *core = nullptr;
        try {
            core = new Core(ndsPath);
        }
        catch (CoreError e) {
            result = "Couldn't load the ROM.";
            break;
        }
        RomTest::done = false;
        RomTest::progress = 0;
        std::atomic<bool> stop(false), finished(false);
        std::string mode = run.jit ? (core->jit.isNative() ? "native code" : "portable executor") : "interpreter";
        std::function<void()> work = [&]() {
            while (!RomTest::done && !stop.load())
                core->runCore();
            finished.store(true);
        };
        WorkerThread::Entry entry = [](int argc, void *argv) -> int {
            (*(std::function<void()>*)argv)();
            return 0;
        };
        OSThread *thread = WorkerThread::createThread(entry, &work, 0x100000,
            OSGetThreadPriority(OSGetCurrentThread()), OS_THREAD_ATTRIB_AFFINITY_CPU1);

        // Show progress, and stop if B is pressed or nothing moves for a long time, which would mean a hang
        auto moved = std::chrono::steady_clock::now();
        uint32_t last = 0;
        bool hung = false;
        while (!finished.load()) {
            if (getInputPress() & defaultKeys[INPUT_B])
                stop.store(true);
            if (RomTest::progress != last) {
                last = RomTest::progress;
                moved = std::chrono::steady_clock::now();
            }
            else if (std::chrono::steady_clock::now() - moved > std::chrono::seconds(60)) {
                hung = true;
                stop.store(true);
            }
            startFrame(palette[0]);
            drawString("ROM Test: " + std::string(run.name) + " (" + mode + ")", SCALE(72), SCALE(30), SCALE(42), palette[1]);
            drawString("Case " + std::to_string(last), SCALE(90), SCALE(124), SCALE(38), palette[1]);
            drawString("Press B to stop", SCALE(90), SCALE(238), SCALE(38), palette[1]);
            endFrame();
        }
        OSJoinThread(thread, nullptr);
        bool stopped = !RomTest::done;
        delete core;
        if (stopped) {
            result = hung ? (std::string("Stuck at case ") + std::to_string(last) + " in " + run.name) : "Stopped early";
            {
                std::lock_guard<FileLock> fileGuard(fileLock());
                if (FILE *file = fopen(RomTest::path.c_str(), "a")) {
                    fprintf(file, "STOPPED %s\n", result.c_str());
                    fclose(file);
                }
            }
            break;
        }
    }

    Settings::threaded2D = saved[0];
    Settings::threaded3D = saved[1];
    Settings::emulateAudio = saved[2];
    Settings::frameskip = saved[3];
    Settings::jit = saved[4];
    Settings::hardware3D = saved[5];
    Settings::fastTiming = saved[6];
    Settings::directBoot = saved[7];
    Settings::fpsLimiter = saved[8];

    // Compare what each run's cases left against the interpreter's, in the order they ran
    if (result == "") {
        std::vector<std::string> lines[3];
        char line[256];
        int run = -1, total = 0, differ[3] = {};
        {
            std::lock_guard<FileLock> fileGuard(fileLock());
            if (FILE *file = fopen(RomTest::path.c_str(), "r")) {
                while (fgets(line, sizeof(line), file)) {
                    if (!strncmp(line, "RUN ", 4)) run++;
                    else if (line[0] == 'C' && run >= 0 && run < 3) lines[run].push_back(line);
                }
                fclose(file);
            }
        }
        total = lines[0].size();
        for (int i = 1; i < 3; i++) {
            for (size_t j = 0; j < lines[0].size(); j++)
                if (j >= lines[i].size() || lines[i][j] != lines[0][j]) differ[i]++;
        }
        result = std::to_string(total) + " cases\nFast timing JIT: " + std::to_string(differ[1]) + " differ\nJIT: " +
            std::to_string(differ[2]) + " differ\n\nResults and times are in romtest.txt";
    }
    message("ROM Test", result);
}
#endif

static Language systemLanguage() {
    // Fall back to English for languages the DS doesn't have
    uint32_t value = cafeLanguage();
    return (value <= LG_SPANISH) ? Language(value) : LG_ENGLISH;
}

bool ConsoleUI::createCore() {
    // Choose the language and nickname for games to see
    // If the Mii name can't be read, a firmware dump's own name is kept
    Spi::setLanguage(gameLanguage ? Language(gameLanguage - 1) : systemLanguage());
    Spi::setNickname(nickname.empty() ? getMiiName() : utf8To16(nickname));

    try {
        // Let a new core reuse ROMs that are already in memory, instead of reading them from file again
        if (core) {
            core->cartridgeNds.keepRom();
            core->cartridgeGba.keepRom();
            delete core;
        }
        core = nullptr;
        Cartridge::dropKeptRoms(ndsPath, gbaPath);

        // Attempt to create the core
        if (Settings::romInRam == 1) {
            // Create the core on another thread when the whole ROM is loaded first, showing how far along it is
            std::atomic<bool> done(false);
            Cartridge::cancelLoad.store(false);
            Core *created = nullptr;
            CoreError error;
            bool failed = false;
            WorkerThread loader([&] {
                try {
                    created = new Core(ndsPath, gbaPath);
                }
                catch (CoreError e) {
                    error = e;
                    failed = true;
                }
                done.store(true);
            });
            while (!done.load()) {
                startFrame(palette[0]);
                int percent = Cartridge::loadPercent.load();
                if (percent >= 0) {
                    drawString("Loading ROM... " + std::to_string(percent) + "%", SCALE(72), SCALE(330), SCALE(42), palette[1]);
                    drawString("\x81 Cancel", SCALE(1218), SCALE(667), SCALE(34), palette[1], true);
                }
                endFrame();
                if (getInputPress() & defaultKeys[INPUT_B])
                    Cartridge::cancelLoad.store(true);
            }
            loader.join();
            if (failed) throw error;
            core = created;
        }
        else {
            core = new Core(ndsPath, gbaPath);
        }
        return true;
    }
    catch (CoreError e) {
        // Inform the user of an error if loading wasn't successful
        std::string text;
        switch (e) {
        case ERROR_NDS_BIOS: // Missing NDS BIOS files
            text = "Make sure the path settings point to valid NDS BIOS files, or boot ROMs directly.\n"
                "You can modify path settings in the noodsu.ini file.";
            message("Error Loading NDS BIOS", text);
            break;
        case ERROR_NDS_FIRM: // Non-bootable firmware file
            text = "Make sure the path settings point to a firmware file dumped from a pre-DSi console,"
                " or boot ROMs directly.\nYou can modify path settings in the noodsu.ini file.";
            message("Error Loading NDS Firmware", text);
            break;
        case ERROR_DSI_BIOS: // Missing DSi BIOS files
            text = "Make sure the path settings point to valid DSi BIOS files, or turn off DSi mode.\n"
                "You can modify path settings in the noodsu.ini file.";
            message("Error Loading DSi BIOS", text);
                break;
        case ERROR_DSI_FIRM: // Invalid DSi firmware file
            text = "Make sure the path settings point to a firmware file dumped from a DSi console,"
                " or turn off DSi mode.\nYou can modify path settings in the noodsu.ini file.";
            message("Error Loading DSi Firmware", text);
            break;
        case ERROR_DSI_NAND: // Missing DSi NAND file
            text = "Make sure the path settings point to a valid DSi NAND file, or turn off DSi mode.\n"
                "You can modify path settings in the noodsu.ini file.";
            message("Error Loading DSi NAND", text);
            break;
        case ERROR_ROM: // Unreadable ROM file
            text = "Make sure the ROM file is accessible and try again.";
            message("Error Loading ROM", text);
            break;
        case ERROR_CANCELLED: // Loading cancelled, which needs no message
            break;
        }

        // Throw out the incomplete core
        core = nullptr;
        return false;
    }
}

void ConsoleUI::startCore() {
    // Tell the threads to run if stopped
    if (running) return;
    running = true;

    // Generated code only runs on core 1, and running threads can't be moved between cores, so create
    // the emulator thread there
    // The UI and helper threads stay off core 1, so the emulator has it to itself with or without the JIT
    // Only stay below the UI's priority if it's on the same core
    int priority = OSGetThreadPriority(OSGetCurrentThread());
    if (OSGetCoreId() == 1) priority = std::min(priority + 1, 31);
#ifdef PROFILE_SAMPLER
    WorkerThread::Entry entry = [](int argc, void *argv) -> int {
        Sampler::start();
        runCore();
        Sampler::stop();
        return 0;
    };
#elif defined(PROFILE_EMU) && defined(HEARTBEAT)
    WorkerThread::Entry entry = [](int argc, void *argv) -> int {
        FrameTrace::start();
        runCore();
        FrameTrace::stop();
        return 0;
    };
#else
    WorkerThread::Entry entry = [](int argc, void *argv) -> int { runCore(); return 0; };
#endif
    if ((emuThread = WorkerThread::createThread(entry, nullptr, 0x100000, priority, OS_THREAD_ATTRIB_AFFINITY_CPU1))) {
        saveThread = new WorkerThread(WorkerThread::CORE_2, checkSave);
        return;
    }

    // Start the threads
    coreThread = new std::thread(runCore);
    saveThread = new WorkerThread(WorkerThread::CORE_2, checkSave);
}

void ConsoleUI::closeCore() {
    // Stop and delete the core before the app exits, so its threads end, its save data is written and its files and
    // sockets are closed; the system can wait forever on what's left running otherwise
    stopCore();
    delete core;
    core = nullptr;
}

void ConsoleUI::stopNetwork() {
    // End the network thread of a stopped core, for exits where the rest of the core can't be deleted since threads
    // on cores 0 and 1 are suspended in the background
    if (core) core->wifi.stopNetwork();
}

void ConsoleUI::stopCore() {
    // Tell the threads to stop if running
    if (running) {
        std::lock_guard<std::mutex> guard(mutex);
        running = false;
        cond.notify_one();
    }
    else return;

    // Wait for the threads to stop
    if (emuThread) {
        OSJoinThread(emuThread, nullptr);
        emuThread = nullptr;
    }
    else
    {
        coreThread->join();
        delete coreThread;
    }
    saveThread->join();
    delete saveThread;
}


void ConsoleUI::runCore() {
    // Run the emulator, waiting for the display after each frame when syncing to it
#ifdef FRAME_LOG
    FrameLog::emulatorStarted();
#endif
    while (running) {
        core->runCore();
        if (core->frameEnded.exchange(false)) {
#ifdef STATE_DEBUG
            debugFrameEnd(core);
#endif
            EP_START(display);
#ifdef FRAME_LOG
            uint32_t finished = OSGetSystemTick();
#endif
            waitForDisplay();
#ifdef FRAME_LOG
            FrameLog::emulatorFrame(finished, core->gpu3D.polygonCountOut);
#endif
            EP_ADD(EP_DISPLAY, display);
#if defined(PROFILE_EMU) && defined(HEARTBEAT)
            EmuProfile::recordFrame(core->gpu.frameNumber, core->spu.ringLevel(), core->spu.ringCatching());
#endif
        }
    }
}

void ConsoleUI::displayTick() {
    // Let the emulator run another frame, saving up at most 2, so it doesn't run ahead after falling behind
    std::lock_guard<std::mutex> guard(displayMutex);
    if (displayTicks < 2) displayTicks++;
    displayCond.notify_one();
}

void ConsoleUI::waitForDisplay() {
    // Wait for the next display refresh when syncing to it and not fast-forwarding, giving up after a while in case
    // refreshes stop being reported
    if (!Settings::syncToDisplay || !Settings::fpsLimiter) return;
    std::unique_lock<std::mutex> lock(displayMutex);

    // Don't wait if slow frames left the audio buffer low, so the emulator makes up the time instead of the buffer
    // staying low for the next scene change
    if (core->spu.ringLow()) {
        displayTicks = 0;
        return;
    }
    displayCond.wait_for(lock, std::chrono::milliseconds(100), [] { return displayTicks > 0; });
    if (displayTicks > 0) displayTicks--;
}

void ConsoleUI::checkSave() {
    while (running) {
        // Check save files every few seconds and update them if changed
        std::unique_lock<std::mutex> lock(mutex);
        cond.wait_for(lock, std::chrono::seconds(3), [&]{ return !running; });
        core->cartridgeNds.writeSave();
        core->cartridgeGba.writeSave();
        core->spi.writeWfcSettings();
    }
}
