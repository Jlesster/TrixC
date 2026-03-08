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
- **Neovim integration** — live connection to nvim via Unix socket for LSP diagnostics, buffer info, and quick-jump to errors

All accessible via a hotkey — no external apps needed.

---

## 2. Current State (v0.3.0-dev)

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
- [x] **Neovim panel** — msgpack-RPC socket connection to nvim for LSP diagnostics, buffer info, and quick-jump

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
- [ ] **CRITICAL: nvim socket crash** — compositor crashes to tty when nvim socket connects at startup (see below)

---

## 2.5. Current Critical Issue: nvim Socket Crash

### Symptom

When Trixie starts with a configured `nvim_socket` path (e.g., `/tmp/trixie-nvim.sock`), the compositor crashes to the TTY shortly after attempting to connect to the nvim socket. The crash occurs during startup, typically 2 seconds after the compositor initializes (the nvim connect timer fires at startup).

### Environment

- **Trigger**: `nvim_connect()` called from `nvim_connect_timer_cb()` in `main.c:225-226`
- **Socket path**: Configured via `overlay nvim_socket` in `trixie.conf`
- **nvim spawn**: nvim is spawned with `--listen /tmp/trixie-nvim.sock` via `nv_spawn()` in `nvim_panel.c:778-811`

### Investigation Status

**Root cause unknown.** The crash may be related to:

1. **Thread safety**: The reader thread in `nvim_panel.c` (`nv_reader_fn`) creates a `dup()`'d file descriptor and uses `poll()` with a 200ms timeout. Something about this may conflict with the compositor's event loop.

2. **FD leak or conflict**: When nvim connects, there may be a file descriptor issue — possibly related to how the socket is created, connected, or how `dup()` is handled.

3. **Event loop interference**: The nvim reader thread runs independently from the Wayland event loop. If it triggers something that interacts with the compositor's state (e.g., via IPC callbacks like `overlay_nvim_state`), there could be a race condition.

4. **nvim socket timing**: nvim may not be ready to accept connections immediately, or the socket may be in an inconsistent state when `nvim_connect()` is called.

### Code Paths Involved

- `src/main.c:223-229` — `nvim_connect_timer_cb()` — timer callback that triggers connect at startup
- `src/nvim_panel.c:529-589` — `nvim_connect()` — socket connection and reader thread creation
- `src/nvim_panel.c:491-523` — `nv_reader_fn()` — reader thread that polls the socket
- `src/nvim_panel.c:638-653` — `overlay_nvim_state()` — bridge receiver called from IPC when nvim plugin pushes state

### Debugging Suggestions

1. **Add logging**: Instrument `nvim_connect()`, `nv_reader_fn()`, and the main event loop to see where the crash occurs
2. **GDB/valgrind**: Run under gdb or valgrind to capture the crash location
3. **Delay connect**: Try increasing the initial timer delay or connecting only after the compositor is fully idle
4. **Simplify**: Temporarily disable the reader thread and see if the crash persists (just connect without reading)
5. **Check nvim state**: Verify nvim is actually running and the socket exists before connecting

---

## 3. Roadmap / Vision

### Phase 1: Stabilization (v0.3.x)

**Goal**: Fix critical bugs, complete incomplete features.

1. **FIX: nvim socket crash**
   - Investigate and fix the crash that occurs when nvim socket connects at startup
   - Likely root causes: thread safety, FD management, event loop race conditions
   - See Section 2.5 for detailed debugging notes

2. **Complete IPC Event System**
   - Implement proper event push to subscribers
   - Focus change notifications
   - Workspace change notifications
   - Title change notifications

3. **Complete Config Hot-Reload**
   - Implement inotify-based file watching
   - Signal compositor on config change

4. **Polish Window Management**
   - Fix window rule matching edge cases
   - Complete urgency UI in bar
   - Improve floating window behavior

5. **Memory & Performance**
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
├── nvim_panel.c    # Neovim msgpack-RPC integration, LSP diagnostics
├── anim.c          # Animation engine
├── ipc.c           # IPC socket handling
├── lua.c           # Lua scripting
├── files_panel.c   # File browser panel
├── run_panel.c     # Command runner panel
├── lsp_panel.c     # LSP client panel
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

### Neovim Integration

Trixie connects to nvim via a Unix socket (default: `/tmp/trixie-nvim.sock`) using msgpack-RPC:

- **Poll mode**: Overlay polls nvim every N ms for buffer name, cursor position, and LSP diagnostics
- **Push mode**: nvim plugin pushes state via IPC to Trixie (preferred)
- **Protocol**: Custom msgpack encoding in `nvim_panel.c`, handles nvim's msgpack-RPC responses
- **nvim spawn**: Press 's' in the nvim panel to spawn nvim with `--listen` socket

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

*Document Version: 0.2*  
*Last Updated: 2026-03-08*
