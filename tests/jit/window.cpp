// Shows both screens in a window as a run goes, for game.sh's --window; only built and linked with it, since it needs SDL2
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <cstdint>

extern bool (*windowOpen)(const char *title, int position);
extern void (*windowShow)(const uint32_t *frame);

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;

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

static void show(const uint32_t *frame) {
    // Frames have red in the lowest byte; closing the window only stops showing it, and the run carries on
    // At most 60 a second are shown, since showing one can wait for the display, and runs are often faster than that
    static uint32_t last;
    if (SDL_GetTicks() - last < 16) return;
    last = SDL_GetTicks();
    SDL_Event event;
    while (window && SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT || (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE)) {
            SDL_DestroyWindow(window);
            window = nullptr;
        }
    }
    if (!window) return;
    SDL_UpdateTexture(texture, nullptr, frame, 256 * 4);
    SDL_RenderCopy(renderer, texture, nullptr, nullptr);
    SDL_RenderPresent(renderer);
}

static struct Register {
    Register() { windowOpen = open; windowShow = show; }
} registered;
