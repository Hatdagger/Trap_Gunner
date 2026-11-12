/* Trap Gunner high frame rate (work in progress).
 *
 * Default: at the battle loop's VSync, redraw in-between frames (replay_pass)
 * with blended player positions and camera anchors.
 * TG_PROBE=1 (diagnostic): at the battle loop's VSync (0x800BD578 called from
 * 0x80014CCC) run one task of the game's task table inside a discarded
 * render pass and count which RAM pages it changes. Tells us which tasks
 * only draw (safe to replay with blended state) and which also run logic.
 * See docs/SPLITSCREEN_NOTES.md.
 */
#include "mod_plugins.h"
#include "cpu_state.h"
#include "psx_cycle_freeze.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace {
constexpr const char* Plugin = "tg.frame-rate";
constexpr uint32_t VSyncFn = 0x800BD578, LoopVSyncRet = 0x80014CD4;
constexpr uint32_t GP = 0x80108A74;
constexpr uint32_t TaskTable = 0x80119AD8, TaskCount = 18, TaskStride = 12;
constexpr uint32_t DbIndex = 0x8013855C, DispX = 0x801384C0, DispY = 0x801384C4;
constexpr uint32_t RamWords = 0x200000 / 4;

uint32_t rd(uint32_t a) { return psx_mod_read_word(a); }
uint16_t rh(uint32_t a) { return uint16_t(rd(a & ~3u) >> ((a & 2u) * 8)); }

std::vector<uint32_t> before;
// psx_mod_counter_add keeps the name pointer: intern names.
std::set<std::string> names;
const char* intern(const char* s) { return names.insert(s).first->c_str(); }
uint32_t probe_task = 0, hook_calls = 0;

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

int probe_pass(CPUState* cpu, void*, uint32_t) {
    const uint32_t slot = TaskTable + TaskStride * probe_task;
    const uint32_t fn = rd(slot);
    if (!fn || !(psx_mod_read_byte(slot + 5) & 4)) return 0;
    before.resize(RamWords);
    for (uint32_t i = 0; i < RamWords; ++i) before[i] = rd(0x80000000u + 4 * i);
    psx_mod_write_word(GP + 0x93C, slot);   // the dispatcher's "current task"
    call(cpu, fn);
    uint32_t changed_pages = 0;
    for (uint32_t page = 0; page < RamWords / 1024; ++page) {
        uint32_t n = 0;
        for (uint32_t i = page * 1024; i < (page + 1) * 1024; ++i)
            if (rd(0x80000000u + 4 * i) != before[i]) ++n;
        if (!n) continue;
        ++changed_pages;
        if (std::getenv("TG_PROBE_DETAIL") && probe_task == uint32_t(std::atoi(std::getenv("TG_PROBE_DETAIL"))))
            for (uint32_t i = page * 1024; i < (page + 1) * 1024; ++i) {
                const uint32_t now = rd(0x80000000u + 4 * i);
                if (now == before[i]) continue;
                char dn[64];
                std::snprintf(dn, sizeof dn, "tg.d.%08X.%08X.%08X", 0x80000000u + 4 * i, before[i], now);
                psx_mod_counter_add(intern(dn), 1);
            }
        char name[64];
        std::snprintf(name, sizeof name, "tg.probe.t%02u.%08X", probe_task, 0x80000000u + page * 4096);
        psx_mod_counter_add(intern(name), n);
    }
    char name[48];
    std::snprintf(name, sizeof name, "tg.probe.t%02u.runs", probe_task);
    psx_mod_counter_add(intern(name), 1);
    return 0;   // discard: this pass only measures
}

// Stage 2a: redraw the frame from the current state with the game's draw
// tasks (no blending yet) to check the replay reproduces the real image.
constexpr uint32_t GsSetWorkBase = 0x800A4D68, WorkBases = 0x801093A0;
constexpr uint32_t ClearOT = 0x800A4AF8, DrawOT = 0x800A4AC8, DrawSync = 0x8009FED4;
constexpr uint32_t SetBg = 0x800A3850, ObjectList = 0x80015BF0;
constexpr uint32_t OTs[4] = {0x80119948, 0x80119970, 0x80119998, 0x80119A10};
constexpr uint32_t DrawTasks[] = {6, 10, 11, 12, 13, 14};
constexpr uint32_t SubmitStart = 0x80014CEC, SubmitEnd = 0x80014F48;

