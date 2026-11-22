/* Trap Gunner dual screen: local versus on one PC with a monitor per player.
 *
 * No network and no second copy of the game: one game runs, and each frame
 * of a battle is drawn twice from the same state with the framework's
 * offline local pair (psx_mod_render_local_pair). View 0 follows player 1
 * and goes to the main window, view 1 follows player 2 and goes to the
 * second window (ui/tg_dual_main.inc opens it, ui/tg_dual_gl.inc draws it).
 *
 * 2-player versus: the battle is set up in the game's one-player WIDE
 * layout instead of the top/bottom split, as online versus does
 * (src/mods/tg_netplay_view.cpp, the same hooks with the same addresses):
 * the WIDE HUD art, the centred camera for both players, and each view
 * showing only its own player's trap and ammo panel.
 * 1-player fights are WIDE already: the second screen shows the CPU
 * opponent's view of the same fight.
 * Everything else (menus, movies, result screens) is mirrored.
 *
 * Package plugin: off unless the "Dual Screen" mod is on, and never in a
 * netplay match (every online match clears the mod plan).
 */
#include "mod_plugins.h"
#include "cpu_state.h"
#include "psx_cycle_freeze.h"
#include <atomic>
#include <cstdint>

extern "C" int tg_dual_screen_open(void);   /* ui/tg_dual_main.inc */

