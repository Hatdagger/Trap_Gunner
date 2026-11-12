/* Trap Gunner system menu: a controller-first front end for the launcher.
 *
 * Compiled into recomp-ui's launcher_imgui.cpp by the configure-time overlay
 * (cmake/tg_framework_overlay.cmake); the framework submodule itself is never
 * edited. It owns the launcher's dashboard, settings, mods and netplay views
 * (the network pages are in tg_menu_net.inc) and draws them in the style of
 * the game's own SYSTEM OPTIONS screen: a bronze frame around a static-filled
 * CRT, items in green, the chosen item in blue, values in white, and the page
 * name on a framed plate. D-pad to move, one button to confirm or toggle,
 * one to go back. Everything else (the controller page, online sign-in,
 * every modal) is the stock launcher, reached from here and returned from
 * with its own Back.
 *
 * It draws straight onto the launcher window's draw list on a 640x480 design
 * grid scaled to the window, and reads input through ImGui's key state, so it
 * needs nothing from the platform layer. All game state goes through the
 * launcher model's own functions and the mod provider; this file keeps only
 * cursor positions.
 *
 * Expected from the including file: LauncherModel, LauncherTheme, asset(),
 * ui_pick_file(), request_mod_commit_launch(), mod_note_error(),
 * tg_menu_open_folder(), tg_menu_lobby_popups(), and what tg_menu_net.inc
 * lists.
 */

