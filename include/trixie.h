/* trixie.h — Wayland compositor with AwesomeWM-style Lua scripting */
#pragma once
#ifndef WLR_USE_UNSTABLE
#define WLR_USE_UNSTABLE
#endif
#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_content_type_v1.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_pointer_gestures_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_tearing_control_v1.h>
#define HAVE_TEARING_CONTROL 1
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

#ifdef HAS_XWAYLAND
#include <wlr/xwayland.h>
#include <xcb/xcb.h>
#endif

#define TRIXIE_VERSION_STR "0.5.0"

/* ── Geometry ──────────────────────────────────────────────────────────── */
typedef struct {
  int x, y, w, h;
} Rect;

static inline bool rect_empty(Rect r) { return r.w <= 0 || r.h <= 0; }
static inline bool rect_contains(Rect r, int x, int y) {
  return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}
static inline Rect rect_inset(Rect r, int n) {
  return (Rect){r.x + n, r.y + n, r.w - n * 2, r.h - n * 2};
}

/* ── Color ─────────────────────────────────────────────────────────────── */
typedef struct {
  uint8_t r, g, b, a;
} Color;
static inline Color color_hex(uint32_t rgb) {
  return (Color){(rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff, 0xff};
}
static inline Color color_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  return (Color){r, g, b, a};
}

/* ── Modifier masks ────────────────────────────────────────────────────── */
#define MOD_SUPER (1u << 0)
#define MOD_CTRL (1u << 1)
#define MOD_ALT (1u << 2)
#define MOD_SHIFT (1u << 3)

/* ── Layout ────────────────────────────────────────────────────────────── */
typedef enum {
  LAYOUT_DWINDLE = 0,
  LAYOUT_COLUMNS,
  LAYOUT_ROWS,
  LAYOUT_THREECOL,
  LAYOUT_MONOCLE,
  LAYOUT_COUNT
} Layout;

/* ── BSP Dwindle tree ──────────────────────────────────────────────────── */
#define DWINDLE_MAX_NODES 128
#define DWINDLE_NULL (-1)
typedef enum { DNODE_LEAF = 0, DNODE_CONTAINER } DNodeKind;
typedef enum { DSPLIT_H = 0, DSPLIT_V } DSplitDir;

typedef struct {
  DNodeKind kind;
  DSplitDir split;
  float ratio;
  int child[2];
  uint32_t pane_id;
  Rect rect;
  bool in_use;
} DwindleNode;

typedef struct {
  DwindleNode nodes[DWINDLE_MAX_NODES];
  int root, count;
} DwindleTree;

/* ── Pane / Workspace ──────────────────────────────────────────────────── */
#define MAX_PANES 64
#define MAX_WORKSPACES 16
#define MAX_SCRATCHPADS 16
#define MAX_WIN_RULES 64
#define MAX_KEYBINDS 512
#define MAX_MONITORS 8

typedef uint32_t PaneId;

typedef struct {
  PaneId id;
  char app_id[128];
  char title[256];
  Rect rect;
  Rect float_rect;
  bool floating, fullscreen, ontop;
  bool minimized; /* hidden from display, preserved in workspace */
  bool sticky;    /* visible on all workspaces */
  float rule_opacity;
  int ws_idx; /* which workspace this pane belongs to (-1 = scratchpad) */
  /* per-client Lua signal registry index (LUA_NOREF = none) */
  int lua_signals_ref;
} Pane;

typedef struct {
  int index;
  PaneId panes[MAX_PANES];
  int pane_count;
  PaneId focused;
  bool has_focused;
  Layout layout;
  float main_ratio;
  int gap;
  DwindleTree dwindle;
} Workspace;

