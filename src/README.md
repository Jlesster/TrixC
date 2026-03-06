# Trixie — C Wayland Compositor

Trixie rewritten in C on top of **wlroots** (tinywl-style), ditching the Smithay
Rust stack entirely. The rendering path is now:

```
wlr_scene  (scene graph, damage tracking, hardware planes)
    ↓
DRM/KMS  (wlroots handles buffer age, scanout, vblank)
```

No more `reset_buffers()` hacks. No more fighting Smithay's diff renderer.
wlroots' scene graph handles all of that correctly out of the box.

## Architecture

| File | Responsibility |
|------|---------------|
| `trixie.h` | All types, struct declarations |
| `main.c` | wlroots init, event loop, input, output, shell handlers, action dispatch |
| `twm.c` | Tiling window manager: panes, workspaces, scratchpads, focus, reflow |
| `layout.c` | BSP, Columns, Rows, ThreeCol, Monocle layout algorithms |
| `anim.c` | Per-pane animations (slide, scale-pop, morph) + workspace slide transitions |
| `config.c` | Config file parser + hot-reload |
| `bar.c` | Status bar: pixman CPU rendering into wlr_scene_buffer nodes |
| `deco.c` | Window borders: wlr_scene_rect nodes updated each frame |
| `ipc.c` | IPC command dispatcher (same commands as Rust version) |
| `trixiectl.c` | CLI IPC client |

## Why the rendering now works

The Rust/Smithay version needed `reset_buffers()` every frame to force
`render_frame` to bind the GBM FBO, then `flush_chrome()` drew the bar/borders
into it with raw GL calls. This was fragile: damage tracking would sometimes
skip the bind and chrome went to FBO 0.

With wlroots' scene graph:

- The **bar** is a `wlr_scene_buffer` node — rendered into via pixman, uploaded
  as a wl_buffer. wlroots handles placement and damage automatically.
- **Borders** are `wlr_scene_rect` nodes — zero-copy, GPU-composited colored
  quads. wlroots places them correctly in the scene and only repaints on change.
- **Windows** are `wlr_scene_xdg_surface` nodes — wlroots wires up the
  surface commits, buffer imports, and damage regions automatically.
- **Animations** move scene nodes (`wlr_scene_node_set_position`) — wlroots
  repaints only the dirty regions.

## Features (parity with Rust version)

- ✅ BSP / Columns / Rows / ThreeCol / Monocle layouts
- ✅ Animated open/close (slide from nearest edge)
- ✅ Float open/close (scale-pop with overshoot)
- ✅ Scratchpad drop/fly-back animation
- ✅ Layout morph animations
- ✅ Workspace slide transitions (left/right)
- ✅ Floating windows (Super+drag to move/resize)
- ✅ Fullscreen
- ✅ Smart gaps
- ✅ Multi-workspace (configurable count)
- ✅ Scratchpads
- ✅ Window rules (float, fullscreen, workspace)
- ✅ XDG decorations (server-side forced)
- ✅ Status bar (workspaces, clock, layout, battery, network)
- ✅ Keybinds (configurable, same syntax)
- ✅ Hot-reload config (inotify watch)
- ✅ IPC socket + `trixiectl` CLI
- ✅ VT switching (Ctrl+Alt+F1-F12)
- ✅ XCursor theme
- ✅ exec_once / exec
- ✅ Layer shell (external bars, notifications)
- ✅ Primary selection
- ✅ DMABUF passthrough (via wlroots)

## Build

### Dependencies (Arch)
```
pacman -S wlroots wayland wayland-protocols libxkbcommon pixman meson
```

### Dependencies (Ubuntu/Debian)
```
apt install libwlroots-dev wayland-protocols libxkbcommon-dev libpixman-1-dev meson
```

### Build
```bash
meson setup build
ninja -C build
sudo ninja -C build install
```

### Run
```bash
# From a TTY (no display server needed):
./build/trixie

# Or with custom config:
TRIXIE_CONFIG=~/.config/trixie/trixie.conf ./build/trixie
```

## IPC

```bash
# Same commands as the Rust version:
trixiectl workspace 3
trixiectl focus left
trixiectl layout next
trixiectl float
trixiectl scratchpad term
trixiectl spawn foot
trixiectl reload
trixiectl quit
trixiectl status
trixiectl status_json
```

## Config

Copy `trixie.conf.example` to `~/.config/trixie/trixie.conf`. Config is
hot-reloaded when the file changes (inotify).

## wlroots version

Built and tested against wlroots **0.18**. The `meson.build` requests
`wlroots-0.18`. For 0.17 you may need to adjust a few API names
(`wlr_scene_output_layout_add_output`, `wlr_output_state_init`).