namespace tg_menu {

enum Page {
    PAGE_MAIN = 0, PAGE_SETTINGS, PAGE_MODS,
    PAGE_NET_MODE, PAGE_NET_ROOMS, PAGE_LOBBY, PAGE__COUNT
};

struct State {
    int    cursor[PAGE__COUNT] = {0};
    int    top[PAGE__COUNT] = {0};
    bool   classic_settings = false;   /* the stock page was asked for */
    bool   classic_mods = false;
    bool   classic_net = false;
    bool   net_ready = false;          /* network settings loaded this run */
    double signin_since = 0.0;         /* waiting on a saved online sign-in */
    double next_list = 0.0;            /* next room list request */
    bool   direct = false;             /* the join-by-address editor is up */
    int    direct_ip[4] = {192, 168, 1, 1};
    int    direct_port = 7777;
    int    direct_field = 3;
    int    last_view = -1;
    double next_scan = 0.0;
    std::vector<std::string> new_packages;   /* files waiting in mods/new */
    char   note[256] = {0};
    double note_until = 0.0;
};
static State g_state;

/* ---- look ------------------------------------------------------------------ */

/* Design grid. */
static const float kW = 640.0f, kH = 480.0f;
static const float kRowH = 30.0f;     /* one menu line */
static const float kItem = 19.0f;     /* menu item text size */
static const float kListX = 58.0f;    /* item labels */
static const float kValueR = 584.0f;  /* right edge of item values */
static const float kListY = 66.0f;    /* first item line */

/* Colours taken from the game's SYSTEM OPTIONS screen. */
static const ImU32 kBack      = IM_COL32(22, 17, 10, 255);    /* behind the frame */
static const ImU32 kBrassHi   = IM_COL32(186, 150, 88, 255);
static const ImU32 kBrass     = IM_COL32(128, 98, 52, 255);
static const ImU32 kBrassLo   = IM_COL32(62, 46, 24, 255);
static const ImU32 kBrassEdge = IM_COL32(34, 25, 12, 255);
static const ImU32 kScreen    = IM_COL32(9, 10, 10, 255);
static const ImU32 kPlateIn   = IM_COL32(14, 24, 18, 255);    /* greenish plate glass */
static const ImU32 kGreen     = IM_COL32(52, 222, 60, 255);   /* items */
static const ImU32 kGreenDim  = IM_COL32(26, 96, 32, 255);    /* unavailable items */
static const ImU32 kBlue      = IM_COL32(52, 128, 255, 255);  /* the chosen item */
static const ImU32 kBlueBand  = IM_COL32(52, 128, 255, 34);
static const ImU32 kWhite     = IM_COL32(232, 232, 228, 255); /* values */
static const ImU32 kGrey      = IM_COL32(140, 142, 138, 255); /* small print */
static const ImU32 kRed       = IM_COL32(236, 70, 54, 255);   /* problems */
static const ImU32 kAmber     = IM_COL32(250, 196, 64, 255);  /* notes */

/* Kept under the names the network pages use. */
static const ImU32 kText = kWhite, kBright = kWhite, kDim = kGrey,
                   kHi = kBlue, kOn = kGreen, kOff = kGreenDim, kWarn = kRed;

struct Gfx {
    ImDrawList* dl;
    ImFont*     font;
    float       s;      /* window pixels per design unit */
    ImVec2      o;      /* window position of design (0,0) */
};

static ImVec2 at(const Gfx& g, float x, float y) {
    return ImVec2(g.o.x + x * g.s, g.o.y + y * g.s);
}

static float text_width(const Gfx& g, float size, const char* s) {
    return g.font->CalcTextSizeA(size * g.s, FLT_MAX, 0.0f, s).x / g.s;
}

static void text(const Gfx& g, float x, float y, float size, ImU32 color,
                 const char* s, float wrap = 0.0f) {
    g.dl->AddText(g.font, size * g.s, at(g, x, y), color, s, nullptr,
                  wrap > 0.0f ? wrap * g.s : 0.0f);
}

static void text_right(const Gfx& g, float right, float y, float size,
                       ImU32 color, const char* s) {
    text(g, right - text_width(g, size, s), y, size, color, s);
}

static void text_centre(const Gfx& g, float cx, float y, float size,
                        ImU32 color, const char* s) {
    text(g, cx - text_width(g, size, s) * 0.5f, y, size, color, s);
}

static std::string upper(const char* s) {
    std::string out(s ? s : "");
    for (char& c : out) c = (char)std::toupper((unsigned char)c);
    return out;
}

static bool same_word(const char* a, const char* b) {
    if (!a || !b) return false;
    for (; *a && *b; ++a, ++b)
        if (std::tolower((unsigned char)*a) != std::tolower((unsigned char)*b))
            return false;
    return *a == *b;
}

/* Cheap per-frame noise for the CRT static. */
static unsigned noise_next(unsigned& seed) {
    seed = seed * 1664525u + 1013904223u;
    return seed >> 8;
}

/* TV static over a rectangle: a scatter of faint grey specks that changes
 * every frame, as on the game's menu screens. */
static void static_fill(const Gfx& g, float x0, float y0, float x1, float y1,
                        int specks, int alpha) {
    unsigned seed = (unsigned)ImGui::GetFrameCount() * 2654435761u ^
                    (unsigned)(x0 * 131.0f + y0 * 977.0f);
    const float w = x1 - x0, h = y1 - y0;
    const float d = 1.1f;
    for (int i = 0; i < specks; ++i) {
        const float x = x0 + (float)(noise_next(seed) % 10000u) * 0.0001f * (w - d);
        const float y = y0 + (float)(noise_next(seed) % 10000u) * 0.0001f * (h - d);
        const int v = 70 + (int)(noise_next(seed) % 110u);
        g.dl->AddRectFilled(at(g, x, y), at(g, x + d, y + d),
                            IM_COL32(v, v, v, alpha));
    }
}

/* A bevelled brass bar: lighter top edge, darker bottom edge. */
static void brass_rect(const Gfx& g, float x0, float y0, float x1, float y1,
                       float round) {
    g.dl->AddRectFilledMultiColor(at(g, x0, y0), at(g, x1, y1),
                                  kBrassHi, kBrassHi, kBrassLo, kBrassLo);
    g.dl->AddRect(at(g, x0, y0), at(g, x1, y1), kBrassEdge, round * g.s, 0,
                  1.2f * g.s);
}

static void rivet(const Gfx& g, float x, float y) {
    g.dl->AddCircleFilled(at(g, x, y), 2.6f * g.s, kBrassEdge);
    g.dl->AddCircleFilled(at(g, x - 0.5f, y - 0.5f), 1.8f * g.s, kBrassHi);
}

/* The page: brass bezel, CRT screen, side brackets, and the name plate on
 * top (the slot that reads CHARACTER SELECT on the game's own screens). */
static void bezel(const Gfx& g, const char* title) {
    /* Frame. */
    g.dl->AddRectFilledMultiColor(at(g, 4.0f, 4.0f), at(g, kW - 4.0f, kH - 4.0f),
                                  kBrass, kBrass, kBrassLo, kBrassLo);
    g.dl->AddRect(at(g, 4.0f, 4.0f), at(g, kW - 4.0f, kH - 4.0f), kBrassEdge,
                  10.0f * g.s, 0, 2.0f * g.s);
    /* Side brackets, as on the game's frame. */
    for (int side = 0; side < 2; ++side) {
        const float x = side == 0 ? 6.0f : kW - 20.0f;
        for (float y = 70.0f; y < kH - 60.0f; y += 72.0f) {
            brass_rect(g, x, y, x + 14.0f, y + 30.0f, 2.0f);
            rivet(g, x + 7.0f, y + 7.0f);
            rivet(g, x + 7.0f, y + 23.0f);
        }
    }
    /* Bottom grille. */
    for (float x = 40.0f; x < kW - 40.0f; x += 16.0f)
        g.dl->AddRectFilled(at(g, x, kH - 22.0f), at(g, x + 9.0f, kH - 8.0f),
                            kBrassEdge, 1.5f * g.s);
    /* Screen. */
    const float sx0 = 24.0f, sy0 = 34.0f, sx1 = kW - 24.0f, sy1 = kH - 30.0f;
    g.dl->AddRectFilled(at(g, sx0 - 3.0f, sy0 - 3.0f), at(g, sx1 + 3.0f, sy1 + 3.0f),
                        kBrassEdge, 22.0f * g.s);
    g.dl->AddRectFilled(at(g, sx0, sy0), at(g, sx1, sy1), kScreen, 20.0f * g.s);
    g.dl->PushClipRect(at(g, sx0 + 6.0f, sy0 + 6.0f), at(g, sx1 - 6.0f, sy1 - 6.0f), true);
    static_fill(g, sx0, sy0, sx1, sy1, 2600, 46);
    g.dl->PopClipRect();
    /* Name plate. */
    const float pw = 260.0f, px = (kW - pw) * 0.5f;
    brass_rect(g, px, 10.0f, px + pw, 40.0f, 4.0f);
    rivet(g, px + 7.0f, 25.0f);
    rivet(g, px + pw - 7.0f, 25.0f);
    g.dl->AddRectFilled(at(g, px + 16.0f, 15.0f), at(g, px + pw - 16.0f, 35.0f),
                        IM_COL32(4, 6, 5, 255), 3.0f * g.s);
    text_centre(g, kW * 0.5f, 18.0f, 13.0f, kGreen, title);
}

/* The framed box in the corner (SYSTEM OPT. on the game's screen): a big
 * blue caption and, under it, what the chosen item does. */
static void info_plate(const Gfx& g, float x, float y, float w, float h,
                       const char* caption, const char* body, ImU32 body_color = kWhite) {
    brass_rect(g, x, y, x + w, y + h, 4.0f);
    /* The red-capped clips top and bottom. */
    const float cx = x + w * 0.5f;
    g.dl->AddRectFilled(at(g, cx - 18.0f, y - 3.0f), at(g, cx + 18.0f, y + 4.0f),
                        IM_COL32(96, 20, 16, 255), 2.0f * g.s);
    g.dl->AddRectFilled(at(g, cx - 18.0f, y + h - 4.0f), at(g, cx + 18.0f, y + h + 3.0f),
                        IM_COL32(96, 20, 16, 255), 2.0f * g.s);
    g.dl->AddRectFilled(at(g, x + 7.0f, y + 7.0f), at(g, x + w - 7.0f, y + h - 7.0f),
                        kPlateIn, 2.0f * g.s);
    g.dl->PushClipRect(at(g, x + 7.0f, y + 7.0f), at(g, x + w - 7.0f, y + h - 7.0f), true);
    static_fill(g, x + 7.0f, y + 7.0f, x + w - 7.0f, y + h - 7.0f, 160, 60);
    g.dl->PopClipRect();
    if (caption && caption[0])
        text(g, x + 16.0f, y + 12.0f, 17.0f, kBlue, caption);
    if (body && body[0])
        text(g, x + 16.0f, y + 36.0f, 10.0f, body_color, body, w - 32.0f);
}

/* Bottom-left key hints, game style: the confirm key in blue, the back key
 * in green ("X:SELECT  TRIANGLE:EXIT" on the PlayStation). */
static void hints(const Gfx& g, const char* confirm_what, const char* back_what,
                  const char* extra = nullptr) {
    float x = 44.0f;
    const float y = kH - 54.0f, size = 11.0f;
    auto part = [&](const char* key, ImU32 key_color, const char* what) {
        text(g, x, y, size, key_color, key);
        x += text_width(g, size, key);
        std::string rest = std::string(":") + what + "   ";
        text(g, x, y, size, kWhite, rest.c_str());
        x += text_width(g, size, rest.c_str());
    };
    if (confirm_what) part("A / ENTER", kBlue, confirm_what);
    if (back_what)    part("B / ESC", kGreen, back_what);
    if (extra)        text(g, x, y, size, kGrey, extra);
    if (g_state.note[0] && ImGui::GetTime() < g_state.note_until)
        text_right(g, kValueR, y - 18.0f, 11.0f, kAmber, g_state.note);
}

static void note(const char* s) {
    std::snprintf(g_state.note, sizeof g_state.note, "%s", s ? s : "");
    g_state.note_until = ImGui::GetTime() + 4.0;
}

/* One menu line: label on the left, value on the right. Returns true when it
 * was clicked with the mouse. */
static bool row(const Gfx& g, float x, float y, float right, bool selected,
                bool enabled, const char* label, const char* value,
                bool* hovered_out, float size = kItem) {
    const ImVec2 a = at(g, x - 10.0f, y - 3.0f), b = at(g, right + 6.0f, y + kRowH - 5.0f);
    const bool hovered = ImGui::IsMouseHoveringRect(a, b, false);
    if (hovered_out) *hovered_out = hovered;
    if (selected) g.dl->AddRectFilled(a, b, kBlueBand, 3.0f * g.s);
    const ImU32 lc = !enabled ? kGreenDim : selected ? kBlue : kGreen;
    text(g, x, y, size, lc, label);
    if (value && value[0])
        text_right(g, right, y, size, !enabled ? kGreenDim : selected ? kBlue : kWhite,
                   value);
    return hovered && enabled && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
}

static bool hover_moved(bool hovered) {
    return hovered && (ImGui::GetIO().MouseDelta.x != 0.0f ||
                       ImGui::GetIO().MouseDelta.y != 0.0f);
}

/* Scroll marks at the right of the list, like the plate's red arrows. */
static void scroll_marks(const Gfx& g, float y_top, float y_bottom, bool above, bool below) {
    const float x = kValueR + 14.0f;
    if (above)
        g.dl->AddTriangleFilled(at(g, x - 5.0f, y_top + 8.0f), at(g, x + 5.0f, y_top + 8.0f),
                                at(g, x, y_top), kRed);
    if (below)
        g.dl->AddTriangleFilled(at(g, x - 5.0f, y_bottom - 8.0f), at(g, x + 5.0f, y_bottom - 8.0f),
                                at(g, x, y_bottom), kRed);
}

/* ---- input ---------------------------------------------------------------- */

struct Input {
    bool up, down, left, right, confirm, back, alt, start, page_up, page_down;
};

static bool key(ImGuiKey k, bool repeat) { return ImGui::IsKeyPressed(k, repeat); }

static Input read_input(const LauncherModel* m) {
    Input in = {};
    /* A modal owns the screen, a text field owns the keys, and a rebind
     * capture owns the pad. */
    if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId |
                                    ImGuiPopupFlags_AnyPopupLevel) ||
        ImGui::GetIO().WantTextInput || m->capturing || m->hk_capturing ||
        m->camera_capturing || m->setup_wizard_open)
        return in;
    in.up    = key(ImGuiKey_UpArrow, true) || key(ImGuiKey_GamepadDpadUp, true) ||
               key(ImGuiKey_GamepadLStickUp, true);
    in.down  = key(ImGuiKey_DownArrow, true) || key(ImGuiKey_GamepadDpadDown, true) ||
               key(ImGuiKey_GamepadLStickDown, true);
    in.left  = key(ImGuiKey_LeftArrow, true) || key(ImGuiKey_GamepadDpadLeft, true) ||
               key(ImGuiKey_GamepadLStickLeft, true);
    in.right = key(ImGuiKey_RightArrow, true) || key(ImGuiKey_GamepadDpadRight, true) ||
               key(ImGuiKey_GamepadLStickRight, true);
    in.confirm = key(ImGuiKey_Enter, false) || key(ImGuiKey_KeypadEnter, false) ||
                 key(ImGuiKey_Space, false) || key(ImGuiKey_GamepadFaceDown, false);
    /* B, and Triangle as on the game's own menus. */
    in.back  = key(ImGuiKey_Escape, false) || key(ImGuiKey_Backspace, false) ||
               key(ImGuiKey_GamepadFaceRight, false) ||
               key(ImGuiKey_GamepadFaceUp, false);
    in.alt   = key(ImGuiKey_Delete, false) || key(ImGuiKey_GamepadFaceLeft, false);
    in.start = key(ImGuiKey_GamepadStart, false);
    in.page_up   = key(ImGuiKey_PageUp, true) || key(ImGuiKey_GamepadR1, true);
    in.page_down = key(ImGuiKey_PageDown, true) || key(ImGuiKey_GamepadL1, true);
    return in;
}