/* ── Scratchpad ────────────────────────────────────────────────────────── */
typedef enum { SCRATCH_FIELD_APP_ID = 0, SCRATCH_FIELD_TITLE } ScratchField;
typedef enum {
  SCRATCH_SPEC_EXACT = 0,
  SCRATCH_SPEC_SUBSTR,
  SCRATCH_SPEC_GLOB
} ScratchSpec;
typedef struct {
  ScratchField field;
  ScratchSpec kind;
  char pat[128];
} ScratchPattern;

typedef struct {
  char name[64];
  char app_id[128];
  char exec[256];
  float width_pct, height_pct;
  PaneId pane_id;
  bool has_pane, visible;
  ScratchPattern pattern;
} Scratchpad;

typedef struct {
  char app_id[128];
  bool float_rule, fullscreen_rule, noborder, notitle;
  int forced_ws, forced_w, forced_h, forced_x, forced_y;
  float opacity;
} WinRule;

/* ── TWM state ─────────────────────────────────────────────────────────── */
typedef struct {
  Workspace workspaces[MAX_WORKSPACES];
  int ws_count, active_ws;
  Pane panes[MAX_PANES];
  int pane_count;
  int screen_w, screen_h;
  Rect content_rect, bar_rect;
  int gap, border_w, padding;
  bool smart_gaps, bar_visible;
  uint32_t ws_urgent_mask;
  Scratchpad scratchpads[MAX_SCRATCHPADS];
  int scratch_count;
} TwmState;

/* ── Signal system ─────────────────────────────────────────────────────── */
/* Each signal is a named list of Lua function refs.
 * Global signals live in the server's signal table.
 * Per-object signals live in a table stored in the Lua registry,
 * referenced from the object (e.g. Pane.lua_signals_ref). */
#define MAX_SIGNAL_NAME 64
#define MAX_SIGNALS 64
#define MAX_SIGNAL_CBS 16 /* handlers per signal name */

typedef struct {
  char name[MAX_SIGNAL_NAME];
  int fn_refs[MAX_SIGNAL_CBS]; /* LUA_NOREF = empty slot */
  int count;
} LuaSignal;

typedef struct {
  LuaSignal signals[MAX_SIGNALS];
  int count;
} LuaSignalTable;

/* ── Animation ─────────────────────────────────────────────────────────── */
typedef enum {
  ANIM_OPEN,
  ANIM_CLOSE,
  ANIM_FLOAT_OPEN,
  ANIM_FLOAT_CLOSE,
  ANIM_SCRATCH_OPEN,
  ANIM_SCRATCH_CLOSE,
  ANIM_MORPH,
  ANIM_FADE_IN,
  ANIM_FADE_OUT
} AnimKind;

typedef enum { WS_DIR_LEFT = 0, WS_DIR_RIGHT } WsDir;

typedef struct {
  AnimKind kind;
  Rect target;
  int from[4];
  int duration_ms;
  bool active;
  struct timespec start;
  float opacity_from, opacity_to;
} PaneAnim;

typedef struct {
  PaneId id;
  PaneAnim anim;
} AnimEntry;

typedef struct {
  bool active;
  WsDir dir;
  int duration_ms, screen_w;
  struct timespec start;
  float cached_e;
} WsAnim;

typedef struct {
  AnimEntry entries[MAX_PANES];
  uint32_t id_index[MAX_PANES];
  int idx_map[MAX_PANES];
  int count;
  int screen_w, screen_h;
  WsAnim ws;
} AnimSet;

/* ── Actions ───────────────────────────────────────────────────────────── */
typedef enum {
  ACTION_EXEC = 0,
  ACTION_CLOSE,
  ACTION_FULLSCREEN,
  ACTION_TOGGLE_FLOAT,
  ACTION_TOGGLE_BAR,
  ACTION_FOCUS_LEFT,
  ACTION_FOCUS_RIGHT,
  ACTION_FOCUS_UP,
  ACTION_FOCUS_DOWN,
  ACTION_MOVE_LEFT,
  ACTION_MOVE_RIGHT,
  ACTION_MOVE_UP,
  ACTION_MOVE_DOWN,
  ACTION_WORKSPACE,
  ACTION_MOVE_TO_WS,
  ACTION_NEXT_WS,
  ACTION_PREV_WS,
  ACTION_NEXT_LAYOUT,
  ACTION_PREV_LAYOUT,
  ACTION_GROW_MAIN,
  ACTION_SHRINK_MAIN,
  ACTION_SWAP_MAIN,
  ACTION_SCRATCHPAD,
  ACTION_SWITCH_VT,
  ACTION_QUIT,
  ACTION_RELOAD,
  ACTION_EMERGENCY_QUIT,
  ACTION_RESIZE_RATIO,
} ActionKind;

