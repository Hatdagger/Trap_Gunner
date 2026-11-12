# Trap Gunner's changes to framework sources, applied to COPIES at configure
# time. The psxrecomp and recomp-ui submodules stay pinned to their upstream
# commits and are never edited: each overlaid file is read from the
# submodule, its insertions are made at fixed anchor lines, the result is
# written under the build tree, and the game target compiles that copy in
# place of the original.
#
# Every anchor must match exactly once. If a framework bump moves or rewrites
# an anchor, configure stops with the anchor's text instead of building a
# launcher that silently lost the menu.
#
# Overlays:
#   recomp-ui  launcher_imgui.cpp  the Trap Gunner system menu (ui/tg_menu.inc)
#   psxrecomp  runtime main.cpp    disc auto-detection beside the executable,
#                                  the menu kept in Steam Deck Game Mode,
#                                  player 1 on any connected controller

set(TG_OVERLAY_DIR "${CMAKE_BINARY_DIR}/tg_overlay")

# _tg_insert(<content-var> <anchor> <text> BEFORE|AFTER)
function(_tg_insert content_var anchor text where)
    set(_c "${${content_var}}")
    string(FIND "${_c}" "${anchor}" _first)
    if(_first EQUAL -1)
        message(FATAL_ERROR "tg overlay: anchor not found in ${TG_OVERLAY_CURRENT}:\n${anchor}")
    endif()
    string(FIND "${_c}" "${anchor}" _last REVERSE)
    if(NOT _first EQUAL _last)
        message(FATAL_ERROR "tg overlay: anchor is not unique in ${TG_OVERLAY_CURRENT}:\n${anchor}")
    endif()
    if(where STREQUAL "BEFORE")
        string(REPLACE "${anchor}" "${text}${anchor}" _c "${_c}")
    else()
        string(REPLACE "${anchor}" "${anchor}${text}" _c "${_c}")
    endif()
    set(${content_var} "${_c}" PARENT_SCOPE)
endfunction()

# _tg_replace(<content-var> <anchor> <text>): replace the one match.
function(_tg_replace content_var anchor text)
    _tg_insert(${content_var} "${anchor}" "" BEFORE)   # checks it exists, once
    string(REPLACE "${anchor}" "${text}" _c "${${content_var}}")
    set(${content_var} "${_c}" PARENT_SCOPE)
endfunction()

# _tg_swap_source(<target> <original> <overlaid>): compile the overlaid copy
# instead of the original, with the original's directory still searched first
# for its own quoted includes.
function(_tg_swap_source target original overlaid)
    get_target_property(_srcs ${target} SOURCES)
    list(FIND _srcs "${original}" _idx)
    if(_idx EQUAL -1)
        message(FATAL_ERROR "tg overlay: ${original} is not a source of ${target}")
    endif()
    list(REMOVE_AT _srcs ${_idx})
    list(INSERT _srcs ${_idx} "${overlaid}")
    set_property(TARGET ${target} PROPERTY SOURCES ${_srcs})
    get_filename_component(_dir "${original}" DIRECTORY)
    set_source_files_properties("${overlaid}" PROPERTIES
        INCLUDE_DIRECTORIES "${_dir};${CMAKE_SOURCE_DIR}/ui")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${original}")
endfunction()

# ---- recomp-ui: the system menu ---------------------------------------------