static void move_cursor(Page page, int count, const Input& in, int visible) {
    int& c = g_state.cursor[page];
    int& t = g_state.top[page];
    if (count <= 0) { c = t = 0; return; }
    if (in.up)   c = (c + count - 1) % count;
    if (in.down) c = (c + 1) % count;
    if (c < 0) c = 0;
    if (c >= count) c = count - 1;
    if (c < t) t = c;
    if (c >= t + visible) t = c - visible + 1;
    if (t > count - visible) t = count - visible;
    if (t < 0) t = 0;
}

/* ---- main page ------------------------------------------------------------- */

enum MainEntry { MAIN_START = 0, MAIN_NETWORK, MAIN_SETTINGS, MAIN_MODS, MAIN_EXIT };

static int enabled_mod_count(const LauncherModel* m, int* total_out) {
    int on = 0, total = 0;
    const RecompLauncherCModProvider* mods = m->mods;
    if (mods && mods->feature_count && mods->feature_get) {
        const int n = mods->feature_count(mods->ctx);
        for (int i = 0; i < n; ++i) {
            RecompLauncherCModFeature f;
            std::memset(&f, 0, sizeof f);
            if (!mods->feature_get(mods->ctx, i, &f)) continue;
            if (f.hidden && !f.enabled) continue;
            ++total;
            if (f.enabled) ++on;
        }
    }
    if (total_out) *total_out = total;
    return on;
}