typedef struct {
  ActionKind kind;
  int n;
  float ratio_delta;
  char exec_cmd[256];
  char name[64];
} Action;

typedef struct {
  uint32_t mods;
  xkb_keysym_t sym;
  Action action;
} Keybind;

/* ── Gesture ───────────────────────────────────────────────────────────── */
#include "gesture.h"

/* ── Shader pipeline ───────────────────────────────────────────────────── */
#include "shader.h"

/* ── Keyboard config ───────────────────────────────────────────────────── */
typedef struct {
  char kb_layout[64], kb_variant[64], kb_options[128];
  int repeat_rate, repeat_delay;
} KeyboardCfg;

/* ── Monitor config ────────────────────────────────────────────────────── */
typedef struct {
  char name[64];
  int width, height, refresh, pos_x, pos_y;
  float scale;
  float saturation;
  bool shader_enabled;
  bool shader_set;
} MonitorCfg;

/* ── Scratchpad config ─────────────────────────────────────────────────── */
typedef struct {
  char name[64], app_id[128], exec[256];
  float width_pct, height_pct;
} ScratchpadCfg;

/* ── Bar config — minimal, Lua owns the rest ───────────────────────────── */
typedef enum { BAR_TOP = 0, BAR_BOTTOM } BarPos;

typedef struct {
  BarPos position;
  int height;
} BarCfg;

/* ── Canvas — FreeType/HarfBuzz pixel buffer exposed to Lua ───────────── */
typedef struct {
  uint32_t *px;
  int w, h;   /* buffer dimensions  */
  int stride; /* px per row (== w)  */
} Canvas;

/* ── Wibox — a Lua-owned bar/widget surface ────────────────────────────── */
#define MAX_WIBOXES 8

typedef struct TrixieWibox {
  struct wlr_scene_buffer *scene_buf;
  Canvas canvas;
  int x, y, w, h;
  bool visible;
  int lua_draw_ref; /* Lua draw callback ref          */
  BarPos position;
  bool dirty;
  lua_State *L;         /* current Lua state              */
  void **lua_ud_wb_ptr; /* &WiboxUD.wb — nulled on reset  */
} TrixieWibox;

/* ── Colors ────────────────────────────────────────────────────────────── */
typedef struct {
  Color active_border, inactive_border;
  Color active_title, inactive_title;
  Color pane_bg, background;
  Color focus_ring;
} ColorScheme;

/* ── Workspace layout override ─────────────────────────────────────────── */
typedef struct {
  Layout layout;
  float ratio;
  bool layout_set, ratio_set;
} WsCfgOverride;