// Moving state blended between ticks: each player's world position and the
// camera anchor that follows it (0x8002BE20).
constexpr uint32_t Players = 0x80124EB0, PlayerStride = 0xA08, PlayerPos = 0x888;
constexpr uint32_t CamCoords = 0x801197E0, CamStride = 0xB4, CamT = 0x18;
constexpr int32_t TeleportLimit = 2000;   // per tick, game units
struct Snap { bool valid = false; int32_t v[4][3]; };
Snap prev, cur;
uint32_t addr_of(int k, int i) {
    return k < 2 ? Players + PlayerStride * k + PlayerPos + 4 * i
                 : CamCoords + CamStride * (k - 2) + CamT + 4 * i;
}
void capture(Snap& s) {
    for (int k = 0; k < 4; ++k) for (int i = 0; i < 3; ++i) s.v[k][i] = int32_t(rd(addr_of(k, i)));
    s.valid = true;
}
void apply_blend(uint32_t alpha_q16) {
    if (!prev.valid || !cur.valid) return;
    for (int k = 0; k < 4; ++k) {
        bool jump = false;
        for (int i = 0; i < 3; ++i) if (std::abs(cur.v[k][i] - prev.v[k][i]) > TeleportLimit) jump = true;
        if (jump) { psx_mod_counter_add("tg.fr.snap", 1); continue; }
        for (int i = 0; i < 3; ++i) {
            const int64_t a = prev.v[k][i], b = cur.v[k][i];
            psx_mod_write_word(addr_of(k, i), uint32_t(int32_t(a + ((b - a) * int64_t(alpha_q16) >> 16))));
        }
    }
}

// Own WIDE view for one player (prototype): the game's single-player layout
// drawn for `seat`. Layout byte gp+0xA = 0 (wide), the WIDE HUD art uploaded
// as battle setup does (0x800151E0 with TIM 0x800F541C), and view 0's player
// index (0x80108A9C) set to the seat.
constexpr uint32_t LayoutByte = GP + 0xA, ViewPlayers = 0x80108A9C;
constexpr uint32_t LoadTim = 0x800151E0, WideHudTim = 0x800F541C;
int own_view_seat = -1;

int replay_pass(CPUState* cpu, void*, uint32_t alpha_q16) {
    if (!std::getenv("TG_NO_BLEND")) apply_blend(alpha_q16);
    if (own_view_seat >= 0) {
        call(cpu, LoadTim, WideHudTim);
        psx_mod_write_word(LayoutByte & ~3u, (rd(LayoutByte & ~3u) & ~0x00FF0000u));
        const uint32_t vp = rd(ViewPlayers & ~3u);
        const uint32_t other = uint32_t(own_view_seat ^ 1);
        psx_mod_write_word(ViewPlayers & ~3u, (vp & ~0xFFFFu) | uint32_t(own_view_seat) | (other << 8));
    }
    const uint32_t db = psx_mod_read_byte(GP + 0x934) & 1;
    call(cpu, GsSetWorkBase, rd(WorkBases + 4 * db));
    for (uint32_t ot : OTs) call(cpu, ClearOT, 0, 0, ot + 0x14 * db);
    uint32_t tasks[18], ntasks = 0;
    if (const char* e = std::getenv("TG_DRAW_TASKS")) {        // test: comma list
        for (const char* p = e; *p && ntasks < 18; ) {
            tasks[ntasks++] = uint32_t(std::strtoul(p, const_cast<char**>(&p), 10));
            if (*p == ',') ++p;
        }
    } else for (uint32_t t : DrawTasks) tasks[ntasks++] = t;
    for (uint32_t k = 0; k < ntasks; ++k) {
        const uint32_t t = tasks[k];
        const uint32_t slot = TaskTable + TaskStride * t;
        if (!(psx_mod_read_byte(slot + 5) & 4) || !rd(slot)) continue;
        psx_mod_write_word(GP + 0x93C, slot);
        call(cpu, rd(slot));
    }
    if (std::getenv("TG_REPLAY_OBJECTS")) call(cpu, ObjectList);
    // Submit exactly as the battle loop does after its flip: background
    // colour, then the four OTs with the per-view offset/clip set between
    // them (0x80014CEC..0x80014F48). Registers as the loop holds them.
    const CPUState saved = *cpu;
    cpu->gpr[17] = OTs[1]; cpu->gpr[18] = OTs[2];
    cpu->gpr[19] = OTs[0]; cpu->gpr[20] = OTs[3];
    cpu->gpr[28] = GP;
    const int ok = psx_mod_run_guest_span(cpu, SubmitStart, SubmitEnd);
    *cpu = saved;
    if (!ok) { psx_mod_counter_add("tg.fr.submit_span_failed", 1); return 0; }
    call(cpu, DrawSync, 0);
    return 1;
}

