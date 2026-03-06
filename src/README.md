# Trixie — Wayland Compositor (C Implementation)

A wlroots-based Wayland compositor with tiling window management, animations,
a built-in status bar, IPC, and hot-reloadable config — a full C port of the
original Rust/Smithay implementation.

---

## Features

- **Five tiling layouts**: BSP, Columns, Rows, ThreeCol, Monocle  
- **Smooth animations**: open/close slide, float scale-bounce, layout morph,
  workspace slide — all with cubic/quint easing  
- **Built-in status bar**: workspaces, clock, layout, battery, network, volume,
  custom shell modules  
- **Scratchpads**: named floating terminals / apps toggled with a keybind  
- **Server-side decorations**: coloured borders, always server-side  
- **IPC socket**: `trixiectl` client for scripting  
- **Hot-reload**: inotify config watcher — edit and save, changes apply live  
- **Smart gaps**: collapse gaps when only one tiled window is present  

---

## Dependencies

| Library | Package (Arch) | Package (Debian/Ubuntu) |
|---|---|---|
| wlroots 0.17 or 0.18 | `wlroots` | `libwlroots-dev` |
| wayland-server | `wayland` | `libwayland-dev` |
| xkbcommon | `libxkbcommon` | `libxkbcommon-dev` |
| pixman | `pixman` | `libpixman-1-dev` |
| freetype2 | `freetype2` | `libfreetype-dev` |
| harfbuzz | `harfbuzz` | `libharfbuzz-dev` |
| wayland-protocols | `wayland-protocols` | `wayland-protocols` |

Also requires `meson`, `ninja`, `wayland-scanner`, and `pkg-config`.

### Arch Linux

```sh
pacman -S wlroots wayland libxkbcommon pixman freetype2 harfbuzz \
          wayland-protocols meson ninja
```

### Debian / Ubuntu (Trixie / 24.04+)

```sh
apt install libwlroots-dev libwayland-dev libxkbcommon-dev libpixman-1-dev \
            libfreetype-dev libharfbuzz-dev wayland-protocols \
            meson ninja-build pkg-config
```

---

## Build

```sh
meson setup build
ninja -C build
sudo ninja -C build install
```

Binaries installed: `/usr/local/bin/trixie`, `/usr/local/bin/trixiectl`

### Debug build (verbose logging)

```sh
meson setup build --buildtype=debug
ninja -C build
```

---

## Running

Start from a TTY (not inside another Wayland/X session):

```sh
trixie
```

Or launch from a display manager session script.

The compositor sets `WAYLAND_DISPLAY` automatically, so client apps spawned
from keybinds inherit it.

---

## Configuration

Config file: `~/.config/trixie/trixie.conf`  
(or `$XDG_CONFIG_HOME/trixie/trixie.conf`)

Changes are picked up automatically via inotify — no restart needed.

### Example `trixie.conf`