/* ── Main config — thin C layer, Lua owns everything else ─────────────── */
typedef struct {
  /* Font — needed before Lua runs for wibox canvas */
  char font_path[256], font_path_bold[256], font_path_italic[256];
  float font_size;

  /* Core geometry defaults — overridable from Lua */
  int gap, outer_gap, border_width, corner_radius;
  bool smart_gaps, no_title, xwayland;

  char cursor_theme[64];
  int cursor_size;

  ColorScheme colors;
  float saturation;
  bool shader_enabled;

  int workspaces, idle_timeout;
  char seat_name[64];
  WsCfgOverride ws_overrides[MAX_WORKSPACES];
  Layout ws_layout[MAX_WORKSPACES];
  float ws_ratio[MAX_WORKSPACES];
  bool ws_layout_set[MAX_WORKSPACES];
  bool ws_ratio_set[MAX_WORKSPACES];

  BarCfg bar;
  KeyboardCfg keyboard;
  GestureConfig gestures;

  MonitorCfg monitors[MAX_MONITORS];
  int monitor_count;

  /* Win rules — also settable from Lua via trixie.rules */
  WinRule win_rules[MAX_WIN_RULES];
  int win_rule_count;

  ScratchpadCfg scratchpads[MAX_SCRATCHPADS];
  int scratchpad_count;

  Keybind keybinds[MAX_KEYBINDS];
  int keybind_count;

  char exec_once[16][256];
  int exec_once_count;
  char exec[16][256];
  int exec_count;

  char build_dir[256];
} Config;

/* ── Bar / Deco forward decls ──────────────────────────────────────────── */
typedef struct TrixieBar TrixieBar;
typedef struct TrixieDeco TrixieDeco;

/* ── Layer surface ─────────────────────────────────────────────────────── */
typedef struct TrixieServer TrixieServer;

typedef struct {
  struct wlr_layer_surface_v1 *wlr_surface;
  struct wlr_scene_layer_surface_v1 *scene_surface;
  TrixieServer *server;
  struct wl_listener map, unmap, destroy;
  struct wl_list link;
} TrixieLayerSurface;

/* ── Output ────────────────────────────────────────────────────────────── */
typedef struct {
  struct wlr_output *wlr_output;
  TrixieServer *server;
  struct wl_listener frame, request_state, destroy;
  struct wl_list link;
  struct wlr_scene_output *scene_output;
  int width, height, logical_w, logical_h;
  TrixieBar *bar;
  TrixieDeco *deco;
  TrixieWibox *wiboxes[MAX_WIBOXES];
  int wibox_count;
  bool was_animating, deco_dirty;
  /* per-output saturation shader pipeline */
  TrixieShader shader;
  /* per-output Lua signal table ref */
  int lua_signals_ref;
} TrixieOutput;

/* ── Keyboard ──────────────────────────────────────────────────────────── */
typedef struct {
  TrixieServer *server;
  struct wlr_keyboard *wlr_keyboard;
  struct wl_listener modifiers, key, destroy;
  struct wl_list link;
} TrixieKeyboard;

/* ── Drag mode ─────────────────────────────────────────────────────────── */
typedef enum { DRAG_NONE = 0, DRAG_MOVE, DRAG_RESIZE } DragMode;

/* ── View ──────────────────────────────────────────────────────────────── */
typedef struct TrixieView {
  TrixieServer *server;
  struct wlr_xdg_toplevel *xdg_toplevel;
  struct wlr_scene_tree *scene_tree;
  PaneId pane_id;
  bool mapped;
  struct wl_listener map, unmap, destroy, commit;
  struct wl_listener request_fullscreen, set_title, set_app_id;
  struct wlr_foreign_toplevel_handle_v1 *foreign_handle;
  struct wl_listener foreign_request_activate, foreign_request_close;
  struct wl_list link;
  bool is_xwayland;
  bool xw_scene_attached;   /* true once wlr_scene_surface_create has run */
  bool xw_commit_connected; /* true once xs->surface->events.{map,unmap,commit}
                               are wired */
  bool xw_surface_listeners_pending; /* set when xs->surface was NULL at
                                        new_surface time */
#ifdef HAS_XWAYLAND
  struct wlr_xwayland_surface *xwayland_surface;
#else
  void *xwayland_surface;
#endif
} TrixieView;

/* ── IPC ───────────────────────────────────────────────────────────────── */
#define MAX_IPC_SUBSCRIBERS 16

