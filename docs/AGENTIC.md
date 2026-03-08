# Trixie — Agentic Specification Document

## 1. Project Overview

**Trixie** is a modern Wayland compositor written in C, built on top of wlroots. It combines a tiling window manager with an embedded **TUI (Terminal User Interface) development suite**, making it a self-contained environment for both desktop computing and software development.

### Core Identity

- **Type**: Wayland Compositor (not X11)
- **Language**: Pure C (no Rust, no Python)
- **Build System**: Meson
- **Window Model**: Tiling (BSP, Spiral, Columns, Rows, ThreeCol, Monocle)
- **Embedded Tools**: Full TUI overlay with dev tools

### What Makes Trixie Unique

Unlike other compositors (Sway, Hyprland, River) that delegate tooling to external processes, Trixie embeds its development environment directly into the compositor. The **TUI Overlay** provides:

- Workspace visualization
- Command palette with fuzzy search
- Process monitoring
- Log viewer (wlr logs)
- Git integration
- Build system
- Notes

All accessible via a hotkey — no external apps needed.

---

## 2. Current State (v0.2.1)

### What's Implemented

#### Window Management
- [x] Tiling layouts: BSP, Spiral, Columns, Rows, ThreeCol, Monocle
- [x] Floating windows with drag/resize
- [x] Fullscreen mode
- [x] Workspace system (1-9 workspaces)
- [x] Scratchpads (hidden windows)
- [x] Window decorations (title bars, borders)
- [x] Per-workspace layout/ratio overrides
- [x] Window rules (float, noborder, opacity, workspace assignment)

#### Compositor Foundation
- [x] wlroots-based Wayland compositor
- [x] XWayland support (optional, compile-time)
- [x] Multi-monitor support
- [x] Cursor themes and shapes
- [x] Output management
- [x] Input handling (keyboard, pointer)
- [x] Foreign toplevel management (for taskbars/launchers)

#### TUI Overlay (Development Suite)
- [x] Workspace map panel
- [x] Command palette with fuzzy search
- [x] Process list (CPU/memory from /proc)
- [x] Log viewer (wlr log ring buffer)
- [x] Git panel (branch, status, recent commits)
- [x] Build panel (run build commands)
- [x] Notes panel (persisted scratchpad)

#### Bar
- [x] Built-in status bar (wlr-layer-shell)
- [x] Module system (workspaces, layout, clock, battery, network, volume, cpu, memory)
- [x] Custom exec modules
- [x] Powerline separators
- [x] Theme integration

#### Configuration
- [x] Config file parsing (~/.config/trixie/trixie.conf)
- [x] Theme presets (Catppuccin Mocha/Latte, Gruvbox, Nord, Tokyo Night)
- [x] Hot reload
- [x] Lua scripting (runtime customization)
- [x] IPC socket for external control

#### Animation
- [x] Window open/close animations
- [x] Float open/close animations
- [x] Scratchpad animations
- [x] Workspace transitions
- [x] Morph animations
- [x] Fade in/out

### What's Broken / Missing

- [ ] **Inotify config watching** — file changes not detected (stub exists)
- [ ] **IPC event push** — subscribers don't receive events
- [ ] **Proper fractional scaling** — implemented but may have issues
- [ ] **Window rule exact matching** — may have edge cases
- [ ] **Bar urgency dots** — ws_urgent_mask implemented but UI incomplete

### Build & Dependencies

- wlroots (from source or package)
- wayland
- pixman-1
- libinput
- xkbcommon
- libseat
- libdisplay-info
- libdrm
- OpenGL/ES2 ( Mesa)
- Freetype2
- Meson + Ninja

---

## 3. Roadmap / Vision

### Phase 1: Stabilization (v0.3.x)

**Goal**: Fix critical bugs, complete incomplete features.

1. **Complete IPC Event System**
   - Implement proper event push to subscribers
   - Focus change notifications
   - Workspace change notifications
   - Title change notifications

2. **Complete Config Hot-Reload**
   - Implement inotify-based file watching
   - Signal compositor on config change

