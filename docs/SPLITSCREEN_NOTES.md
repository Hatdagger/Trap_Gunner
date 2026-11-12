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