/* ── Main server struct ────────────────────────────────────────────────── */
struct TrixieServer {
  struct wl_event_source *reload_idle;
  struct wl_display *display;
  struct wlr_backend *backend;
  struct wlr_renderer *renderer;
  struct wlr_allocator *allocator;
  struct wlr_compositor *compositor;
  struct wlr_scene *scene;
  struct wlr_scene_output_layout *scene_layout;
  struct wlr_output_layout *output_layout;
  struct wlr_xdg_shell *xdg_shell;
  struct wlr_layer_shell_v1 *layer_shell;
  struct wlr_xdg_output_manager_v1 *output_mgr;
  struct wlr_xdg_decoration_manager_v1 *deco_mgr;
  struct wlr_server_decoration_manager *srv_deco;
  struct wlr_foreign_toplevel_manager_v1 *foreign_toplevel_mgr;
  struct wlr_screencopy_manager_v1 *screencopy_mgr;
  struct wlr_tearing_control_manager_v1 *tearing_mgr;
  struct wlr_cursor *cursor;
  struct wlr_xcursor_manager *xcursor_mgr;
  struct wlr_seat *seat;
  struct wlr_idle_inhibit_manager_v1 *idle_inhibit_mgr;
  struct wlr_pointer_gestures_v1 *pointer_gestures;
  struct wlr_pointer_constraints_v1 *pointer_constraints;
  struct wlr_relative_pointer_manager_v1 *relative_pointer_mgr;
  struct wlr_xdg_activation_v1 *xdg_activation;
  struct wlr_cursor_shape_manager_v1 *cursor_shape_mgr;
  struct wlr_fractional_scale_manager_v1 *fractional_scale_mgr;
  struct wlr_presentation *presentation;
  struct wlr_scene_rect *bg_rect;
  struct wlr_session *session;

  struct wlr_scene_tree *layer_background;
  struct wlr_scene_tree *layer_windows;
  struct wlr_scene_tree *layer_chrome;
  struct wlr_scene_tree *layer_floating;
  struct wlr_scene_tree *layer_chrome_floating;
  struct wlr_scene_tree *layer_overlay;

  struct wl_listener new_output, new_input, new_xdg_surface, new_xdg_popup;
  struct wl_listener new_layer_surface, new_deco;
  struct wl_listener cursor_motion, cursor_motion_abs;
  struct wl_listener cursor_button, cursor_axis, cursor_frame;
  struct wl_listener seat_request_cursor, seat_request_set_selection;
  struct wl_listener seat_request_set_primary_selection;
  struct wl_listener seat_request_start_drag, seat_start_drag;
  struct wl_listener new_pointer_constraint, xdg_activation_request;
  struct wl_listener cursor_shape_request;
  struct wl_listener swipe_begin, swipe_update, swipe_end;
  struct wl_listener pinch_begin, pinch_update, pinch_end;
  struct wl_listener idle_inhibit_new;

  struct wl_list outputs, views, keyboards, layer_surfaces;

  TwmState twm;
  AnimSet anim;
  Config cfg;
  GestureTracker gesture;
  bool running;
  DragMode drag_mode;
  PaneId drag_pane;
  int idle_inhibit_count;
  bool exec_once_done;

  int ipc_fd, inotify_fd;
  struct wl_event_source *ipc_src, *inotify_src, *idle_timer;
  int idle_timeout_ms;
  int subscriber_fds[MAX_IPC_SUBSCRIBERS];
  int subscriber_count;

  pid_t reload_pid;
  int reload_pipe_fd;
  struct wl_event_source *reload_pipe_src;
  char reload_new_bin[512];
  char **saved_argv;

  /* ── Lua state ─────────────────────────────────────────────────────── */
  lua_State *L;

  /* Global signal table — connect_signal/emit_signal targets */
  LuaSignalTable signals;

  /* Legacy C-config keybinds (still supported alongside Lua trixie.key) */
  struct {
    uint32_t mods;
    xkb_keysym_t sym;
    int fn_ref;
  } lua_binds[MAX_KEYBINDS];
  int lua_bind_count;

