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