```
// ── General ──────────────────────────────────────────────────────────────
gap          = 6
border_width = 2
workspaces   = 9
smart_gaps   = true

// ── Colors ───────────────────────────────────────────────────────────────
colors {
    active_border   = #b4befe
    inactive_border = #45475a
    pane_bg         = #11111b
}

// ── Bar ──────────────────────────────────────────────────────────────────
bar {
    position        = bottom
    height          = 28
    bg              = #181825
    fg              = #cdd6f4
    accent          = #b4befe
    dim             = #585b70
    active_ws_fg    = #11111b
    active_ws_bg    = #b4befe
    occupied_ws_fg  = #b4befe
    inactive_ws_fg  = #585b70
    modules_left    = [workspaces]
    modules_center  = [clock]
    modules_right   = [layout, battery, network, volume]
    pill_radius     = 4
    item_spacing    = 4
}

// Custom bar module
bar_module cpu {
    exec     = "top -bn1 | grep 'Cpu' | awk '{print $2}'"
    interval = 2
    icon     = " "
    color    = #a6e3a1
}

// ── Keyboard ─────────────────────────────────────────────────────────────
keyboard {
    layout       = us
    repeat_rate  = 30
    repeat_delay = 300
}

// ── Scratchpads ───────────────────────────────────────────────────────────
scratchpad term {
    app_id = foot
    width  = 70%
    height = 60%
}

// ── Window rules ──────────────────────────────────────────────────────────
window_rule discord { float = true }
window_rule steam    { workspace = 2 }

// ── Startup ───────────────────────────────────────────────────────────────
exec_once = foot
exec_once = waybar

// ── Keybinds ──────────────────────────────────────────────────────────────
keybind = SUPER:Return,  exec,         foot
keybind = SUPER:q,       close
keybind = SUPER:f,       fullscreen
keybind = SUPER+SHIFT:space, toggle_float
keybind = SUPER+SHIFT:b, toggle_bar
keybind = SUPER:h,       focus, left
keybind = SUPER:l,       focus, right
keybind = SUPER:k,       focus, up
keybind = SUPER:j,       focus, down
keybind = SUPER+SHIFT:h, move,  left
keybind = SUPER+SHIFT:l, move,  right
keybind = SUPER:Tab,     next_layout
keybind = SUPER:equal,   grow_main
keybind = SUPER:minus,   shrink_main
keybind = SUPER+CTRL:Right, next_workspace
keybind = SUPER+CTRL:Left,  prev_workspace
keybind = SUPER:1,  workspace, 1
keybind = SUPER:2,  workspace, 2
keybind = SUPER:3,  workspace, 3
keybind = SUPER:4,  workspace, 4
keybind = SUPER:5,  workspace, 5
keybind = SUPER:6,  workspace, 6
keybind = SUPER:7,  workspace, 7
keybind = SUPER:8,  workspace, 8
keybind = SUPER:9,  workspace, 9
keybind = SUPER+SHIFT:1, move_to_workspace, 1
keybind = SUPER:d,  scratchpad, term
```

### Monitor config

```
monitor eDP-1 {
    width   = 1920
    height  = 1080
    refresh = 144
    scale   = 1.0
}
```

---

## IPC — trixiectl

```sh
trixiectl workspace 3
trixiectl next_workspace
trixiectl prev_workspace
trixiectl layout next
trixiectl layout prev
trixiectl float
trixiectl float_move 10 0
trixiectl float_resize 50 50
trixiectl scratchpad term
trixiectl close
trixiectl fullscreen
trixiectl focus left
trixiectl move_to_workspace 2
trixiectl grow_main
trixiectl shrink_main
trixiectl spawn "foot"
trixiectl reload
trixiectl status
trixiectl status_json
trixiectl quit
```

---

## Architecture

```
main.c       — wlroots init, event loop, XDG shell, cursor, keyboard, output
twm.c        — tiling window manager: panes, workspaces, scratchpads, float
layout.c     — BSP / Columns / Rows / ThreeCol / Monocle layout algorithms
anim.c       — animation engine: open/close/morph/float/scratch/workspace
bar.c        — status bar: freetype2+harfbuzz text, system modules, pixel buffer
deco.c       — window border decorations via wlr_scene_rect
config.c     — recursive-descent config parser with variable substitution
ipc.c        — Unix socket IPC command dispatcher
trixiectl.c  — standalone IPC client binary
trixie.h     — shared types and function declarations
```

---

## Emergency exit

`Super+Shift+Print` — hard-kills the compositor immediately.

---

## Differences from the Rust build

This C implementation is architecturally equivalent to the Rust/Smithay version
with these practical differences:

- Uses **wlroots** instead of Smithay (same DRM/GBM/EGL pipeline, different API)
- Bar text uses **freetype2 + harfbuzz** (same as the spec) with ARGB8888 pixmap
  uploaded via `wlr_scene_buffer`
- Custom bar modules run **synchronously** per frame (the Rust version used
  background threads); upgrade to `pthread` polling threads for heavy commands
- No LSP server (`trixie --lsp`) — that can be added as a separate binary
- `zwp_linux_dmabuf_v1` and `wp_primary_selection_unstable_v1` are provided
  automatically by wlroots
