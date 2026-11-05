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

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "../screen_layout.h"
#include "../../core/core.h"
#include "../../core/defines.h"

enum MenuInputs {
    INPUT_A,
    INPUT_B,
    INPUT_SELECT,
    INPUT_START,
    INPUT_RIGHT,
    INPUT_LEFT,
    INPUT_UP,
    INPUT_DOWN,
    INPUT_R,
    INPUT_L,
    INPUT_X,
    INPUT_Y,
    INPUT_MENU,
    INPUT_FAST_HOLD,
    INPUT_FAST_TOGG,
    INPUT_SCRN_SWAP,
    INPUT_MIC_BLOW,
    INPUT_POINTER_TOUCH,
    INPUT_MAX
};

struct MenuTouch {
    bool pressed;
    float x, y;

    MenuTouch(bool pressed, float x, float y):
        pressed(pressed), x(x), y(y) {}
};

struct MenuItem {
    std::string name;
    std::string setting;
    void *iconTex;
    uint8_t iconSize;
    bool header;

    MenuItem(std::string name, std::string setting = "", void *iconTex = nullptr, uint8_t iconSize = 0):
        name(name), setting(setting), iconTex(iconTex), iconSize(iconSize), header(false) {}
    MenuItem(std::string name, const char *setting): MenuItem(name, std::string(setting)) {} // Not the header one
    MenuItem(std::string name, bool header):
        name(name), setting(""), iconTex(nullptr), iconSize(0), header(header) {}
    bool operator<(const MenuItem &item) { return (name < item.name); }
};

class ConsoleUI {
public:
    static Core *core;
    static bool running;
    alignas(4096) static uint32_t framebuffer[256 * 192 * 8];
    static ScreenLayout layout;
    static bool gbaMode;
    static bool screenShift; // Whether the screen textures are upscaled

    // With high-res 3D, the GPU draws the 3D over native screens, so the CPU doesn't have to read the 3D output
    static uint32_t *hiResData; // The high-res 3D frame taken from the core
    static void *hiResTexture; // A texture of that frame, or a blank one when there isn't one
    static void *blankHiRes; // A blank texture for screens without high-res 3D, which shows their native 3D
    static bool hiResTop; // Whether the 3D is on the top screen

    // The GPU builds the screens at twice the size once for each new frame into textures of their own, keeping the last
    // ones, and the UI draws those, averaged with the last ones for ghosting
    static void *composed[2], *composedLast[2];
    static bool screensComposed; // Whether the screens being drawn are those
    static void composeScreens(void *top, void *bottom, void *hiRes, bool hiResTop);
    static void composeGbaScreen(void *gba);

    // With ghosting at native size, frames alternate between two halves of the framebuffer, so the last one is kept as
    // it was, and the GPU averages the two
    static uint32_t *screenFrame; // Where the current frame is
    static uint32_t *lastFrame; // Where the last frame is, or null without ghosting
    static void *drawLastScreen; // The last frame's screen to average with the next one, if any

    static int showFpsCounter;
    static int menuTheme;
    static int frameColor; // The screen frame's color, or 0 for none
    static int gameLanguage; // The language games see, or 0 for the system's
    static std::string nickname; // The name games see in UTF-8, or empty for the Mii's
    static int micInput; // Whether games hear the GamePad's microphone
    static int stickPointer; // Whether the right stick moves a pointer on the touch screen, and when it hides
    static int keyBinds[INPUT_MAX];

    // Data that is defined per-platform
    static uint32_t defaultKeys[INPUT_MAX];
    static const char *keyNames[32];
    static const int micRate; // Microphone samples per second
    static const uint32_t pointerKeys; // The right stick's directions, which the pointer takes from the game

    // Functions that are implemented per-platform
    static void startFrame(uint32_t color);
    static void endFrame();
    static void *createTexture(uint32_t *data, int width, int height);
    static void destroyTexture(void *texture);
    static void releaseTexture(void *texture);
    static void drawFrames(void *tv, void *gamepad);
    static void *createHiResTexture(uint32_t *data);
    static void destroyHiResTexture(void *texture);
    static void drawTexture(void *texture, float tx, float ty, float tw, float th, float x, float y,
        float w, float h, bool filter = true, int rotation = 0, uint32_t color = 0xFFFFFFFF);
    static void drawOverlay(void *texture, float x, float y, float w, float h, bool gamepad);
    static uint32_t getInputHeld();
    static MenuTouch getInputTouch();
    static void getInputStick(float &x, float &y);
    static size_t getMicSamples(int16_t *out, size_t max);
    static void closeMic();
    static std::u16string getMiiName(); // Empty if it can't be read
    static bool editText(std::u16string &text, int maxLength, int language, bool numpad = false); // False if canceled

    static void drawRectangle(float x, float y, float w, float h, uint32_t color = 0xFFFFFFFF);
    static void drawString(std::string string, float x, float y,
        float size, uint32_t color = 0xFFFFFFFF, bool alignRight = false);
    static void fillAudioBuffer(uint32_t *buffer, int count, int rate);
    static void displayTick();
    static void relayout() { changed = true; }
#ifdef DRC_DEBUG
    static std::string displayDebugText();
#endif
    static uint32_t getInputPress();

    static void initialize(int width, int height, std::string root, std::string prefix);
    static void mainLoop(MenuTouch (*specialTouch)() = nullptr, ScreenLayout *touchLayout = nullptr);
    static int setPath(std::string path);
    static bool fileBrowser();
    static void startCore();
    static void stopCore();

private:
    static void *fileTextures[2];
    static void *folderTextures[2];
    static void *fontTexture;
    static void *pointerTexture;

    static const uint32_t *palette;
    static uint32_t uiWidth, uiHeight;
    static uint32_t lineHeight;
    static bool touchMode;

    static std::string ndsPath, gbaPath;
    static std::string basePath, curPath, defaultFolder;
    static bool changed;
    static void *frameTextures[2]; // The screen frame for the TV and the gamepad
    static void updateFrames(ScreenLayout *gamepad);

    // The right stick's pointer, in touch screen pixels, and when it hides
    static float pointerX, pointerY;
    static std::chrono::steady_clock::time_point pointerHide;
    static bool updatePointer(uint32_t held, ScreenLayout *gamepad);

    static std::thread *coreThread;
    static WorkerThread *saveThread;
    static std::condition_variable cond;
    static std::mutex mutex;
    static int fpsLimiterBackup;

    static const uint32_t themeColors[];
    static const uint8_t charWidths[];

    ConsoleUI() {} // Private to prevent instantiation
    static void *bmpToTexture(uint8_t *bmp);
    static int stringWidth(std::string &string);

    static uint32_t menu(std::string title, std::vector<MenuItem> &items,
        int &index, std::string actionX = "", std::string actionPlus = "", bool actionOk = true,
        std::string actionY = "", uint32_t hiddenKeys = 0);
    static uint32_t message(std::string title, std::string text, int type = 0);

#ifdef JIT_SELFTEST
    static void jitSelfTest();
#endif
#ifdef ROM_TEST
    static void romTest();
#endif
    static void settingsMenu();
    static void controlsMenu();
    static void aboutMenu();
    static void pauseMenu();
    static int launchOptions(bool gba);
    static int saveTypeMenu(bool gba);

    static bool createCore();
    static void runCore();

    // When syncing to the display, the emulator runs one frame for each display refresh, which the platform reports
    static std::mutex displayMutex;
    static std::condition_variable displayCond;
    static int displayTicks;
    static void waitForDisplay();
    static void checkSave();
};