static void start_game(LauncherModel* m) {
    /* The stock PLAY button's decisions, in the same order. */
    if (launcher_model_bios_blocks_play(m)) {
        launcher_model_bios_play_prompt(m);
    } else if (!launcher_model_can_launch(m)) {
        if (m->setup_wizard_supported) m->setup_wizard_open = true;
        else note("SELECT A DISC IMAGE FIRST");
    } else if (!launcher_model_prepare_rom_patch(m)) {
        launcher_model_set_view(m, LNG_VIEW_MODS);
    } else {
        request_mod_commit_launch(m);
    }
}

/* Status lines, label in green and value in white, top right of the screen. */
static void status_line(const Gfx& g, float& y, const char* label, ImU32 vcolor,
                        const char* value, float x0 = 352.0f, float x1 = kValueR) {
    text(g, x0, y, 11.0f, kGreen, label);
    text_right(g, x1, y, 11.0f, vcolor, value);
    y += 18.0f;
}

static void draw_main(LauncherModel* m, const Gfx& g, const Input& in) {
    struct Entry { MainEntry id; const char* label; const char* caption; const char* info; };
    Entry entries[5];
    int count = 0;
    entries[count++] = { MAIN_START, m->in_session ? "RESUME GAME" : "START GAME",
        m->in_session ? "RESUME" : "START",
        m->in_session ? "Return to the game in progress."
                      : "Start Trap Gunner with the settings and mods chosen "
                        "here." };
    if (m->netplay_supported && !m->in_session)
        entries[count++] = { MAIN_NETWORK, "NETWORK PLAY", "NETWORK",
            "Versus over LAN or online, two players, each on their own full "
            "screen. The host is player 1, the guest is player 2." };
    entries[count++] = { MAIN_SETTINGS, "SETTINGS", "SYSTEM OPT.",
        "Display, sound and controls." };
    if (m->mods)
        entries[count++] = { MAIN_MODS, "MODS", "MODS",
            "Switch installed mods on or off, and install new ones." };
    entries[count++] = { MAIN_EXIT, m->in_session ? "QUIT GAME" : "EXIT",
        m->in_session ? "QUIT" : "EXIT",
        m->in_session ? "Close the game." : "Close the launcher." };

    move_cursor(PAGE_MAIN, count, in, count);
    int& cur = g_state.cursor[PAGE_MAIN];

    bezel(g, "TRAP GUNNER");

    /* The title, as on the game's start screen: silver, leaning forward. */
    {
        const char* t1 = "TRAP";
        const char* t2 = "GUNNER";
        const float size = 30.0f;
        const ImU32 shade = IM_COL32(60, 64, 76, 255);
        text(g, kListX + 2.0f, kListY + 2.0f, size, shade, t1);
        text(g, kListX, kListY, size, IM_COL32(226, 230, 238, 255), t1);
        text(g, kListX + 32.0f, kListY + 30.0f, size, shade, t2);
        text(g, kListX + 30.0f, kListY + 28.0f, size, IM_COL32(226, 230, 238, 255), t2);
    }

    int activate = -1;
    const float list_y = kListY + 82.0f;
    for (int i = 0; i < count; ++i) {
        bool hovered = false;
        if (row(g, kListX, list_y + kRowH * i, 300.0f, i == cur, true,
                entries[i].label, nullptr, &hovered))
            activate = i;
        if (hover_moved(hovered)) cur = i;
    }
    if (in.confirm) activate = cur;
    if (in.start) activate = 0;

    /* Status. */
    {
        float y = kListY + 6.0f;
        char buf[160];
        const bool ready = launcher_model_can_launch(m);
        status_line(g, y, "DISC", ready ? kWhite : kRed, ready ? "READY" : "NOT FOUND");
        /* Player 1 always takes a connected controller, with the keyboard
         * live beside it (runtime overlay); a seat set to the keyboard shows
         * as both. */
        const std::string src = upper(launcher_model_player_src_label(m, 0));
        status_line(g, y, "CONTROLLER", kWhite,
                    src == "KEYBOARD" ? "PAD + KEYBOARD" : src.c_str());
        std::snprintf(buf, sizeof buf, "%u / 15 BLOCKS",
                      (unsigned)launcher_model_memcard_blocks_used(m, 0));
        status_line(g, y, "MEMORY CARD", kWhite, buf);
        if (m->mods) {
            int total = 0;
            const int on = enabled_mod_count(m, &total);
            std::snprintf(buf, sizeof buf, "%d ON OF %d", on, total);
            status_line(g, y, "MODS", kWhite, buf);
        }
        const std::string fs = upper(launcher_model_fullscreen_label(m));
        std::snprintf(buf, sizeof buf, "%s  %s",
                      upper(launcher_model_aspect_label(m)).c_str(),
                      fs == "OFF" ? "WINDOW" : fs.c_str());
        status_line(g, y, "DISPLAY", kWhite, buf);
    }

    info_plate(g, 344.0f, 278.0f, 250.0f, 120.0f, entries[cur].caption, entries[cur].info);
    hints(g, "SELECT", nullptr, "START:START GAME");

    if (activate < 0) return;
    cur = activate;
    switch (entries[activate].id) {
        case MAIN_START:    start_game(m); break;
        case MAIN_NETWORK:  launcher_model_set_view(m, LNG_VIEW_NETPLAY_MODE); break;
        case MAIN_SETTINGS: launcher_model_set_view(m, LNG_VIEW_SETTINGS); break;
        case MAIN_MODS:     launcher_model_set_view(m, LNG_VIEW_MODS); break;
        case MAIN_EXIT:     m->action = LNG_ACTION_QUIT; break;
    }
}