namespace {
constexpr const char* Plugin = "tg.dual-screen";

std::atomic<int> s_wanted{0};

constexpr uint32_t GP = 0x80108A74;
constexpr uint32_t LayoutByte = GP + 0xA;           // 0 wide, 1 split
constexpr uint32_t ViewPlayers = 0x80108A9C;        // u8 view 0 player, u8 view 1 player
constexpr uint32_t HudMask = GP + 0x8;              // bit n: draw player n's HUD panel
constexpr uint32_t LoadTim = 0x800151E0, WideHudTim = 0x800F541C, SplitHudTim = 0x800F945C;
constexpr uint32_t SetupTimRet[] = {0x80028EB8, 0x800282C4};  // battle setup's HUD TIM loads
constexpr uint32_t VSyncFn = 0x800BD578, LoopVSyncRet = 0x80014CD4;
constexpr uint32_t DbIndex = 0x8013855C, DispX = 0x801384C0, DispY = 0x801384C4;
constexpr uint32_t TaskTable = 0x80119AD8, TaskStride = 12;
constexpr uint32_t GsSetWorkBase = 0x800A4D68, WorkBases = 0x801093A0;
constexpr uint32_t ClearOT = 0x800A4AF8, DrawSync = 0x8009FED4;
constexpr uint32_t OTs[4] = {0x80119948, 0x80119970, 0x80119998, 0x80119A10};
constexpr uint32_t DrawTasks[] = {3, 6, 10, 11, 12, 13, 14};
constexpr uint32_t SubmitStart = 0x80014CEC, SubmitEnd = 0x80014F48;
constexpr uint32_t SetCamState = 0x80031014;
constexpr uint32_t HudPanelFns[] = {0x8005CA7C, 0x8005D0D4};

uint32_t rd(uint32_t a) { return psx_mod_read_word(a); }
uint8_t rb(uint32_t a) { return psx_mod_read_byte(a); }
uint16_t rh(uint32_t a) { return uint16_t(rd(a & ~3u) >> ((a & 2u) * 8)); }
void wb(uint32_t a, uint8_t v) {
    const uint32_t w = a & ~3u, sh = (a & 3u) * 8;
    psx_mod_write_word(w, (rd(w) & ~(0xFFu << sh)) | (uint32_t(v) << sh));
}

uint32_t call(CPUState* cpu, uint32_t fn, uint32_t a0 = 0, uint32_t a1 = 0,
              uint32_t a2 = 0, uint32_t a3 = 0) {
    const CPUState saved = *cpu;
    cpu->pc = 0;
    cpu->gpr[4] = a0; cpu->gpr[5] = a1; cpu->gpr[6] = a2; cpu->gpr[7] = a3;
    psx_dispatch_call(cpu, fn, saved.gpr[31]);
    const uint32_t r = cpu->gpr[2];
    *cpu = saved;
    return r;
}

bool dual() { return s_wanted.load() && tg_dual_screen_open(); }

bool two_humans() { return rb(GP + 0xC) == 1 && rb(GP + 0xD) == 1; }

// The battle's drawing tasks are running (parked with flag 0x01 on the
// result screens, which task 7 draws instead: those are mirrored).
bool battle_drawing() {
    const uint32_t slot = TaskTable + TaskStride * 11;   // map and objects
    return (rb(slot + 5) & 4) && rd(slot);
}

// 2-human battle setup is about to load the split-screen HUD art: choose the
// WIDE layout instead, before the battle tasks are created.
void on_load_tim(CPUState* cpu, uint32_t) {
    if (!dual() || g_psx_render_pass_active) return;
    const uint32_t ra = cpu->gpr[31];
    if (ra != SetupTimRet[0] && ra != SetupTimRet[1]) return;
    if (cpu->gpr[4] != SplitHudTim) return;
    wb(LayoutByte, 0);
    cpu->gpr[4] = WideHudTim;
    psx_mod_counter_add("tg.dual.wide_setup", 1);
}

// Both players are watched full screen: both get the centred camera (state
// 2) instead of the one-player human camera's dead zone and lean (3, 4).
void on_set_cam_state(CPUState* cpu, uint32_t) {
    if (!dual() || g_psx_render_pass_active) return;
    const uint32_t state = cpu->gpr[5];
    if ((state != 3 && state != 4) || rb(LayoutByte) != 0 || !two_humans()) return;
    cpu->gpr[5] = 2;
    psx_mod_counter_add("tg.dual.cam_centred", 1);
}

// In WIDE every player's trap and ammo panel lands in the same place: while
// a panel routine runs, the HUD mask holds only the view's own player.
int in_hud_wrap = 0;
int wrap_hud(CPUState* cpu, uint32_t address) {
    if (!dual() || in_hud_wrap || rb(LayoutByte) != 0 || !two_humans()) return 0;
    const uint8_t saved = rb(HudMask);
    wb(HudMask, uint8_t(1u << (rb(ViewPlayers) & 1u)));
    in_hud_wrap = 1;
    psx_dispatch_call(cpu, address, cpu->gpr[31]);
    in_hud_wrap = 0;
    wb(HudMask, saved);
    return 1;   // the call ran with the original body
}

// One view: the frame redrawn by the game's own draw tasks with view 0
// following `eye` (0 = player 1, 1 = player 2 or the CPU opponent).
int draw_view(CPUState* cpu, void*, uint32_t eye) {
    const int seat = int(eye & 1u);
    wb(ViewPlayers, uint8_t(seat));
    wb(ViewPlayers + 1, uint8_t(seat ^ 1));
    const uint32_t db = rb(GP + 0x934) & 1;
    call(cpu, GsSetWorkBase, rd(WorkBases + 4 * db));
    for (uint32_t ot : OTs) call(cpu, ClearOT, 0, 0, ot + 0x14 * db);
    for (uint32_t t : DrawTasks) {
        const uint32_t slot = TaskTable + TaskStride * t;
        if (!(rb(slot + 5) & 4) || !rd(slot)) continue;
        psx_mod_write_word(GP + 0x93C, slot);
        call(cpu, rd(slot));
    }
    const CPUState saved = *cpu;
    cpu->gpr[17] = OTs[1]; cpu->gpr[18] = OTs[2];
    cpu->gpr[19] = OTs[0]; cpu->gpr[20] = OTs[3];
    cpu->gpr[28] = GP;
    const int ok = psx_mod_run_guest_span(cpu, SubmitStart, SubmitEnd);
    *cpu = saved;
    if (!ok) { psx_mod_counter_add("tg.dual.span_failed", 1); return 0; }
    call(cpu, DrawSync, 0);
    return 1;
}

// The battle loop's VSync: the frame is finished, draw both views.
void on_vsync(CPUState* cpu, uint32_t) {
    if (!g_psx_render_pass_active) in_hud_wrap = 0;   // a rolled-back pass can leave it set
    if (g_psx_render_pass_active || cpu->gpr[31] != LoopVSyncRet || !dual()) return;
    if (rb(LayoutByte) != 0) return;                   // split screen: shown as is, mirrored
    if (!battle_drawing()) { psx_mod_counter_add("tg.dual.result_screen", 1); return; }
    const uint32_t db = rh(DbIndex) & 1;
    PSXModStereoFrame frame{};
    frame.struct_size = sizeof frame;
    frame.period_vblanks = 2;                          // battles draw every other VBlank
    frame.x = rh(DispX + 2 * db);
    frame.y = rh(DispY + 2 * db);
    frame.w = 512;
    frame.h = 240;
    psx_mod_counter_add(psx_mod_render_local_pair(cpu, &frame, draw_view, nullptr)
                        ? "tg.dual.pairs" : "tg.dual.pair_refused", 1);
}

void activate() { s_wanted = 1; }
void session_reset() { s_wanted = 0; }
} // namespace

/* For ui/tg_dual_main.inc: open the second window while the mod is on. */
extern "C" int tg_dual_screen_wanted(void) { return s_wanted.load(); }

PSX_MOD_CONSTRUCTOR(tg_register_dual_screen_plugin) {
    psx_mod_register_activation_plugin(Plugin, activate);
    psx_mod_register_session_reset_plugin(Plugin, session_reset);
    psx_mod_register_function_entry_plugin(Plugin, LoadTim, on_load_tim);
    psx_mod_register_function_entry_plugin(Plugin, VSyncFn, on_vsync);
    psx_mod_register_function_entry_plugin(Plugin, SetCamState, on_set_cam_state);
    for (uint32_t fn : HudPanelFns) psx_mod_register_function_filter_plugin(Plugin, fn, wrap_hud);
}
