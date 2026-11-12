# Trap Gunner (USA, SLUS-00679) — split-screen / netplay view notes

RAM addresses found by static analysis of the boot EXE (no disc data here).

| Address | Size | Meaning |
|---|---|---|
| 0x80108A74 | — | `$gp` base |
| 0x80124EA0 | u8[7] | System options: 0 difficulty, 1 translucency, 2 display icon, 3 camera angle (0 NORMAL, 1 TOP, 2 QUARTER), 4 **screen mode (0 WIDE, 1 SPLIT)**, 5 vibration, 6 sound |
| 0x801093CC | s8 | Game mode; 8 = one-human-player battle setup |
| 0x80108A7E (`gp+0xA`) | u8 | **Battle layout in use: 0 wide, 1 split.** Set by battle setup at 0x80028100: mode 8 forces 0, otherwise copies options[4] |
| 0x80108A80 / 0x80108A81 | u8 | Set alongside the layout (0x81 = 1 for two-human battles) |
| 0x801093D4 | u8 | 1 for mode 8, else 2 |

Option menu draw: 0x8004170C (value table 0x800CE974, 3 entries × 4 bytes per row).

## Netplay constraint

psxrecomp rollback compares a full digest of CPU + main RAM (and GPU/VRAM at
baseline) between peers every frame. Each peer must therefore run identical
RAM and VRAM; anything per-player can only differ in the host presenter
(OpenGL output). Writing a different layout/camera into game RAM on each peer
would be treated as a desync and "healed" back to the host's state.

## Runtime observations (Linux headless build, 2P VS MAN, DEPOT)

- Battle renders at 30 fps: the display flips every 2 vblanks.
- Framebuffers are 512x240 at VRAM (0,0) and (0,240); textures live at x >= 512.
  No spare VRAM for two more full-size buffers.
- During a 2-human match: 0x801093CC = 9, 0x801093D4 = 3, layout (gp+0xA) = 1.
- Writing layout = 0 mid-match makes the world render WIDE following P1; the
  HUD is half broken because the HUD descriptor is chosen once at battle start
  (0x800282A4: 0x800F541C for wide, 0x800F945C for split -> func_800151E0).

## Plan: per-player WIDE in netplay

Both peers must stay byte-identical, so both run the same modified game:

1. In a netplay 2-human battle, use the WIDE layout and HUD.
2. Render two frames per 30 fps logic tick: P1's wide view, flip, P2's wide view,
   flip (60 Hz total, 30 fps per player). Camera + HUD target alternate per view.
   Record which player each displayed buffer belongs to in a spare RAM byte.
3. Presentation only (allowed to differ per peer): register a retained-scene
   predicate that HOLDs the presenter while the displayed buffer belongs to the
   other seat (psx_netplay_local_slot()), so each window only ever shows its
   own player's view.

## Battle frame loop (psxrecomp 060ef0ab build, 2P VS MAN, DEPOT)

Main loop 0x80014C04..0x80014F88 (one iteration = one 30 fps tick):

1. `db = *(s16*)0x8013855C` -> gp+0x934; packet buffer select 0x800A4D68.
2. ClearOTagR (0x800A4AF8) on 4 ordering tables, entry stride 0x14 by db:
   A 0x80119948, B 0x80119970, C 0x80119998, D 0x80119A10.
3. Task loop 0x8001571C: 18 slots at 0x80119AD8 (stride 12: fn +0, flags +5,
   bit 2 = run). Then object list 0x80015BF0 (head gp+0x944, fn at +8, next +4).
4. DrawSync(0) 0x8009FED4, then **VSync(gp+1) at 0x80014CCC** (gp+1 = 2).
5. SetDispMask(1) 0x8009FB58, **flip 0x800A3E88** (PutDispEnv from per-buffer
   table 0x801384C0, frame counter 0x80138558, toggles db).
6. Clear colour 0x800A3850, then DrawOTag (0x800A4AC8) of A, B, C, D
   (with small text draws 0x800A3B68/0x800A3C58 in between).