/* ---- settings page ---------------------------------------------------------- */

struct Setting {
    const char* label;
    const char* detail;
    bool        (*offered)(LauncherModel*);
    const char* (*value)(LauncherModel*);
    /* dir: 0 confirm, -1 left, +1 right. */
    void        (*step)(LauncherModel*, int dir);
};

static bool always(LauncherModel*) { return true; }

static const char* volume_value(LauncherModel* m) {
    static char buf[16];
    std::snprintf(buf, sizeof buf, "%d", m->s.volume);
    return buf;
}

static void internal_resolution_step(LauncherModel* m, int dir) {
    const int n = launcher_model_internal_resolution_count(m);
    if (n <= 0) return;
    int i = launcher_model_internal_resolution_index(m) + (dir < 0 ? -1 : 1);
    if (i < 0) i = n - 1;
    if (i >= n) i = 0;
    launcher_model_set_internal_resolution(m, i);
}

static const Setting kSettings[] = {
    { "ASPECT", "The shape of the picture. 4:3 is the original; wider shapes "
                "show more of the stage.",
      always, [](LauncherModel* m) { return launcher_model_aspect_label(m); },
      [](LauncherModel* m, int) { launcher_model_cycle_aspect(m); } },
    { "FULLSCREEN", "Off, borderless or exclusive fullscreen.",
      always, [](LauncherModel* m) { return launcher_model_fullscreen_label(m); },
      [](LauncherModel* m, int) { launcher_model_cycle_fullscreen(m); } },
    { "WINDOW SIZE", "Size of the window when fullscreen is off.",
      always, [](LauncherModel* m) { return launcher_model_window_size_label(m); },
      [](LauncherModel* m, int) { launcher_model_cycle_window_size(m); } },
    { "RESOLUTION", "How finely the 3D picture is drawn. Higher is sharper and "
                    "needs a faster graphics card.",
      [](LauncherModel* m) { return launcher_model_internal_resolution_offered(m) &&
                                    launcher_model_internal_resolution_count(m) > 0; },
      [](LauncherModel* m) { return launcher_model_internal_resolution_label(m); },
      internal_resolution_step },
    { "TEXTURE FILTER", "Smooths textures. Off keeps the original sharp pixels.",
      always, [](LauncherModel* m) { return launcher_model_texture_filter_label(m); },
      [](LauncherModel* m, int) { launcher_model_toggle_texture_filter(m); } },
    { "VSYNC", "Matches the picture to the display to avoid tearing.",
      always, [](LauncherModel* m) { return launcher_model_vsync_label(m); },
      [](LauncherModel* m, int) { launcher_model_cycle_vsync(m); } },
    { "VOLUME", "Overall sound level.",
      always, volume_value,
      [](LauncherModel* m, int dir) { launcher_model_volume_delta(m, dir < 0 ? -5 : 5); } },
    { "SKIP THIS MENU", "Start the game directly next time, without this menu.",
      always, [](LauncherModel* m) -> const char* { return m->s.skip_launcher ? "On" : "Off"; },
      [](LauncherModel* m, int) { launcher_model_request_skip_toggle(m); } },
    { "BUTTON LAYOUT", "Choose the controllers for player 1 and player 2 and "
                       "change which button does what.",
      always, [](LauncherModel*) -> const char* { return ">"; },
      [](LauncherModel* m, int dir) { if (dir >= 0) launcher_model_open_config(m, 0); } },
    { "ALL SETTINGS", "The full settings page: renderer, filters, hotkeys and "
                      "everything else. Easiest with a mouse.",
      always, [](LauncherModel*) -> const char* { return ">"; },
      [](LauncherModel*, int dir) { if (dir >= 0) g_state.classic_settings = true; } },
    { "RESTORE DEFAULTS", "Put every setting back to how it was first installed.",
      [](LauncherModel* m) { return launcher_model_can_restore_defaults(m); },
      [](LauncherModel*) -> const char* { return ">"; },
      [](LauncherModel* m, int dir) { if (dir >= 0) launcher_model_request_restore_defaults(m); } },
    { "EXIT", "Back to the system menu.",
      always, [](LauncherModel*) -> const char* { return ""; },
      [](LauncherModel* m, int dir) { if (dir >= 0) launcher_model_set_view(m, LNG_VIEW_DASHBOARD); } },
};