  /* Lua window rules (trixie.rules.rules table) */
  struct {
    char app_id[128], title[128];
    int workspace;
    bool follow, set_float, float_val;
    bool set_fullscreen, fullscreen_val;
    bool noborder, notitle;
    float opacity;
    int x, y, w, h;
  } lua_rules[MAX_WIN_RULES];
  int lua_rule_count;

#ifdef HAS_XWAYLAND
  struct wlr_xwayland *xwayland;
  struct wl_listener xwayland_ready;
  struct wl_listener new_xwayland_surface;
#endif
};

/* ════════════════════════════════════════════════════════════════════════
 * Function declarations
 * ════════════════════════════════════════════════════════════════════════ */

/* ── Layout ────────────────────────────────────────────────────────────── */
const char *layout_label(Layout l);
Layout layout_next(Layout l);
Layout layout_prev(Layout l);
void layout_compute(Layout, Rect, int n, float ratio, int gap, Rect *out);
void dwindle_clear(DwindleTree *);
void dwindle_insert(DwindleTree *, PaneId, PaneId focused, Rect, int gap);
void dwindle_remove(DwindleTree *, PaneId);
void dwindle_recompute(DwindleTree *, Rect, int gap);
void dwindle_recompute_subtree(DwindleTree *, int idx, Rect, int gap);
bool dwindle_get_rect(DwindleTree *, PaneId, Rect *out);
bool dwindle_has_leaf(DwindleTree *, PaneId);
void dwindle_sync(DwindleTree *, const PaneId *tiled, int n, PaneId focused);
bool dwindle_adjust_split(DwindleTree *, PaneId, float delta);
bool dwindle_toggle_split(DwindleTree *, PaneId);
bool dwindle_swap_cycle(DwindleTree *, PaneId, bool forward);
bool dwindle_swap_main(DwindleTree *, PaneId);
bool dwindle_swap_dir(DwindleTree *, PaneId, int dx, int dy);

/* ── TWM ───────────────────────────────────────────────────────────────── */
void twm_init(TwmState *, int w, int h, int bar_h, bool bar_bottom, int gap,
              int border_w, int pad, int ws_count, bool smart_gaps);
void twm_resize(TwmState *, int w, int h);
void twm_set_bar_height(TwmState *, int h, bool at_bottom);
void twm_reflow(TwmState *);
PaneId twm_open(TwmState *, const char *app_id);
PaneId twm_open_ex(TwmState *, const char *app_id, bool floating,
                   bool fullscreen);
void twm_close(TwmState *, PaneId);
void twm_set_title(TwmState *, PaneId, const char *title);
void twm_set_focused(TwmState *, PaneId);
PaneId twm_focused_id(TwmState *);
Pane *twm_focused(TwmState *);
Pane *twm_pane_by_id(TwmState *, PaneId);
void twm_focus_dir(TwmState *, int dx, int dy);
void twm_toggle_float(TwmState *);
void twm_float_move(TwmState *, PaneId, int dx, int dy);
void twm_float_resize(TwmState *, PaneId, int dw, int dh);
void twm_switch_ws(TwmState *, int n);
void twm_move_to_ws(TwmState *, int n);
void twm_swap(TwmState *, bool forward);
void twm_swap_main(TwmState *);
void twm_swap_dir(TwmState *, int dx, int dy);
void twm_register_scratch(TwmState *, const char *name, const char *app_id,
                          const char *exec, float wpct, float hpct);
bool twm_try_assign_scratch(TwmState *, PaneId, const char *app_id);
void twm_scratch_notify_title(TwmState *, PaneId);
void twm_toggle_scratch(TwmState *, const char *name);
int ipc_scratch_json(TwmState *, char *buf, size_t sz);
PaneId new_pane_id(void);