So at the VSync entry of tick n+1, tick n+1's logic is done and its OTs are
built but not drawn, while frame n is complete in the back buffer and is shown
by the next flip: render-pass flip mode **PENDING**.

Heavy per-tick work: task 10 (0x8003C638, map) and task 11 (0x8003EA34,
objects/characters), ~1000 calls each.

## Cameras (libgs)

`GsSetRefView2` = 0x800A4D88. Two GsRVIEW2 views, set alternately while each
split half is drawn:

| View | GsRVIEW2 | super (GsCOORDINATE2) |
|---|---|---|
| P1 | 0x80119830 | 0x801197E0 |
| P2 | 0x801198E4 | 0x80119894 |

Both views: vp = (0, 0, -10000), vr = 0, rz = 0; the camera follows its player
through the super coordinate's translation (coord.t at +0x18), with the same
fixed rotation matrix. GsSetProjection-like call 0x800A4A98 (0xA10/0xA60 for
the map, 0x400/0x400 for objects).

## Which tasks only draw (render-pass probe)

`src/mods/tg_frame_rate_plugin.cpp` (stage 1) runs one task per sample inside
a discarded render pass at the battle VSync and counts changed RAM per 4 KiB
page. Results (2P DEPOT, one sample each):

| Task | Fn | Writes besides stack / gp / packet area |
|---|---|---|
| 0, 1, 7, 8, 9, 15, 16 | various | none this tick |
| 6 | 0x8003A82C | 0x8015A000 / 0x8015D000 (80 words each) |
| 10 map | 0x8003C638 | libgs caches 0x80102000, 0x8010D000-E000, 0x80115000-6000, 0x80138000; packets 0x801E7000-0x801EC000 |
| 11 objects | 0x8003EA34 | GsCOORDINATE2 work matrices from 0x80125234 (stride 0x50), libgs packet pointers 0x80109F00 / 0x80112000, packets 0x801B7000-0x801D5938, draw counter gp+0xA74 |
| 12, 13, 14 | 0x800368C8, 0x8005EA10, 0x80037494 | packets only |
| 2-5, 17 | | not sampled (17 waits on VSync inside the task: never run it in a pass) |

Task 11 draws per view with 0x8003C8B4(player, view) twice, then increments
the draw counter gp+0xA74 (blink phase for traps/markers: 0x8003CCF0 /6,
0x8003CF58 /4). Map and objects therefore only draw; their RAM writes are
libgs caches and packets, so they can be replayed in a pass with blended
camera / character coordinates.

## Stage 2a: unblended redraw works

`replay_pass` (env `TG_REPLAY=1` while the package is enabled) redraws the
frame inside a render pass from the current (tick n+1) state:

1. `GsSetWorkBase` 0x800A4D68 with the per-buffer base table 0x801093A0[db]
   (db = gp+0x934); ClearOTagR the four OTs.
2. Run draw tasks 6, 10, 11, 12, 13, 14 (gp+0x93C = slot, as the dispatcher).
3. Replay the battle loop's own submit span 0x80014CEC..0x80014F48 with
   s1 = OT B, s2 = OT C, s3 = OT A, s4 = OT D: background colour, then the
   four OTs with per-view draw offset/clip between them (0x800A3B68 offset,
   0x800A3C58 clip RECT): view 1 centre (184, 60), view 2 (184, 180), HUD
   (256, 120).
4. DrawSync.

Result (render_pass_dump): both halves, both characters and the HUD match the
game's frame layout; passes 10/10, aborts 0, span failures 0. Next: snapshot
moving state each tick and blend it before step 2.

Movement probe (P1 holding right): camera anchor 0x80119894 coord.t.z follows
the moving character (+7/tick); bone work matrices from 0x80125234 (stride
0x50) carry world positions; more decoding of the player object needed.

## Player records and stage 2b blending

