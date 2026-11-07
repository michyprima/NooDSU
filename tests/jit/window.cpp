// Shows both screens in a window as a run goes, for game.sh's --window; only built and linked with it, since it needs SDL2
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <cstdint>

extern bool (*windowOpen)(const char *title, int position);
extern void (*windowShow)(const uint32_t *frame);
extern bool (*windowInput)(int &x, int &y, uint32_t &keys);
extern int (*windowState)();

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static bool quit; // Asked to quit from the menu bar, Cmd+Q or a signal, which ends the run

static bool open(const char *title, int position) {
    // Each run's window goes next to the others, at twice the screens' size
    // Without vsync, so showing frames doesn't slow the run down to the display's rate
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "0");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return false;
    window = SDL_CreateWindow(title, 40 + position * 540, 60, 512, 768, 0);
    renderer = window ? SDL_CreateRenderer(window, -1, 0) : nullptr;
    texture = renderer ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 256, 384) : nullptr;
    if (texture) SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE); // The top byte isn't alpha
    return texture;
}

static void poll() {
    // Handle the window's events, even once it's closed so macOS doesn't take the app for hung
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) quit = true;
        if ((event.type == SDL_QUIT || (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE)) &&
            window) {
            SDL_DestroyWindow(window);
            window = nullptr;
        }
    }
}

static int state() {
    // Whether the window is open (0), closed (1), or quitting was asked for (2)
    return quit ? 2 : window ? 0 : 1;
}

static void show(const uint32_t *frame) {
    // Frames have red in the lowest byte
    // At most 60 a second are shown, since showing one can wait for the display, and runs are often faster than that
    static uint32_t last;
    if (SDL_GetTicks() - last < 16) return;
    last = SDL_GetTicks();
    poll();
    if (!window) return;
    SDL_UpdateTexture(texture, nullptr, frame, 256 * 4);
    SDL_RenderCopy(renderer, texture, nullptr, nullptr);
    SDL_RenderPresent(renderer);
}

static bool input(int &x, int &y, uint32_t &keys) {
    // Give the held keys as KEYINPUT bits, then 10 and 11 for X and Y, and whether the mouse is down on the bottom screen,
    // with its position there; the window is twice the screens' size
    poll();
    keys = 0;
    if (!window || !(SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS)) return false;
    static const SDL_Scancode map[12] = { SDL_SCANCODE_X, SDL_SCANCODE_Z, SDL_SCANCODE_RSHIFT, SDL_SCANCODE_RETURN,
        SDL_SCANCODE_RIGHT, SDL_SCANCODE_LEFT, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_W, SDL_SCANCODE_Q,
        SDL_SCANCODE_S, SDL_SCANCODE_A };
    const uint8_t *state = SDL_GetKeyboardState(nullptr);
    for (int i = 0; i < 12; i++)
        if (state[map[i]]) keys |= 1 << i;
    int mx, my;
    if (!(SDL_GetMouseState(&mx, &my) & SDL_BUTTON(SDL_BUTTON_LEFT)) || my < 384) return false;
    x = mx / 2;
    y = (my - 384) / 2;
    return true;
}

static struct Register {
    Register() { windowOpen = open; windowShow = show; windowInput = input; windowState = state; }
} registered;
