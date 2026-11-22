/* Trap Gunner dual screen: the second window itself.
 *
 * Compiled into an overlaid copy of psxrecomp's runtime main.cpp
 * (cmake/tg_framework_overlay.cmake); the submodule itself is untouched.
 *
 * While the "Dual Screen" mod is on (src/mods/tg_dual_screen.cpp), a second
 * game window opens: borderless fullscreen on another monitor when there is
 * one, otherwise a normal window beside the main one. It can be dragged to
 * any monitor; a double click switches it between fullscreen and a window.
 * Closing it puts the game back on one screen until the next start. Closing
 * the main window still quits the game.
 *
 * Everything here runs on the main thread, from the game's event drain. The
 * window is handed to the renderer (tg_dual_gl.inc), which draws into it.
 */
#include <atomic>
#include <cstdio>

extern "C" int gl_renderer_tg_dual_set_window(SDL_Window* win);
extern "C" int tg_dual_screen_wanted(void);   /* the mod is active */

static SDL_Window* s_tg_dual_window = nullptr;
/* 0 closed, 1 open, 2 closed by the player (stays closed this session). */
static std::atomic<int> s_tg_dual_state{0};
static std::atomic<int> s_tg_dual_close_main{0}, s_tg_dual_close_second{0},
    s_tg_dual_toggle_fullscreen{0};

/* For the title plugin: the second window is up, draw both views. */
extern "C" int tg_dual_screen_open(void) { return s_tg_dual_state.load() == 1; }

#if defined(PSX_SDL3)
/* Sees every event whichever loop drains it. With two windows open, closing
 * one no longer sends a quit, so the main window's close is turned into one
 * here (by tg_dual_poll, outside the event queue). */
static bool SDLCALL tg_dual_watch(void*, SDL_Event* ev) {
    if (!s_tg_dual_window || !ev) return true;
    const SDL_WindowID second = SDL_GetWindowID(s_tg_dual_window);
    if (ev->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        if (ev->window.windowID == second)
            s_tg_dual_close_second = 1;
        else if (sdl_window && ev->window.windowID == SDL_GetWindowID(sdl_window))
            s_tg_dual_close_main = 1;
    } else if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev->button.windowID == second &&
               ev->button.button == SDL_BUTTON_LEFT && ev->button.clicks == 2) {
        s_tg_dual_toggle_fullscreen = 1;
    }
    return true;
}

static void tg_dual_show() {
    SDL_ShowWindow(s_tg_dual_window);
    if (sdl_window) SDL_RaiseWindow(sdl_window);   /* keep the keyboard on player 1's window */
    gl_renderer_tg_dual_set_window(s_tg_dual_window);
    psx_mod_set_local_pair_presentation(1);
    s_tg_dual_state = 1;
    std::fprintf(stdout, "psxrecomp: tg dual screen: second window open\n");
}

static void tg_dual_open() {
    const int w = 960, h = 720;
    int count = 0;
    SDL_DisplayID other = 0;
    const SDL_DisplayID mine = SDL_GetDisplayForWindow(sdl_window);
    if (SDL_DisplayID* displays = SDL_GetDisplays(&count)) {
        for (int i = 0; i < count; ++i)
            if (displays[i] != mine) { other = displays[i]; break; }
        SDL_free(displays);
    }
    SDL_Window* win = SDL_CreateWindow("Trap Gunner Recompiled - Player 2", 0, 0, w, h,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                                       SDL_WINDOW_HIDDEN);
    if (!win) {
        std::fprintf(stderr, "psxrecomp: tg dual screen: window failed (%s)\n", SDL_GetError());
        s_tg_dual_state = 2;
        return;
    }
    SDL_Rect b{};
    if (other && SDL_GetDisplayBounds(other, &b)) {
        SDL_SetWindowPosition(win, b.x + (b.w - w) / 2, b.y + (b.h - h) / 2);
        SDL_SetWindowFullscreen(win, true);   /* borderless, that monitor's desktop mode */
    } else {
        int mx = 0, my = 0, mw = 0, mh = 0;
        SDL_GetWindowPosition(sdl_window, &mx, &my);
        SDL_GetWindowSize(sdl_window, &mw, &mh);
        SDL_SetWindowPosition(win, mx + mw + 16, my);
    }
    s_tg_dual_window = win;
    static bool watching = false;
    if (!watching) watching = SDL_AddEventWatch(tg_dual_watch, nullptr);
    tg_dual_show();
}

static void tg_dual_close(int next_state) {
    psx_mod_set_local_pair_presentation(0);
    gl_renderer_tg_dual_set_window(nullptr);
    SDL_HideWindow(s_tg_dual_window);   /* kept, and reused if it opens again */
    s_tg_dual_state = next_state;
    std::fprintf(stdout, "psxrecomp: tg dual screen: second window closed\n");
}
#endif

static void tg_dual_poll() {
#if defined(PSX_SDL3)
    if (s_tg_dual_close_main.exchange(0)) {
        SDL_Event quit{};
        quit.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit);
    }
    const bool wanted = g_gl_active && sdl_window && tg_dual_screen_wanted();
    const int state = s_tg_dual_state.load();
    if (state == 1) {
        if (!wanted) { tg_dual_close(0); return; }
        if (s_tg_dual_close_second.exchange(0)) { tg_dual_close(2); return; }
        if (s_tg_dual_toggle_fullscreen.exchange(0)) {
            const bool fs = (SDL_GetWindowFlags(s_tg_dual_window) & SDL_WINDOW_FULLSCREEN) != 0;
            SDL_SetWindowFullscreen(s_tg_dual_window, !fs);
        }
        return;
    }
    if (!wanted) {
        if (state == 2) s_tg_dual_state = 0;   /* the next session may open it again */
        return;
    }
    if (state == 0) {
        if (s_tg_dual_window) tg_dual_show();
        else tg_dual_open();
    }
#endif
}
