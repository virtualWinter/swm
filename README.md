# swm

A minimal, floating-only Wayland compositor — a port of
[sowm](https://github.com/virtualWindows/sowm) (Simple/Shitty Opinionated
Window Manager) to [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots).

It keeps sowm's philosophy: no tiling, no tabs, no panels — just windows you
move and resize yourself, with focus-follows-cursor and a handful of
keybindings. Everything is configured through a single text file in your
XDG config directory (no recompiling needed).

## Features

- Floating-only layout (no tiling, no workspaces-from-tiling)
- 6 virtual workspaces
- Focus-follows-cursor
- `MOD` + left-drag to move, `MOD` + right-drag to resize
- Fullscreen toggle (with an optional top gap for a status bar)
- Center / kill / alt-tab window cycling
- Key-launched programs
- Fully runtime-editable configuration via XDG

## Dependencies

- A C compiler and `pkg-config`
- `wlroots` (the Makefile is pinned to the `wlroots-0.20` pkg-config name)
- `wayland-server`
- `xkbcommon`
- A seat/session provider — `libseat` (with `seatd`) or logind

On a Debian/Ubuntu-like system:

```sh
sudo apt install build-essential pkg-config \
  libwlroots-dev wayland-protocols libxkbcommon-dev libseat-dev
```

If your wlroots installs under a different pkg-config name (e.g. `wlroots`),
edit `PKGS` in the `Makefile`.

## Build & install

```sh
make            # builds ./build/swm
make install        # installs to ~/.local/bin/swm (PREFIX overridable; no root needed)
```

Other targets: `make clean`, `make run` (build and execute).

## Running

`swm` needs a Wayland session/seat. The easiest ways to try it:

- **Nested** (from inside an existing Wayland session):

  ```sh
  ./build/swm
  ```

  It will open as a Wayland client window you can poke at.

- **From a TTY**: make sure `XDG_RUNTIME_DIR` is set and a seat manager is
  running (`seatd` or a logind session), then launch `swm` from a
  console/display manager. This takes over the display.

`swm` prints the socket it listens on, e.g. `swm: running on
WAYLAND_DISPLAY=wayland-1`. Point clients at it with `WAYLAND_DISPLAY`.

## Configuration

Configuration lives at:

```
$XDG_CONFIG_HOME/swm/swm.conf      (falls back to ~/.config/swm/swm.conf)
```

On first run, `swm` creates this file with sensible defaults, so you always
have a template to edit. Changes take effect after restarting `swm` (there is
no live reload).

Lines are `key = value` directives; `#` starts a comment.

### Tunables

| Setting         | Meaning                                                   | Default     |
|-----------------|-----------------------------------------------------------|-------------|
| `mod`           | Primary modifier, used by the `MOD` alias in bindings     | `SUPER`     |
| `border_width`  | Focus border thickness in pixels (0 = borderless)         | `1`         |
| `gap`           | Pixels reserved at the top (e.g. for a bar); fullscreen   | `0`         |
|                 | windows shrink by this amount so the bar stays visible    |             |
| `border_normal` | Border color of unfocused windows (hex)                   | `#333333`   |
| `border_select` | Border color of the focused window (hex)                  | `#880000`   |

### Keybindings

Binding syntax:

```
<MODS>+<KEY> = <action> [args]
```

- **Modifiers** (joined with `+`): `SUPER`/`LOGO`, `ALT`/`MOD1`, `SHIFT`,
  `CTRL`/`CONTROL`, `MOD2`/`MOD3`/`MOD5`, and `MOD` (the `mod =` alias above).
- **Keys** use xkbcommon names and are case-insensitive: `q`, `Return`,
  `Tab`, `1`–`6`, `XF86AudioRaiseVolume`, etc. Bindings are matched on the
  *base* (unshifted) key, so `SUPER+SHIFT+1` works even though Shift makes the
  key produce `!`.

### Actions

| Action                 | Args        | Description                              |
|------------------------|-------------|------------------------------------------|
| `kill`                 | —           | Close the focused window                 |
| `center`               | —           | Center the focused window on its output  |
| `fullscreen`           | —           | Toggle fullscreen                        |
| `next` / `prev`        | —           | Cycle focus forward / backward (alt-tab) |
| `workspace <n>`        | `1`–`6`     | Switch to workspace *n*                  |
| `send <n>`             | `1`–`6`     | Move focused window to workspace *n*     |
| `exec <cmd...>`        | command     | Launch a program (whitespace-separated, `"quotes"` allowed) |

### Example

```ini
mod = SUPER

border_width = 2
gap = 0
border_normal = #333333
border_select = #880000

SUPER+q        = kill
SUPER+c        = center
SUPER+f        = fullscreen
ALT+Tab        = next
ALT+SHIFT+Tab  = prev

SUPER+d        = exec srun
SUPER+Return   = exec st
SUPER+SHIFT+1  = send 1

XF86AudioRaiseVolume = exec amixer sset Master 5%+
```

## Default keybindings

| Binding             | Action                          |
|---------------------|---------------------------------|
| `Super+q`           | Kill focused window             |
| `Super+c`           | Center window                   |
| `Super+f`           | Toggle fullscreen               |
| `Alt+Tab`           | Next window                     |
| `Alt+Shift+Tab`     | Previous window                 |
| `Super+d`           | `exec srun` (the launcher in `../srun`) |
| `Super+w`           | `exec bud ~/Wallpapers`         |
| `Super+p`           | `exec scr`                      |
| `Super+Return`      | `exec st`                       |
| `XF86Audio*`        | Volume up/down/mute (`amixer`)   |
| `XF86MonBrightness*`| Brightness up/down (`bri`)      |
| `Super+1`…`Super+6` | Switch to workspace 1–6         |
| `Super+Shift+1`…`6` | Send focused window to ws 1–6   |

`Super` here means the `MOD` key (Logo / Windows key by default).

## Mouse controls

- Move the pointer over a window to focus it.
- `MOD` + left button drag — move the window.
- `MOD` + right button drag — resize the window.

## Project layout

```
src/swm.h     Shared types, globals, and function prototypes
src/config.c  Runtime config loader/parser (XDG config file)
src/client.c  Client lifecycle, focus, window ops, workspaces, xdg toplevel
src/input.c   Keyboard and pointer/cursor handling
src/output.c  Output lifecycle and frame commits
src/main.c    Globals, helpers, and compositor setup/teardown
Makefile      gcc + pkg-config build
```

## Notes

- This is a port of sowm's logic to wlroots; credit for the design goes to the
  sowm authors. `swm` targets wlroots 0.20 and uses unstable wlroots APIs
  (`-DWLR_USE_UNSTABLE`).
- Only XDG-shell toplevel clients are supported (no X11/XWayland, no layer-shell
  panels — use the `gap` setting if you want to leave room for an external bar).