/* ── Animation ─────────────────────────────────────────────────────────── */
void anim_set_resize(AnimSet *, int w, int h);
void anim_open(AnimSet *, PaneId, Rect);
void anim_close(AnimSet *, PaneId, Rect);
void anim_float_open(AnimSet *, PaneId, Rect);
void anim_float_close(AnimSet *, PaneId, Rect);
void anim_scratch_open(AnimSet *, PaneId, Rect);
void anim_scratch_close(AnimSet *, PaneId, Rect);
void anim_morph(AnimSet *, PaneId, Rect from, Rect to);
void anim_fade_in(AnimSet *, PaneId, int ms);
void anim_fade_out(AnimSet *, PaneId, int ms);
void anim_cancel(AnimSet *, PaneId);
bool anim_tick(AnimSet *);
Rect anim_get_rect(AnimSet *, PaneId, Rect fallback);
float anim_get_opacity(AnimSet *, PaneId, float fallback);
bool anim_is_closing(AnimSet *, PaneId);
bool anim_any(AnimSet *);
int anim_ws_incoming_x(AnimSet *);
int anim_ws_outgoing_x(AnimSet *);
void anim_workspace_transition(AnimSet *, WsDir);

/* ── Config ────────────────────────────────────────────────────────────── */
void config_defaults(Config *);
void config_load(Config *, const char *path);
void config_reload(Config *);
void config_apply_fallback_keybinds(Config *);
void config_set_theme(Config *, const char *name);

/* ── Canvas / font ─────────────────────────────────────────────────────── */
/* Implemented in bar.c — used by wibox and deco */
bool canvas_font_init(const char *path, const char *bold, const char *italic,
                      float size_pt);
bool canvas_font_reload(const char *path, const char *bold, const char *italic,
                        float size_pt);
int canvas_measure(const char *text);
int canvas_draw_text(Canvas *c, int x, int y, const char *text, Color fg);
int canvas_font_ascender(void);
int canvas_font_height(void);
void canvas_fill_rect(Canvas *c, int x, int y, int w, int h, Color col);
void canvas_clear(Canvas *c, Color col);
void canvas_draw_image(Canvas *c, const uint32_t *src_px, int src_w, int src_h,
                       int dx, int dy, int dw, int dh);

/* ── Wibox ─────────────────────────────────────────────────────────────── */
TrixieWibox *wibox_create(TrixieServer *s, TrixieOutput *o, int x, int y, int w,
                          int h);
void wibox_destroy(TrixieWibox *wb);
void wibox_mark_dirty(TrixieWibox *wb);
void wibox_commit(TrixieWibox *wb);
void wibox_lua_draw(TrixieWibox *wb, lua_State *L);
/* Destroy all wiboxes — only call from output_handle_destroy / shutdown */
void wibox_clear_output(TrixieOutput *o);
/* Reset callbacks without destroying scene nodes — safe to call mid-loop */
void wibox_reset_output(TrixieOutput *o, lua_State *L);

/* ── Bar (thin wrapper kept for output_handle_frame) ──────────────────── */
TrixieBar *bar_create(struct wlr_scene_tree *, int w, int h, const Config *);
bool bar_update(TrixieBar *, TwmState *, const Config *);
void bar_destroy(TrixieBar *);
void bar_mark_dirty(TrixieBar *);
void bar_set_visible(TrixieBar *, bool);
void bar_resize(TrixieBar *, int w, int h);

/* ── Deco ──────────────────────────────────────────────────────────────── */
TrixieDeco *deco_create(struct wlr_scene_tree *tiled,
                        struct wlr_scene_tree *floating);
void deco_destroy(TrixieDeco *);
void deco_mark_dirty(TrixieDeco *);
void deco_update(TrixieDeco *, TwmState *, AnimSet *, const Config *);

/* ── IPC ───────────────────────────────────────────────────────────────── */
void ipc_dispatch(TrixieServer *, const char *line, char *reply, size_t sz);
bool ipc_subscribe(TrixieServer *, int fd);
void ipc_push_focus_changed(TrixieServer *);
void ipc_push_workspace_changed(TrixieServer *);
void ipc_push_title_changed(TrixieServer *, PaneId);

