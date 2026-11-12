/* Trap Gunner online versus: every player sees their own full-screen WIDE
 * view (the game's one-player layout) instead of the top/bottom split.
 *
 * Shared frame: during netplay the 2-human battle is set up in the WIDE
 * layout on every peer (layout byte gp+0xA = 0 and the WIDE HUD TIM), so all
 * peers run and draw the same frame, which follows player 1. Guest state is
 * identical everywhere; the netplay digests check it.
 *
 * Own view: at the battle VSync, a peer whose seat is not player 1 redraws
 * the frame inside psx_mod_render_local_view with the view-player bytes
 * (0x80108A9C / 0x80108A9D) swapped, so the camera and HUD follow its own
 * player. The sandbox restores the whole machine; only the presented image
 * differs per peer. Design after R4's Link Battle
 * (tetrisgm/RidgeRacerType4Recomp docs/ONLINE_BATTLE.md).
 *
 * These are game-owned netplay hooks (psx_game_register_netplay_*): package
 * mods are off in netplay, and offline play is untouched.
 * See docs/SPLITSCREEN_NOTES.md.
 */
#include "mod_plugins.h"
#include "cpu_state.h"
#include "psx_cycle_freeze.h"
#include "psx_netplay.h"
#include <cstdint>

namespace {
constexpr uint32_t GP = 0x80108A74;
constexpr uint32_t LayoutByte = GP + 0xA;           // 0 wide, 1 split
constexpr uint32_t ViewPlayers = 0x80108A9C;        // u8 view 0 player, u8 view 1 player
constexpr uint32_t LoadTim = 0x800151E0, WideHudTim = 0x800F541C, SplitHudTim = 0x800F945C;
constexpr uint32_t SetupTimRet[] = {0x80028EB8, 0x800282C4};  // battle setup's HUD TIM loads
constexpr uint32_t VSyncFn = 0x800BD578, LoopVSyncRet = 0x80014CD4;
constexpr uint32_t DbIndex = 0x8013855C, DispX = 0x801384C0, DispY = 0x801384C4;
constexpr uint32_t TaskTable = 0x80119AD8, TaskStride = 12;
constexpr uint32_t GsSetWorkBase = 0x800A4D68, WorkBases = 0x801093A0;
constexpr uint32_t ClearOT = 0x800A4AF8, DrawSync = 0x8009FED4;
constexpr uint32_t OTs[4] = {0x80119948, 0x80119970, 0x80119998, 0x80119A10};
// Task 3 (0x80059168, player update) builds the trap placement / search area
// preview of the view player, so the own view runs it too (in the sandbox:
// its logic is discarded with everything else; the image may lead by a tick).
constexpr uint32_t DrawTasks[] = {3, 6, 10, 11, 12, 13, 14};
constexpr uint32_t SubmitStart = 0x80014CEC, SubmitEnd = 0x80014F48;

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

constexpr uint32_t GameMode = 0x801093CC;   // 9 = VS battle running
bool two_player_battle() {
    // VS battle with two human players (setup stores 1 to both flags).
    return rb(GameMode) == 9 && rb(GP + 0xC) == 1 && rb(GP + 0xD) == 1;
}

// Battle setup is about to load the HUD TIM chosen from the layout: in netplay
// choose WIDE for the 2-human battle, before the battle tasks are created.
void on_load_tim(CPUState* cpu, uint32_t) {
    if (!psx_netplay_active() || g_psx_render_pass_active) return;
    const uint32_t ra = cpu->gpr[31];
    if (ra != SetupTimRet[0] && ra != SetupTimRet[1]) return;
    if (cpu->gpr[4] != SplitHudTim) return;
    wb(LayoutByte, 0);
    cpu->gpr[4] = WideHudTim;
    psx_mod_counter_add("tg.np.wide_setup", 1);
}

// HUD routines of task 13 draw a panel for every player in the HUD player
// mask gp+0x8 (bit n = player n; 3 in a 2-human battle): 0x8005CA7C is the
// current-trap and ammo panel, 0x8005D0D4 the next HUD routine. In WIDE all
// panels land in the same place, so online they overlapped. While either
// routine runs, the mask holds only the view player (0x80108A9C) and is put
// back afterwards: the shared frame shows player 1's panel, the own view
// (players swapped) its own player's. Every peer does the same.
constexpr uint32_t HudMask = GP + 0x8;
constexpr uint32_t HudPanelFns[] = {0x8005CA7C, 0x8005D0D4};
int in_hud_wrap = 0;
int wrap_hud(CPUState* cpu, uint32_t address) {
    if (in_hud_wrap || rb(LayoutByte) != 0 || !two_player_battle()) return 0;
    const uint8_t saved = rb(HudMask);
    wb(HudMask, uint8_t(1u << (rb(ViewPlayers) & 1u)));
    in_hud_wrap = 1;
    psx_dispatch_call(cpu, address, cpu->gpr[31]);
    in_hud_wrap = 0;
    wb(HudMask, saved);
    psx_mod_counter_add("tg.np.hud_wrapped", 1);
    return 1;   // the call ran with the original body
}

int draw_own_view(CPUState* cpu, void* user, uint32_t) {
    const int seat = *static_cast<int*>(user);
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
    if (!ok) { psx_mod_counter_add("tg.np.span_failed", 1); return 0; }
    call(cpu, DrawSync, 0);
    return 1;
}

void on_vsync(CPUState* cpu, uint32_t) {
    if (!g_psx_render_pass_active) in_hud_wrap = 0;   // a rolled-back pass can leave it set
    if (g_psx_render_pass_active || cpu->gpr[31] != LoopVSyncRet) return;
    if (rb(LayoutByte) != 0 || !two_player_battle()) return;
    static int seat;
    seat = psx_netplay_local_slot();
    if (seat <= 0 || seat > 1) return;      // player 1's view is the shared frame
    if (psx_mod_render_local_view_status() != PSX_MOD_RENDER_PASS_READY) {
        psx_mod_counter_add("tg.np.view_unavailable", 1);
        return;
    }
    const uint32_t db = rh(DbIndex) & 1;
    PSXModRenderPass rect{};
    rect.struct_size = sizeof rect;
    rect.x = rh(DispX + 2 * db); rect.y = rh(DispY + 2 * db); rect.w = 512; rect.h = 240;
    psx_mod_counter_add(psx_mod_render_local_view(cpu, &rect, draw_own_view, &seat)
                        ? "tg.np.own_views" : "tg.np.own_view_failed", 1);
}
} // namespace

PSX_MOD_CONSTRUCTOR(tg_register_netplay_view) {
    psx_game_register_netplay_function_entry(LoadTim, on_load_tim);
    psx_game_register_netplay_function_entry(VSyncFn, on_vsync);
    for (uint32_t fn : HudPanelFns) psx_game_register_netplay_function_filter(fn, wrap_hud);
}
