/* Trap Gunner dual screen: the OpenGL side of the second window.
 *
 * Compiled into an overlaid copy of psxrecomp's gpu_gl_renderer.c
 * (cmake/tg_framework_overlay.cmake); the submodule itself is untouched.
 *
 * The framework's offline "local pair" (psx_mod_render_local_pair) draws two
 * views from one game state and normally shows them side by side in one 8:3
 * window. With a second window set, the pair is split instead: the main
 * window shows view 0 (player 1) on its own, the second window view 1
 * (player 2, or the CPU opponent in a 1-player fight). Every other frame
 * (menus, movies, a frame without a fresh pair) is shown whole in the main
 * window and copied, as presented, into the second window: a mirror.
 *
 * The second window is drawn by the composing thread with the composing
 * context itself (made current on the second window, then back), so the
 * presenter's textures, programs and vertex state are all available. Its
 * swap never waits for vsync: two vsynced swaps in a row on one thread would
 * halve the frame rate whenever the monitors are out of phase.
 */
#include <stdio.h>

static SDL_Window *s_tg_dual_win = NULL;
static GLuint s_tg_dual_tex = 0;
static int s_tg_dual_tw = 0, s_tg_dual_th = 0, s_tg_dual_pair = 0, s_tg_dual_ready = 0;

/* Main thread: the second window (NULL: none). */
int gl_renderer_tg_dual_set_window(SDL_Window *win) {
    GL_RT_SYNC("tg_dual_set_window");
    s_tg_dual_win = win;
    s_tg_dual_ready = 0;
    return 1;
}

static int tg_dual_active(void) {
    return s_tg_dual_win != NULL && s_stereo_mode == 2;
}

/* What the second window shows at the next swap. pair: a pair's view (PSX
 * orientation, gamma still to apply, 4:3); otherwise a copy of the main
 * window's presented image (already composed, its own aspect). */
static void tg_dual_source(GLuint tex, int tw, int th, int pair) {
    if (!tex || tw < 1 || th < 1) return;
    s_tg_dual_tex = tex;
    s_tg_dual_tw = tw;
    s_tg_dual_th = th;
    s_tg_dual_pair = pair;
    s_tg_dual_ready = 1;
}

/* Called at the start of every main-window swap. */
static void tg_dual_present(void) {
    GLint vp[4] = {0, 0, 0, 0}, draw_fb = 0, read_fb = 0;
    GLboolean scissor;
    Uint64 flags;
    int ww = 0, wh = 0, lx = 0, ly = 0, lw = 0, lh = 0, pt;
    if (!s_tg_dual_ready) return;
    s_tg_dual_ready = 0;
    if (!s_tg_dual_win || !s_ctx || !s_win || s_pass_active) return;
    flags = (Uint64)SDL_GetWindowFlags(s_tg_dual_win);
    if (flags & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)) return;
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetIntegerv(0x8CA6 /* GL_DRAW_FRAMEBUFFER_BINDING */, &draw_fb);
    glGetIntegerv(0x8CAA /* GL_READ_FRAMEBUFFER_BINDING */, &read_fb);
    scissor = glIsEnabled(GL_SCISSOR_TEST);
    if (SDL_GL_MakeCurrent(s_tg_dual_win, s_ctx) != 0) {
        static int warned;
        if (!warned++)
            fprintf(stdout, "psxrecomp: tg dual screen: second window unavailable (%s)\n",
                    SDL_GetError());
        SDL_GL_MakeCurrent(s_win, s_ctx);
        return;
    }
    /* While the second window is current, framebuffer 0 is its own, not the
     * present thread's slot that the bind redirect would pick. */
    pt = s_pt_on;
    s_pt_on = 0;
    SDL_GL_GetDrawableSize(s_tg_dual_win, &ww, &wh);
    p_glBindFramebuffer_raw(PSXGL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glViewport(0, 0, ww > 0 ? ww : 1, wh > 0 ? wh : 1);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (ww > 0 && wh > 0) {
        if (s_tg_dual_pair)
            letterbox_rect_aspect(ww, wh, 4, 3, &lx, &ly, &lw, &lh);
        else
            letterbox_rect_aspect(ww, wh, s_tg_dual_tw, s_tg_dual_th, &lx, &ly, &lw, &lh);
        present_target_quad(s_tg_dual_tex, (float)s_tg_dual_tw, (float)s_tg_dual_th,
                            0, 0, s_tg_dual_tw, s_tg_dual_th, s_tg_dual_pair ? 0 : 1,
                            lx, ly, lw, lh, s_tg_dual_pair, s_tg_dual_pair, 1);
    }
    SDL_GL_SetSwapInterval(0);
    SDL_GL_SwapWindow(s_tg_dual_win);
    s_pt_on = pt;
    SDL_GL_MakeCurrent(s_win, s_ctx);
    /* Without the present thread this context swaps the main window too:
     * give it back its own vsync mode. */
    if (!pt && SDL_GL_SetSwapInterval(s_swap_interval) != 0 && s_swap_interval < 0)
        SDL_GL_SetSwapInterval(1);
    p_glBindFramebuffer_raw(PSXGL_DRAW_FRAMEBUFFER, (GLuint)draw_fb);
    p_glBindFramebuffer_raw(PSXGL_READ_FRAMEBUFFER, (GLuint)read_fb);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    if (scissor) glEnable(GL_SCISSOR_TEST);
}