/* ── Signal system ─────────────────────────────────────────────────────── */
/* C-side helpers for emitting signals into Lua */
void lua_signal_init(LuaSignalTable *t);
/* Connect fn (at stack top) to named signal in table.
 * Pops the function. Returns the fn_ref. */
int lua_signal_connect(lua_State *L, LuaSignalTable *t, const char *name);
/* Disconnect a specific fn_ref from a signal. */
void lua_signal_disconnect(lua_State *L, LuaSignalTable *t, const char *name,
                           int fn_ref);
/* Emit: push args onto stack before calling, pass nargs count.
 * All args are popped. */
void lua_signal_emit(lua_State *L, LuaSignalTable *t, const char *name,
                     int nargs);
/* Emit a signal on a per-object table stored at registry[obj_ref].
 * Creates the table if obj_ref == LUA_NOREF (and writes back the new ref). */
void lua_signal_emit_obj(lua_State *L, int *obj_ref, const char *name,
                         int nargs);

/* ── Lua ───────────────────────────────────────────────────────────────── */
void lua_init(TrixieServer *);
void lua_reload(TrixieServer *);
void lua_destroy(TrixieServer *);
bool lua_dispatch_key(TrixieServer *, uint32_t mods, xkb_keysym_t sym);
void lua_apply_window_rules(TrixieServer *, Pane *, const char *app_id,
                            const char *title);

/* Signal firing — called from main.c / view handlers */
void lua_emit_manage(TrixieServer *, PaneId);
void lua_emit_unmanage(TrixieServer *, PaneId);
void lua_emit_focus(TrixieServer *);
void lua_emit_workspace_changed(TrixieServer *);
void lua_emit_title_changed(TrixieServer *, PaneId);
void lua_emit_property(TrixieServer *, PaneId, const char *prop);
void lua_emit_screen_added(TrixieServer *, TrixieOutput *);
void lua_emit_screen_removed(TrixieServer *, TrixieOutput *);
void lua_apply_pending_monitor(TrixieServer *s, TrixieOutput *o);
void lua_emit_startup(TrixieServer *);

/* ── Server ────────────────────────────────────────────────────────────── */
void server_spawn(TrixieServer *, const char *cmd);
void server_dispatch_action(TrixieServer *, Action *);

void server_sync_focus(TrixieServer *s);
void server_sync_windows(TrixieServer *);
void server_request_redraw(TrixieServer *);
void server_mark_deco_dirty(TrixieServer *);
void server_focus_pane(TrixieServer *, PaneId);
void server_float_toggle(TrixieServer *);
void server_scratch_toggle(TrixieServer *, const char *name);
void server_apply_config_reload(TrixieServer *);
void server_binary_reload(TrixieServer *);
void server_reset_idle(TrixieServer *);
void server_init_ipc(TrixieServer *);
void server_init_config_watch(TrixieServer *);
TrixieView *view_from_pane(TrixieServer *, PaneId);
void server_schedule_reload(TrixieServer *s);
void server_set_bar_inset(TrixieServer *s, int h, bool at_bottom);

/* Reload */
void reload_config(TrixieServer *s);
void lua_register_reload(TrixieServer *s);

/* Deco */
void deco_complete_update(TrixieDeco *d, TwmState *twm, AnimSet *anim,
                          const Config *cfg);

/* Lua layout/button/gesture dispatch */
bool lua_call_layout(TrixieServer *s, const char *name, PaneId *panes,
                     int npanes, Rect area, int gap);
bool lua_dispatch_button(TrixieServer *s, uint32_t mods, uint32_t btn,
                         bool pressed);
bool lua_dispatch_gesture(TrixieServer *s, const char *spec);

/* View geometry helper exposed for lua.c client_geometry */
void view_apply_geom_pub(TrixieServer *s, TrixieView *v, Pane *p);