function(tg_overlay_launcher_menu target)
    if(NOT RECOMP_UI_ROOT OR NOT EXISTS "${RECOMP_UI_ROOT}/src/common/backends/imgui/launcher_imgui.cpp")
        return()   # no launcher in this build
    endif()
    set(_orig "${RECOMP_UI_ROOT}/src/common/backends/imgui/launcher_imgui.cpp")
    set(TG_OVERLAY_CURRENT "${_orig}")
    file(READ "${_orig}" _c)
    string(REPLACE "\r\n" "\n" _c "${_c}")   # a Windows checkout may be CRLF

    # 1. The menu and its glue, just before draw_ui().
    _tg_insert(_c "void draw_ui(LauncherModel* m, const LauncherTheme& th, int logical_w, int logical_h) {"
"/* ---- Trap Gunner system menu (overlay: cmake/tg_framework_overlay.cmake) ---- */
static bool tg_menu_open_folder(const std::string& dir) {
    std::string url, error;
    if (!launcher_directory_url(dir.c_str(), url, error)) return false;
#if defined(LNG_SDL3)
    return SDL_OpenURL(url.c_str());
#else
    return SDL_OpenURL(url.c_str()) == 0;
#endif
}
/* The stock room page's dialogs, for the menu's own room page: match
 * settings, the missing-disc prompt, and the room's mod plan. */
static void tg_menu_lobby_popups(LauncherModel* m, const LauncherTheme& th,
                                 const RecompLauncherCNetplayCallbacks* np,
                                 const LobbySnapshot& s) {
    draw_lobby_settings_popup(m, th, np, s);
    draw_lobby_game_rom_popup(m, th, s);
#if RECOMP_UI_ENABLE_MODS
    draw_lobby_mods_popup(m, th, np, s.is_host);
    if (!m->netplay_lobby_mods_open && !s.local_rom_missing)
        draw_lobby_mod_files_popup(m, th, np, s.is_host);
#endif
}
#include <cfloat>
#include <cmath>
#include \"tg_menu.inc\"
/* ---- end Trap Gunner system menu ---- */

" BEFORE)

    # 2. The menu owns its views; the stock header, body and footer draw for
    #    everything else (controller page, sign-in, setup).
    _tg_insert(_c "    // ---- Marquee header: brand · GAME TITLE · subtitle .......... [nav] ----"
"    const bool tg_title_menu = tg_menu::owns_view(m);
    if (tg_title_menu) tg_menu::draw(m, th);
    if (!tg_title_menu) {   /* stock header, page body and footer */
" BEFORE)
    _tg_insert(_c "    draw_footer(m, th, footer_h);
    if (launch_commit_pending()) {"
"    }   /* !tg_title_menu */
" BEFORE)

    # The closing brace above lands before "draw_footer"; move it after.
    string(REPLACE "    }   /* !tg_title_menu */
    draw_footer(m, th, footer_h);
" "    draw_footer(m, th, footer_h);
    }   /* !tg_title_menu */
" _c "${_c}")

    set(_out "${TG_OVERLAY_DIR}/recomp-ui/launcher_imgui.cpp")
    file(WRITE "${_out}.tmp" "${_c}")
    configure_file("${_out}.tmp" "${_out}" COPYONLY)   # rewritten only on change
    _tg_swap_source(${target} "${_orig}" "${_out}")
    set_property(SOURCE "${_out}" APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_SOURCE_DIR}/ui/tg_menu.inc;${CMAKE_SOURCE_DIR}/ui/tg_menu_net.inc")
    message(STATUS "tg overlay: Trap Gunner system menu in the launcher")
endfunction()

# ---- psxrecomp runtime: disc and controller defaults ------------------------

function(tg_overlay_runtime_defaults target)
    set(_orig "${PSXRECOMP_ROOT}/runtime/src/main.cpp")
    set(TG_OVERLAY_CURRENT "${_orig}")
    file(READ "${_orig}" _c)
    string(REPLACE "\r\n" "\n" _c "${_c}")   # a Windows checkout may be CRLF

    # 1. The disc finder, just before main().
    _tg_insert(_c "int main(int argc, char** argv) {"
"/* ---- Trap Gunner (overlay: cmake/tg_framework_overlay.cmake) ----
 * First run, or the remembered image is gone: adopt a disc image of THIS game
 * that sits beside the executable (or in a disc folder there) instead of
 * asking for one. Candidates are identified by the serial on the disc itself,
 * never by file name; a cue or CHD is preferred over a bare image. */
static std::filesystem::path tg_discover_game_disc_near(
    const std::filesystem::path& exe_dir, const std::string& game_id,
    std::string* serial_out) {
    namespace fs = std::filesystem;
    std::vector<std::string> serials;
    if (!game_id.empty()) serials.push_back(uppercase_ascii(game_id));
    for (const auto& kv : g_disc_serials) {
        const std::string up = uppercase_ascii(kv.second);
        if (std::find(serials.begin(), serials.end(), up) == serials.end())
            serials.push_back(up);
    }
    if (serials.empty()) return {};
    static const char* kSubdirs[] = { \"\", \"disc\", \"discs\", \"iso\", \"roms\", \"games\" };
    static const char* kExts[] = { \".cue\", \".chd\", \".iso\", \".img\", \".bin\" };
    fs::path best;
    int best_rank = 1 << 30;
    std::string best_serial;
    for (const char* sub : kSubdirs) {
        const fs::path dir = sub[0] ? (exe_dir / sub) : exe_dir;
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        int seen = 0;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (++seen > 512) break;
            std::error_code fec;
            if (!it->is_regular_file(fec)) continue;
            const fs::path cand = it->path();
            std::string ext = cand.extension().string();
            for (char& c : ext) c = (char)std::tolower((unsigned char)c);
            int ext_rank = -1;
            for (int i = 0; i < (int)(sizeof kExts / sizeof kExts[0]); ++i)
                if (ext == kExts[i]) { ext_rank = i; break; }
            if (ext_rank < 0) continue;
            const PSXRecompV4::DiscIdentity id = PSXRecompV4::identify_disc(
                cand, std::string(), /*expected_crc*/0, /*has_expected_crc*/false,
                /*compute_crc*/false);
            if (!id.opened || !id.has_header) continue;
            const std::string serial = uppercase_ascii(id.detected_serial);
            const auto pos = std::find(serials.begin(), serials.end(), serial);
            if (pos == serials.end()) continue;
            const int rank = (int)(pos - serials.begin()) * 16 + ext_rank;
            if (rank < best_rank) {
                best_rank = rank;
                best = cand;
                best_serial = serial;
            }
        }
    }
    if (serial_out) *serial_out = best_serial;
    return best;
}
/* ---- end Trap Gunner ---- */

" BEFORE)

    # 2. Use it when there is no usable disc yet. A --disc override is never
    #    second-guessed.
    _tg_insert(_c "    /* lock_mode: the game supports exactly ONE pad type"
"    /* Trap Gunner overlay: no usable disc yet (first run, or the remembered
     * image moved): look for one beside the executable before the launcher
     * has to ask. */
    if (!disc_override_path) {
        const std::filesystem::path tg_exe_dir = exe_dir_from_argv(argv[0]);
        std::error_code tg_ec;
        const bool tg_have_disc = !resolved_disc.empty() &&
            std::filesystem::exists(
                PSXRecompV4::host_resolve(tg_exe_dir, resolved_disc), tg_ec);
        if (!tg_have_disc) {
            std::string tg_serial;
            const std::filesystem::path tg_found =
                tg_discover_game_disc_near(tg_exe_dir, game_id, &tg_serial);
            if (!tg_found.empty()) {
                resolved_disc = normalize_disc_path_for_launch(tg_found);
                std::fprintf(stdout, \"psxrecomp: adopted disc image %s (%s)\\n\",
                             resolved_disc.string().c_str(), tg_serial.c_str());
            }
        }
    }

" BEFORE)

    # 3. Steam Deck Game Mode: keep the launcher. The framework boots straight
    #    into the game there once a disc is set up (the stock launcher needs a
    #    mouse); Trap Gunner's menu is controller-first, and with the disc
    #    found beside the executable every run would otherwise skip it. The
    #    player's own SKIP THIS MENU setting and --no-launcher still apply.
    _tg_replace(_c "            if (disc_ready && !force_launcher) force_no_launcher = true;"
"            (void)disc_ready;   /* Trap Gunner overlay: the menu stays in Game Mode */")
    _tg_replace(_c "\"launcher first (no disc set up yet)\""
                   "\"launcher first (Trap Gunner menu)\"")

    # 4. Player 1 is whatever game controller is connected, with the keyboard
    #    binds live beside it (pad_sources_for keeps them on for every seat).
    #    A seat saved as \"keyboard\" therefore also picks up a controller, now
    #    or when one is plugged in; with no controller the keyboard alone
    #    drives it, as before. Player 2 keeps its own setting.
    _tg_insert(_c "        set_player_device(g_players[s], player_device[s], player_mode[s]);
"
"        if (s == 0 && g_players[0].kind == 1) {   /* Trap Gunner overlay */
            g_players[0].kind = 2;
            g_players[0].guid[0] = '\\0';
        }
" AFTER)

    set(_out "${TG_OVERLAY_DIR}/psxrecomp/main.cpp")
    file(WRITE "${_out}.tmp" "${_c}")
    configure_file("${_out}.tmp" "${_out}" COPYONLY)   # rewritten only on change
    _tg_swap_source(${target} "${_orig}" "${_out}")
    message(STATUS "tg overlay: disc auto-detection and player 1 controller default")
endfunction()
