# Dual screen (local versus, a monitor per player)

One PC, two monitors, two controllers, no network. Turn on **Dual Screen**
in the launcher's Mods page and start the game: a second window opens for
player 2.

- **Where it opens:** borderless fullscreen on your other monitor, or beside
  the main window when there is only one monitor.
- **Moving it:** drag it like any window; **double-click** it to switch
  between fullscreen and windowed on the monitor it is on.
- **Closing it:** puts the game back on one screen until the next start.
  Closing the main window quits the game as usual.

What each screen shows:

| Moment | Main window | Second window |
|---|---|---|
| 2-player versus battle | player 1's own full-screen WIDE view | player 2's own full-screen WIDE view |
| 1-player fight | player 1's WIDE view | the CPU opponent's WIDE view |
| Menus, movies, result screens | the game | the same picture (mirror) |

How it works: the game runs once. At the end of every battle frame the
game's own draw code draws the frame a second time for the other player,
inside the framework's reversible "local pair" sandbox
(`psx_mod_render_local_pair`), so nothing about the game's state changes and
there is no added input lag. 2-player battles use the one-player WIDE layout
(the same hooks as online versus, `src/mods/tg_netplay_view.cpp`).

Code:

- `src/mods/tg_dual_screen.cpp` - the mod (battle layout, both views).
- `ui/tg_dual_main.inc` - the second window (open, place, fullscreen, close).
- `ui/tg_dual_gl.inc` - drawing into it; split of the local pair across the
  two windows instead of side by side in one.
- `cmake/tg_framework_overlay.cmake` - where those hook into copies of the
  framework's `main.cpp` and `gpu_gl_renderer.c`.

Limits for now:

- OpenGL renderer only.
- The High Frame Rate mod's in-between frames are off while this is on.
- Player 2's view has the same missing overlay pieces as online player 2
  (trap icon cluster, stage name/clock, round banner).
- One audio stream for both players.
- Needs SDL3 (the framework's default).
