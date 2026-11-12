/* Trap Gunner high frame rate (work in progress).
 *
 * Stage 1: probe. At the battle loop's VSync (0x800BD578 called from
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

int replay_pass(CPUState* cpu, void*, uint32_t) {
    const uint32_t db = psx_mod_read_byte(GP + 0x934) & 1;
    call(cpu, GsSetWorkBase, rd(WorkBases + 4 * db));
    for (uint32_t ot : OTs) call(cpu, ClearOT, 0, 0, ot + 0x14 * db);
    for (uint32_t t : DrawTasks) {
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
    if (std::getenv("TG_REPLAY")) {
        if (g_psx_render_pass_active || cpu->gpr[31] != LoopVSyncRet) return;
        uint32_t phases[8];
        const uint32_t n = psx_mod_render_pass_plan(2, 0, phases, 8);
        if (!n) { psx_mod_counter_add("tg.fr.plan_empty", 1); return; }
        const uint32_t db = rh(DbIndex) & 1;
        PSXModRenderPass rect{};
        rect.struct_size = sizeof rect;
        rect.x = rh(DispX + 2 * db); rect.y = rh(DispY + 2 * db); rect.w = 512; rect.h = 240;
        for (uint32_t i = 0; i < n; ++i) {
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
}