static void draw_settings_page(LauncherModel* m, const Gfx& g, const Input& in) {
    const Setting* rows[sizeof kSettings / sizeof kSettings[0]];
    int count = 0;
    for (const Setting& s : kSettings)
        if (s.offered(m)) rows[count++] = &s;
    const int visible = 7;
    move_cursor(PAGE_SETTINGS, count, in, visible);
    int& cur = g_state.cursor[PAGE_SETTINGS];
    const int top = g_state.top[PAGE_SETTINGS];

    bezel(g, "SYSTEM OPTIONS");
    int clicked = -1;
    for (int i = top; i < count && i < top + visible; ++i) {
        const char* v = rows[i]->value(m);
        std::string shown = upper(v);
        if (shown == ">") shown = "";
        bool hovered = false;
        if (row(g, kListX, kListY + kRowH * (i - top), kValueR, i == cur, true,
                rows[i]->label, shown.c_str(), &hovered))
            clicked = i;
        if (hover_moved(hovered)) cur = i;
    }
    scroll_marks(g, kListY, kListY + kRowH * visible - 6.0f, top > 0, count > top + visible);

    if (count > 0) {
        char cap[48];
        std::snprintf(cap, sizeof cap, "SYSTEM OPT.  %02d/%02d", cur + 1, count);
        info_plate(g, 304.0f, 288.0f, 290.0f, 110.0f, cap, rows[cur]->detail);
    }
    hints(g, "CHANGE", "EXIT", "LEFT/RIGHT:ADJUST");

    if (clicked >= 0) { cur = clicked; rows[cur]->step(m, 0); return; }
    if (count > 0) {
        if (in.confirm)      rows[cur]->step(m, 0);
        else if (in.right)   rows[cur]->step(m, 1);
        else if (in.left)    rows[cur]->step(m, -1);
    }
    if (in.back) launcher_model_set_view(m, LNG_VIEW_DASHBOARD);
}

/* ---- mods page --------------------------------------------------------------- */

static std::string new_mods_dir() { return asset("mods/new"); }

static const char* archive_extension(const LauncherModel* m) {
    return (m->mods && m->mods->archive_extension && m->mods->archive_extension[0])
               ? m->mods->archive_extension : ".psxmod";
}

static void scan_new_packages(const LauncherModel* m) {
    const double now = ImGui::GetTime();
    if (now < g_state.next_scan) return;
    g_state.next_scan = now + 1.0;
    g_state.new_packages.clear();
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::u8path(new_mods_dir());
    std::filesystem::create_directories(dir, ec);
    const std::string want = upper(archive_extension(m));
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        std::error_code fec;
        if (!it->is_regular_file(fec)) continue;
        if (upper(it->path().extension().u8string().c_str()) != want) continue;
        g_state.new_packages.push_back(it->path().filename().u8string());
        if (g_state.new_packages.size() >= 16) break;
    }
    std::sort(g_state.new_packages.begin(), g_state.new_packages.end());
}