void on_vsync(CPUState* cpu, uint32_t) {
    if (!std::getenv("TG_PROBE")) {   // default: in-between frames; TG_PROBE=1 runs the task probe
        if (g_psx_render_pass_active || cpu->gpr[31] != LoopVSyncRet) return;
        // Round over: the game parks the battle's drawing tasks (task 11 is
        // the map and objects) and draws the result screens itself. A replay
        // would draw nothing, so in-between frames would come out black.
        {
            const uint32_t slot = TaskTable + TaskStride * 11;
            if (!(psx_mod_read_byte(slot + 5) & 4) || !rd(slot)) {
                psx_mod_counter_add("tg.fr.result_screen", 1);
                return;
            }
        }
        prev = cur; capture(cur);
        uint32_t phases[8];
        static const uint32_t shown_after = std::getenv("TG_D") ? uint32_t(std::atoi(std::getenv("TG_D"))) : 1;
        const uint32_t n = psx_mod_render_pass_plan(2, shown_after, phases, 8);
        if (!n) { psx_mod_counter_add("tg.fr.plan_empty", 1); return; }
        const uint32_t db = rh(DbIndex) & 1;
        PSXModRenderPass rect{};
        rect.struct_size = sizeof rect;
        rect.x = rh(DispX + 2 * db); rect.y = rh(DispY + 2 * db); rect.w = 512; rect.h = 240;
        own_view_seat = std::getenv("TG_OWN_VIEW") ? std::atoi(std::getenv("TG_OWN_VIEW")) : -1;
        uint32_t m = n;
        if (std::getenv("TG_TEST3")) { phases[0] = 16384; phases[1] = 32768; phases[2] = 49152; m = 3; }
        for (uint32_t i = 0; i < m; ++i) {
            rect.alpha_q16 = phases[i];
            psx_mod_counter_add(psx_mod_render_pass(cpu, &rect, replay_pass, nullptr)
                                ? "tg.fr.passes" : "tg.fr.pass_failed", 1);
        }
        return;
    }
    if (g_psx_render_pass_active || cpu->gpr[31] != LoopVSyncRet) return;
    if (++hook_calls % 15) return;
    uint32_t phases[8];
    if (!psx_mod_render_pass_plan(2, 0, phases, 8)) {
        psx_mod_counter_add("tg.probe.plan_empty", 1);
        return;
    }
    const uint32_t db = rh(DbIndex) & 1;
    PSXModRenderPass rect{};
    rect.struct_size = sizeof rect;
    rect.alpha_q16 = phases[0];
    rect.x = rh(DispX + 2 * db); rect.y = rh(DispY + 2 * db); rect.w = 512; rect.h = 240;
    psx_mod_render_pass(cpu, &rect, probe_pass, nullptr);  // discarded by design
    probe_task = (probe_task + 1) % TaskCount;
}

// Test only (TG_FORCE_WIDE=1): make the 2-human battle setup choose the WIDE
// layout so the game initializes everything it derives from it.
void on_layout_store(CPUState* cpu, uint32_t) {
    if (std::getenv("TG_FORCE_WIDE")) cpu->gpr[2] = 0;
}

void activate() {
    psx_mod_set_frame_interpolation_source(PSX_MOD_FRAME_SOURCE_FLIP);
    psx_mod_set_frame_interpolation_blend(PSX_MOD_FRAME_INTERPOLATION_HOLD);
    psx_mod_set_frame_interpolation(60);
    psx_mod_set_render_pass_flip(PSX_MOD_RENDER_PASS_FLIP_PENDING);
}
} // namespace

PSX_MOD_CONSTRUCTOR(tg_register_frame_rate_plugin) {
    psx_mod_register_activation_plugin(Plugin, activate);
    psx_mod_register_function_entry_plugin(Plugin, VSyncFn, on_vsync);
    psx_mod_register_instruction_plugin(Plugin, 0x80028DB4, 0xA0228A7E, on_layout_store);
}