3. **Polish Window Management**
   - Fix window rule matching edge cases
   - Complete urgency UI in bar
   - Improve floating window behavior

4. **Memory & Performance**
   - Profile and reduce memory footprint
   - Optimize render pipeline

### Phase 2: Feature Parity (v0.4.x)

**Goal**: Match features of mature compositors.

1. **Enhanced Layouts**
   - Add automatic layout switching based on pane count
   - Layout presets per workspace

2. **Improved Bar**
   - More modules (dates, weather, disk, custom)
   - Clickable modules (workspace switch, exec)
   - Multiple bars (per-output)

3. **Better Keybinds**
   - Mouse binds (bindmovemouse, bindresizemouse)
   - Chords / sequences
   - Device-specific binds

4. **IPC Expansion**
   - JSON output for all commands
   - Better scripting support
   - External module support

### Phase 3: Development Suite (v0.5.x)

**Goal**: Make Trixie the "ultimate TUI dev environment."

1. **Command Palette Expansion**
   - Search files in project
   - Quick jump to symbols
   - Recently used commands

2. **Build Panel Enhancement**
   - Detect build systems (Makefile, meson, cmake)
   - Parse errors/warnings, click to jump
   - Build history

3. **Process Manager**
   - Kill processes
   - Resource graphs (CPU/memory over time)
   - Sort/filter processes

4. **Log Viewer Enhancement**
   - Filter by component
   - Regex search
   - Export logs

5. **Git Panel Expansion**
   - Show staged/unstaged diffs
   - Quick stage/unstage
   - Branch graph

6. **Notes Expansion**
   - Markdown support
   - Multiple notes
   - Searchable

### Phase 4: Extensibility (v0.6.x)

**Goal**: Plugin system and external contributions.

1. **Plugin API**
   - C plugins via shared objects
   - Lua API expansion
   - IPC plugin protocol

2. **External Module System**
   - Third-party bar modules
   - External overlay panels

3. **Community Features**
   - Input methods
   - Color picker
   - Screenshot/recording integration
   - Multi-seat support

---

## 4. Architecture Notes

### Code Organization

```
src/
├── main.c          # Entry point, server setup
├── config.c        # Config parsing, themes
├── twm.c           # Tiling logic (workspaces, panes, layouts)
├── layout.c        # Layout algorithms
├── deco.c          # Window decorations
├── bar.c           # Bar rendering
├── bar_worker.c    # Async module polling
├── overlay.c       # TUI development overlay (1600+ lines)
├── anim.c          # Animation engine
├── ipc.c           # IPC socket handling
├── lua.c           # Lua scripting
└── trixiectl.c     # CLI client (separate binary)

include/
└── trixie.h        # All public types, structs, declarations
```

### Key Design Decisions

1. **Single Process**: Everything runs in one process (unlike Sway's out-of-process bar)
2. **Async Bar Workers**: Modules poll in separate threads
3. **wlr_scene**: Uses wlroots scene graph for rendering
4. **No X11** (unless XWayland): Pure Wayland compositor

### IPC Protocol

- Unix socket at `$XDG_RUNTIME_DIR/trixie.sock`
- Text-based commands (e.g., `workspace 1`, `close`)
- Reply format: `ok:` or `err:` prefix

---

## 5. Contributing

### Getting Started

```bash
# Clone
git clone https://github.com/anomalyco/trixie
cd trixie

# Build
meson setup builddir
meson compile -C builddir

# Run
./builddir/Trixie

# Config
cp examples/trixie.conf ~/.config/trixie/trixie.conf
```

### Code Style

- No comments unless asked
- Follow existing patterns in each file
- Use existing types from trixie.h
- Test before committing

### Testing

- No formal test suite yet
- Manual testing via trixiectl
- Check for memory leaks with valgrind

---

## 6. References

- **Documentation**: `man trixie`, `man trixie.conf`, `man trixiectl`
- **Protocols**: wlroots protocols in `include/protocols/`
- **Examples**: `examples/trixie.conf`

---

*Document Version: 0.1*  
*Last Updated: 2026-03-08*