static void install_new_package(LauncherModel* m, const std::string& name) {
    const RecompLauncherCModProvider* mods = m->mods;
    const std::filesystem::path src =
        std::filesystem::u8path(new_mods_dir()) / std::filesystem::u8path(name);
    const std::string path = src.u8string();
    if (!mods || !mods->install_archive ||
        !mods->install_archive(mods->ctx, path.c_str())) {
        mod_note_error(m);
        note(m->mod_status);
        return;
    }
    /* Installed: it is the game's copy now. Park the file so it is not
     * offered again, without deleting what the player put there. */
    std::error_code ec;
    std::filesystem::path done = src;
    done += ".installed";
    std::filesystem::rename(src, done, ec);
    g_state.next_scan = 0.0;
    note("INSTALLED. SWITCH IT ON IN THE LIST.");
}

static void remove_feature_package(LauncherModel* m, const RecompLauncherCModFeature& f) {
    const RecompLauncherCModProvider* mods = m->mods;
    if (!mods || !mods->package_count || !mods->package_get || !mods->remove_package)
        return;
    const int n = mods->package_count(mods->ctx);
    for (int i = 0; i < n; ++i) {
        RecompLauncherCModPackage p;
        std::memset(&p, 0, sizeof p);
        if (!mods->package_get(mods->ctx, i, &p)) continue;
        if (std::strcmp(p.id, f.package_id) != 0) continue;
        if (!p.removable) { note("BUILT IN: CANNOT BE REMOVED"); return; }
        if (!mods->remove_package(mods->ctx, p.id, p.version)) {
            mod_note_error(m);
            note(m->mod_status);
        } else {
            note("REMOVED");
        }
        return;
    }
}

static void draw_mods_page(LauncherModel* m, const Gfx& g, const Input& in) {
    const RecompLauncherCModProvider* mods = m->mods;
    std::vector<RecompLauncherCModFeature> features;
    if (mods && mods->feature_count && mods->feature_get) {
        const int n = mods->feature_count(mods->ctx);
        for (int i = 0; i < n; ++i) {
            RecompLauncherCModFeature f;
            std::memset(&f, 0, sizeof f);
            if (!mods->feature_get(mods->ctx, i, &f)) continue;
            if (f.hidden && !f.enabled) continue;
            features.push_back(f);
        }
    }
    scan_new_packages(m);

    /* One list: the installed mods, then the new packages waiting in
     * mods/new, then the fixed entries. */
    enum AddRow { ADD_BROWSE = 0, ADD_FOLDER, ADD_CLASSIC, ADD_EXIT, ADD__COUNT };
    const int nf = (int)features.size();
    const int nn = (int)g_state.new_packages.size();
    const int count = nf + nn + ADD__COUNT;
    const int visible = 7;
    move_cursor(PAGE_MODS, count, in, visible);
    int& cur = g_state.cursor[PAGE_MODS];
    const int top = g_state.top[PAGE_MODS];

    bezel(g, "MOD OPTIONS");
    int clicked = -1;
    for (int i = top; i < count && i < top + visible; ++i) {
        std::string label;
        std::string value;
        bool enabled = true;
        if (i < nf) {
            const RecompLauncherCModFeature& f = features[i];
            label = upper(f.name);
            if (label.size() > 26) label = label.substr(0, 25) + ".";
            value = f.has_error ? "N/A" : f.enabled ? "ON" : "OFF";
            enabled = !f.has_error;
        } else if (i - nf < nn) {
            label = upper(g_state.new_packages[i - nf].c_str());
            if (label.size() > 24) label = label.substr(0, 23) + ".";
            value = "INSTALL";
        } else {
            static const char* const kAdd[ADD__COUNT] = {
                "BROWSE FOR A FILE", "OPEN MODS FOLDER", "ALL MOD OPTIONS", "EXIT" };
            label = kAdd[i - nf - nn];
        }
        bool hovered = false;
        if (row(g, kListX, kListY + kRowH * (i - top), kValueR, i == cur, enabled,
                label.c_str(), value.c_str(), &hovered))
            clicked = i;
        if (hover_moved(hovered)) cur = i;
    }
    if (nf == 0)
        text(g, kListX, kListY - 16.0f, 10.0f, kGrey, "NO MODS INSTALLED YET");
    scroll_marks(g, kListY, kListY + kRowH * visible - 6.0f, top > 0, count > top + visible);

    /* What the chosen line is. */
    char cap[64];
    std::string body;
    ImU32 body_color = kWhite;
    if (cur < nf) {
        const RecompLauncherCModFeature& f = features[cur];
        std::snprintf(cap, sizeof cap, "MOD  %02d/%02d", cur + 1, nf);
        body = f.description;
        if (f.package_version[0]) body += std::string("\nVERSION ") + f.package_version;
        if (f.status[0]) {
            body += std::string("\n") + f.status;
            if (f.has_error) body_color = kRed;
        }
    } else if (cur - nf < nn) {
        std::snprintf(cap, sizeof cap, "NEW MOD");
        body = "A mod package found in the mods/new folder next to the game. "
               "Install copies it into the game and adds it to the list, "
               "switched off.";
    } else {
        const int a = cur - nf - nn;
        std::snprintf(cap, sizeof cap, "%s",
                      a == ADD_BROWSE ? "BROWSE" : a == ADD_FOLDER ? "FOLDER"
                      : a == ADD_CLASSIC ? "ALL OPTIONS" : "EXIT");
        body = a == ADD_BROWSE
                   ? "Pick a mod package anywhere on this computer. The file "
                     "window belongs to the system and needs a mouse or keyboard."
             : a == ADD_FOLDER
                   ? "Opens the mods/new folder. Mod packages placed there "
                     "appear in this list, ready to install with the controller."
             : a == ADD_CLASSIC
                   ? "The full mods page, for mods that have settings of their "
                     "own, version choices and search."
                   : "Back to the system menu.";
    }
    info_plate(g, 304.0f, 288.0f, 290.0f, 110.0f, cap, body.c_str(), body_color);
    hints(g, "SWITCH", "EXIT", "X:REMOVE");

    const bool confirm = in.confirm || clicked >= 0;
    if (clicked >= 0) cur = clicked;
    if (confirm) {
        if (cur < nf) {
            const RecompLauncherCModFeature& f = features[cur];
            if (!mods->feature_enable ||
                !mods->feature_enable(mods->ctx, f.package_id, f.id, f.enabled ? 0 : 1)) {
                mod_note_error(m);
                note(m->mod_status);
            } else {
                m->mod_status[0] = '\0';
            }
        } else if (cur - nf < nn) {
            install_new_package(m, g_state.new_packages[cur - nf]);
        } else if (cur - nf - nn == ADD_BROWSE) {
            char pattern[64];
            std::snprintf(pattern, sizeof pattern, "*%s", archive_extension(m));
            ui_pick_file(m, "Install Mod Package", {pattern},
                         (mods && mods->archive_description)
                             ? mods->archive_description : "Mod package",
                         [m, mods](const char* path) {
                if (!path) return;
                if (!mods || !mods->install_archive ||
                    !mods->install_archive(mods->ctx, path)) {
                    mod_note_error(m);
                    note(m->mod_status);
                } else {
                    note("INSTALLED. SWITCH IT ON IN THE LIST.");
                }
            });
        } else if (cur - nf - nn == ADD_FOLDER) {
            if (!tg_menu_open_folder(new_mods_dir()))
                note("COULD NOT OPEN THE FOLDER");
        } else if (cur - nf - nn == ADD_CLASSIC) {
            g_state.classic_mods = true;
        } else {
            launcher_model_set_view(m, LNG_VIEW_DASHBOARD);
        }
    } else if (in.alt && cur < nf) {
        remove_feature_package(m, features[cur]);
    }
    if (in.back) launcher_model_set_view(m, LNG_VIEW_DASHBOARD);
}