Players: 0x80124EB0 + 0xA08 * n (n = 0, 1). World position int32 x/y/z at
+0x888 / +0x88C / +0x890 (pad 1 walking: z +104/tick). The camera follow at
0x8002BE20 anchors camera coordinate 0x801197E0 + 0xB4 * n from it
(coord.t = pos + (-0x64, -0x180, 0)).

Stage 2b blends both player positions and both camera anchors linearly from
the previous tick to the current one before the redraw (teleport guard 2000
units/tick). Forced phases 0.25 / 0.5 / 0.75 (`TG_TEST3=1`) show the walking
character and the camera advancing evenly between the game's frames; passes
282, aborts 0. Not yet blended: facing/rotation, animation pose,
projectiles, traps and effects.

## Per-player WIDE online: design (after R4's Link Battle)

Reference: tetrisgm/RidgeRacerType4Recomp docs/ONLINE_BATTLE.md and
src/mods/r4_link_netplay.c. Every peer runs and draws the same stock split
frame (RAM, VRAM, digests identical). At the battle VSync each peer redraws
its own seat's view inside `psx_mod_render_local_view` (sandboxed, presenter
only). Hooks must be registered with `psx_game_register_netplay_function_entry`
(package mods are off in netplay).

Own-view draw (prototype, `TG_OWN_VIEW=<seat>` with `TG_TEST3=1` offline):
upload the WIDE HUD TIM (0x800151E0 loads a TIM to VRAM: 0x800F541C wide,
0x800F945C split), layout byte gp+0xA = 0, view player bytes 0x80108A9C/D =
(seat, other), then the draw tasks and the submit span. First result: the
world draws in the wide layout and the WIDE top HUD bar appears, but split-HUD
elements are still drawn and the seat swap has no effect yet (both seats show
the same image). Next: find what else the battle setup derives from the layout
(0x80028D44..: HUD state per layout) and the real per-view player index.

## Online per-player WIDE: implemented (src/mods/tg_netplay_view.cpp)

Findings: in a 2-human battle started in the WIDE layout (forced at the
setup store 0x80028DB4 with the test hook `TG_FORCE_WIDE=1`) the game draws
player 1's full view with the proper WIDE HUD. Swapping the view-player bytes
0x80108A9C / 0x80108A9D makes the camera and HUD follow player 2. Battle setup
only uses the layout to pick the HUD TIM; the battle tasks read it when they
start, so the layout must be WIDE before they are created.

Implementation (game-owned netplay hooks, inactive offline):
- Entry of the TIM loader 0x800151E0 from battle setup (ra 0x80028EB8 /
  0x800282C4) with the split TIM: set the layout byte to 0 and load the WIDE
  TIM instead. Every peer does the same, so the shared frame is the WIDE view
  of player 1 on all peers.
- Battle VSync (ra 0x80014CD4), VS battle (0x801093CC = 9) with two humans
  (gp+0xC = gp+0xD = 1), layout WIDE, local slot 1: `psx_mod_render_local_view`
  redraws the frame with the view-player bytes swapped (draw tasks + submit
  span). Slot 0 shows the shared frame.

Verified with two local peers (LAN CLI): delay-sync and rollback, VS MAN on
DEPOT, both players moving: 0 desync, 751 / 646 own views, 0 aborts, 0 device
reads. present_shot shows each window with its own player's WIDE view.

### Fix: overlapping HUD panels online (v0.0.4)

WIDE submit path (gp+0xE = 0, 0x80014DA0): draws the HUD panel OT of player
0x80108A9C at offset (256, 144), then, if that player's human flag
(0x80108A80[0x80108A9D]) is set, the other player's panel at (308, 40)
(DrawOTag call 0x80014F0C, ra 0x80014F14). Single-player WIDE never draws the
second panel (the other player is the CPU). Online both are human, so the two
panels overlapped. A netplay filter on DrawOTag (0x800A4AC8) consumes that
call in a WIDE 2-human battle; every peer does the same (state stays
identical) and the own view skips the opponent's panel the same way.
Verified: rollback, 0 desync; each window shows only its own panel.
