# Trap Gunner system menu (launcher)

The launcher opens on a controller-first menu drawn like the game's own
SYSTEM OPTIONS screen: a bronze frame around a static-filled screen, items
in green, the chosen item in blue, values in white, and the page name on the
plate at the top. D-pad or arrow keys move, A / Enter confirms or toggles,
B / Triangle / Esc goes back, X / Delete is the page's extra action, and
Start starts the game (or the match, in a room you host).

| Page | What is on it |
| --- | --- |
| Main | START GAME, NETWORK PLAY, SETTINGS, MODS, EXIT; disc, controller, memory card, mods and display status |
| Settings | Aspect, fullscreen, window size, resolution, texture filter, vsync, volume, skip this menu, button layout, all settings, restore defaults |
| Mods | Every installed mod as an ON/OFF line; packages dropped in `mods/new` beside the game, ready to install from the controller; browse, open the folder, full mods page; X removes a mod |
| Network play | LAN or online, and the name other players see (left/right picks one of the cast, A types one) |
| Room list | Host a room with one press, join a listed room, join by address (dialled a number at a time), host with a name or password, full network page |
| Room | 1P and 2P, ready state, start match, room settings and mods, leave (Back goes to LEAVE ROOM first) |

The controller page, online sign-in, setup and every dialog are still the
stock launcher's, reached from these pages and left with their own Back.

## Runtime defaults

* **Disc.** On first run, or if the remembered image has moved, the runtime
  looks beside the executable (and in `disc`, `discs`, `iso`, `roms` or
  `games` there) for an image whose disc serial is SLUS-00679 and uses it.
  A `.cue` or `.chd` is preferred over a bare image. `--disc` is never
  overridden.
* **Steam Deck Game Mode.** The framework normally skips the launcher in
  Game Mode once a disc is set up, because its stock pages need a mouse.
  Trap Gunner's menu is controller-first, so it stays; SKIP THIS MENU in
  Settings (or `--no-launcher`) still boots straight into the game.
* **Player 1 controller.** A seat set to the keyboard also takes whatever game
  controller is connected, now or when one is plugged in; the keyboard keeps
  working beside it. Player 2 keeps its own setting.

## How it is built

The psxrecomp and recomp-ui submodules stay pinned to their upstream commits
and are never edited. `cmake/tg_framework_overlay.cmake` reads
`launcher_imgui.cpp` and the runtime's `main.cpp` from the submodules, makes
its insertions at fixed anchor lines, writes the results under
`<build>/tg_overlay/`, and compiles those copies in place of the originals.
The menu itself is `ui/tg_menu.inc` and `ui/tg_menu_net.inc`.

Each anchor has to match exactly once. If a framework bump moves one,
configure stops and prints the anchor text rather than building a launcher
that quietly lost the menu.

Pages can be captured headless with the launcher's script hook, e.g.

```
SDL_VIDEO_DRIVER=offscreen LNG_TEST_HIDDEN=1 \
LNG_SCRIPT="size:800x600;wait:20;shot:main.png;view:settings;wait:5;shot:settings.png;quit" \
./Trap_Gunner
```