#include "tg_menu_net.inc"

/* ---- entry points -------------------------------------------------------------- */

/* True when this menu, not the stock page, draws the current view. */
static bool owns_view(const LauncherModel* m) {
    if (m->view == LNG_VIEW_DASHBOARD) {
        g_state.classic_settings = false;
        g_state.classic_mods = false;
        g_state.classic_net = false;
        g_state.direct = false;
        return true;
    }
    if (m->view == LNG_VIEW_NETPLAY_MODE) {
        /* The stock pages' Back lands here, which ends the detour. */
        g_state.classic_net = false;
        g_state.direct = false;
        return m->netplay_supported;
    }
    if (m->view == LNG_VIEW_NETPLAY || m->view == LNG_VIEW_LOBBY)
        return m->netplay_supported && !g_state.classic_net;
    if (m->view == LNG_VIEW_SETTINGS) return !g_state.classic_settings;
    if (m->view == LNG_VIEW_MODS)     return !g_state.classic_mods && m->mods != nullptr;
    return false;
}

static void draw(LauncherModel* m, const LauncherTheme& th) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    Gfx g;
    g.dl = ImGui::GetWindowDrawList();
    g.font = ImGui::GetFont();
    g.s = std::min(vp->Size.x / kW, vp->Size.y / kH);
    if (g.s <= 0.0f) g.s = 1.0f;
    g.o = ImVec2(vp->Pos.x + (vp->Size.x - kW * g.s) * 0.5f,
                 vp->Pos.y + (vp->Size.y - kH * g.s) * 0.5f);

    /* Fill the whole window behind the 4:3 frame. */
    const ImVec2 a = vp->Pos, b = ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);
    g.dl->PushClipRect(a, b, false);
    g.dl->AddRectFilled(a, b, kBack);

    const Input in = read_input(m);
    if ((int)m->view != g_state.last_view) {   /* a note belongs to its page */
        g_state.last_view = (int)m->view;
        g_state.note[0] = '\0';
    }
    switch (m->view) {
        case LNG_VIEW_SETTINGS: draw_settings_page(m, g, in); break;
        case LNG_VIEW_MODS:     draw_mods_page(m, g, in); break;
        case LNG_VIEW_NETPLAY_MODE: draw_net_mode(m, g, in); break;
        case LNG_VIEW_NETPLAY:      draw_net_rooms(m, g, in); break;
        case LNG_VIEW_LOBBY:        draw_lobby_page(m, th, g, in); break;
        default:                draw_main(m, g, in); break;
    }
    g.dl->PopClipRect();
}

} /* namespace tg_menu */
