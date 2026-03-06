#ifndef TRIXIE_H
#define TRIXIE_H

#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/libinput.h>
#include <wlr/backend/session.h>
#include <wlr/render/allocator.h>
#include <wlr/render/gles2.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

// ── Rect ──────────────────────────────────────────────────────────────────────

typedef struct {
  int x, y, w, h;
} Rect;

static inline Rect rect_make(int x, int y, int w, int h) {
  return (Rect){ x, y, w, h };
}
static inline bool rect_empty(Rect r) {
  return r.w <= 0 || r.h <= 0;
}
static inline bool rect_contains(Rect r, int px, int py) {
  return px >= r.x && py >= r.y && px < r.x + r.w && py < r.y + r.h;
}
static inline Rect rect_inset(Rect r, int px) {
  return (Rect){ r.x + px, r.y + px, r.w - px * 2, r.h - px * 2 };
}

// ── Layout ────────────────────────────────────────────────────────────────────

typedef enum {
  LAYOUT_BSP,
  LAYOUT_COLUMNS,
  LAYOUT_ROWS,
  LAYOUT_THREECOL,
  LAYOUT_MONOCLE,
  LAYOUT_COUNT,
} Layout;

const char *layout_label(Layout l);
Layout      layout_next(Layout l);
Layout      layout_prev(Layout l);

// ── Animation ─────────────────────────────────────────────────────────────────

typedef enum {
  ANIM_OPEN,
  ANIM_CLOSE,
  ANIM_MORPH,
  ANIM_FLOAT_OPEN,
  ANIM_FLOAT_CLOSE,
  ANIM_SCRATCH_OPEN,
  ANIM_SCRATCH_CLOSE,
} AnimKind;

typedef struct {
  AnimKind        kind;
  struct timespec start;
  int             duration_ms;
  Rect            target;
  int             from[4];
  bool            active;
} PaneAnim;

typedef enum { WS_DIR_LEFT, WS_DIR_RIGHT } WsDir;

typedef struct {
  bool            active;
  WsDir           dir;
  struct timespec start;
  int             duration_ms;
  int             screen_w;
} WsTransition;

typedef struct {
  uint32_t id;
  PaneAnim anim;
} AnimEntry;

#define MAX_PANES 256

typedef struct {
  AnimEntry    entries[MAX_PANES];
  int          count;
  WsTransition ws;
  int          screen_w, screen_h;
} AnimSet;

void anim_set_resize(AnimSet *a, int w, int h);
void anim_open(AnimSet *a, uint32_t id, Rect r);
void anim_close(AnimSet *a, uint32_t id, Rect r);
void anim_float_open(AnimSet *a, uint32_t id, Rect r);
void anim_float_close(AnimSet *a, uint32_t id, Rect r);
void anim_scratch_open(AnimSet *a, uint32_t id, Rect r);
void anim_scratch_close(AnimSet *a, uint32_t id, Rect r);
void anim_morph(AnimSet *a, uint32_t id, Rect from, Rect to);
bool anim_tick(AnimSet *a);
Rect anim_get_rect(AnimSet *a, uint32_t id, Rect fallback);
bool anim_is_closing(AnimSet *a, uint32_t id);
bool anim_any(AnimSet *a);
void anim_workspace_transition(AnimSet *a, WsDir dir);
int  anim_ws_incoming_x(AnimSet *a);
int  anim_ws_outgoing_x(AnimSet *a);

// ── Config ────────────────────────────────────────────────────────────────────

typedef struct {
  uint8_t r, g, b, a;
} Color;
static inline Color color_hex(uint32_t v) {
  return (Color){ (v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff, 0xff };
}

typedef enum { BAR_TOP, BAR_BOTTOM } BarPos;

#define MAX_KEYBINDS    128
#define MAX_MODS_STR    64
#define MAX_EXEC_ARGS   16
#define MAX_SCRATCHPADS 16
#define MAX_MONITORS    8
#define MAX_WIN_RULES   32
#define MAX_BAR_MODS    16

typedef enum {
  MOD_SUPER = 1 << 0,
  MOD_CTRL  = 1 << 1,
  MOD_ALT   = 1 << 2,
  MOD_SHIFT = 1 << 3,
} Mods;

typedef enum {
  ACTION_EXEC,
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
  ACTION_WORKSPACE,
  ACTION_MOVE_TO_WS,
  ACTION_NEXT_LAYOUT,
  ACTION_PREV_LAYOUT,
  ACTION_GROW_MAIN,
  ACTION_SHRINK_MAIN,
  ACTION_NEXT_WS,
  ACTION_PREV_WS,
  ACTION_QUIT,
  ACTION_RELOAD,
  ACTION_SWITCH_VT,
  ACTION_SCRATCHPAD,
} ActionKind;

typedef struct {
  ActionKind kind;
  char       exec_cmd[256];
  char       exec_args[MAX_EXEC_ARGS][256];
  int        exec_argc;
  int        n;
  char       name[64];
} Action;

typedef struct {
  uint32_t     mods;
  xkb_keysym_t sym;
  Action       action;
} Keybind;

typedef struct {
  char app_id[128];
  bool float_rule, fullscreen_rule, noborder, notitle;
  int  forced_ws;
  int  forced_w, forced_h;
  int  forced_x, forced_y;
} WinRule;

typedef struct {
  char  name[64];
  char  app_id[64];
  float width_pct, height_pct;
} ScratchpadCfg;

typedef struct {
  char  name[64];
  int   width, height, refresh;
  int   pos_x, pos_y;
  float scale;
} MonitorCfg;

typedef struct {
  BarPos position;
  int    height;
  Color  bg, fg, accent, dim;
  Color  active_ws_fg, active_ws_bg;
  Color  occupied_ws_fg, inactive_ws_fg;
  char   modules_left[MAX_BAR_MODS][64];
  char   modules_center[MAX_BAR_MODS][64];
  char   modules_right[MAX_BAR_MODS][64];
  int    modules_left_n, modules_center_n, modules_right_n;
  bool   separator;
  Color  separator_color;
  int    pill_radius;
  int    item_spacing;
  float  font_size;
} BarCfg;

typedef struct {
  Color active_border, inactive_border;
  Color pane_bg;
} Colors;

typedef struct {
  char kb_layout[32];
  char kb_variant[32];
  char kb_options[128];
  int  repeat_rate, repeat_delay;
} KbCfg;

typedef struct {
  char  font_path[256];
  float font_size;
  int   gap, border_width, corner_radius;
  bool  smart_gaps;
  char  cursor_theme[64];
  int   cursor_size;

  Colors colors;
  BarCfg bar;
  KbCfg  keyboard;

  Keybind keybinds[MAX_KEYBINDS];
  int     keybind_count;

  WinRule win_rules[MAX_WIN_RULES];
  int     win_rule_count;

  MonitorCfg monitors[MAX_MONITORS];
  int        monitor_count;

  ScratchpadCfg scratchpads[MAX_SCRATCHPADS];
  int           scratchpad_count;

  char exec_once[16][256];
  int  exec_once_count;
  char exec[16][256];
  int  exec_count;

  int  workspaces;
  char seat_name[64];
} Config;

void config_defaults(Config *c);
void config_load(Config *c, const char *path);
void config_reload(Config *c);

// ── TWM ───────────────────────────────────────────────────────────────────────

#define MAX_WORKSPACES 16

typedef uint32_t PaneId;
PaneId           new_pane_id(void);

typedef enum {
  PANE_SHELL,
  PANE_EMPTY,
} PaneKind;

typedef struct {
  PaneId   id;
  PaneKind kind;
  char     app_id[128];
  char     title[256];
  bool     fullscreen;
  bool     floating;
  Rect     rect;
  Rect     float_rect;
  void    *surface;
} Pane;

typedef struct {
  int    index;
  PaneId panes[MAX_PANES];
  int    pane_count;
  PaneId focused;
  bool   has_focused;
  Layout layout;
  float  main_ratio;
  int    gap;
} Workspace;

typedef struct {
  char   name[64];
  char   app_id[64];
  PaneId pane_id;
  bool   has_pane;
  bool   visible;
  float  width_pct, height_pct;
} Scratchpad;

typedef struct {
  Pane panes[MAX_PANES];
  int  pane_count;

  Workspace workspaces[MAX_WORKSPACES];
  int       ws_count;
  int       active_ws;

  Scratchpad scratchpads[MAX_SCRATCHPADS];
  int        scratch_count;

  int  screen_w, screen_h;
  Rect content_rect;
  Rect bar_rect;
  bool bar_visible;

  int  gap, border_w, padding;
  bool smart_gaps;
} TwmState;

Pane  *twm_pane_by_id(TwmState *t, PaneId id);
Pane  *twm_focused(TwmState *t);
PaneId twm_focused_id(TwmState *t);

void twm_init(TwmState *t,
              int       w,
              int       h,
              int       bar_h,
              bool      bar_bottom,
              int       gap,
              int       border_w,
              int       pad,
              int       ws_count,
              bool      smart_gaps);
void twm_resize(TwmState *t, int w, int h);
void twm_set_bar_height(TwmState *t, int h, bool at_bottom);
void twm_reflow(TwmState *t);

PaneId twm_open(TwmState *t, const char *app_id);
void   twm_close(TwmState *t, PaneId id);
void   twm_set_title(TwmState *t, PaneId id, const char *title);
void   twm_set_focused(TwmState *t, PaneId id);

void twm_toggle_float(TwmState *t);
void twm_float_move(TwmState *t, PaneId id, int dx, int dy);
void twm_float_resize(TwmState *t, PaneId id, int dw, int dh);

void twm_register_scratch(
    TwmState *t, const char *name, const char *app_id, float wpct, float hpct);
bool twm_try_assign_scratch(TwmState *t, PaneId id, const char *app_id);
void twm_toggle_scratch(TwmState *t, const char *name);

void twm_focus_dir(TwmState *t, int dx, int dy);
void twm_swap(TwmState *t, bool forward);
void twm_switch_ws(TwmState *t, int n);
void twm_move_to_ws(TwmState *t, int n);

void layout_compute(Layout l, Rect area, int n, float ratio, int gap, Rect *out);

// ── Bar renderer ──────────────────────────────────────────────────────────────

typedef struct TrixieBar TrixieBar;
TrixieBar *bar_create(struct wlr_scene_tree *layer, int w, int h, const Config *cfg);
void       bar_destroy(TrixieBar *b);
void       bar_update(TrixieBar *b, TwmState *twm, const Config *cfg);
void       bar_resize(TrixieBar *b, int w, int h);

// ── Border/decoration renderer ────────────────────────────────────────────────

typedef struct TrixieDeco TrixieDeco;
TrixieDeco               *deco_create(struct wlr_scene_tree *layer);
void deco_update(TrixieDeco *d, TwmState *twm, AnimSet *anim, const Config *cfg);
void deco_destroy(TrixieDeco *d);

// ── Compositor state ──────────────────────────────────────────────────────────

#define MAX_OUTPUTS   8
#define MAX_KEYBOARDS 8

struct TrixieOutput;
struct TrixieKeyboard;
struct TrixieView;

typedef enum { DRAG_NONE, DRAG_MOVE, DRAG_RESIZE } DragMode;
typedef enum { MOUSE_NORMAL, MOUSE_PASSTHROUGH } MouseMode;

typedef struct TrixieServer {
  struct wl_display              *display;
  struct wlr_backend             *backend;
  struct wlr_renderer            *renderer;
  struct wlr_allocator           *allocator;
  struct wlr_scene               *scene;
  struct wlr_scene_output_layout *scene_layout;

  /* Z-ordered scene layers (bottom → top) */
  struct wlr_scene_tree *layer_background;
  struct wlr_scene_tree *layer_windows;
  struct wlr_scene_tree *layer_floating;
  struct wlr_scene_tree *layer_overlay;

  struct wlr_xdg_shell                 *xdg_shell;
  struct wlr_layer_shell_v1            *layer_shell;
  struct wlr_xdg_output_manager_v1     *output_mgr;
  struct wlr_xdg_decoration_manager_v1 *deco_mgr;
  struct wlr_server_decoration_manager *srv_deco;

  struct wlr_compositor                                   *compositor;
  struct wlr_subcompositor                                *subcompositor;
  struct wlr_data_device_manager                          *ddm;
  struct wlr_primary_selection_unstable_v1_device_manager *primary_sel;

  struct wlr_output_layout   *output_layout;
  struct wlr_cursor          *cursor;
  struct wlr_xcursor_manager *xcursor_mgr;
  struct wlr_seat            *seat;

  struct wl_list views;
  struct wl_list outputs;
  struct wl_list keyboards;

  struct wl_listener new_output;
  struct wl_listener new_input;
  struct wl_listener new_xdg_surface;
  struct wl_listener new_layer_surface;
  struct wl_listener new_deco;
  struct wl_listener cursor_motion;
  struct wl_listener cursor_motion_abs;
  struct wl_listener cursor_button;
  struct wl_listener cursor_axis;
  struct wl_listener cursor_frame;
  struct wl_listener seat_request_cursor;
  struct wl_listener seat_request_set_selection;
  struct wl_listener seat_request_set_primary_selection;

  TwmState twm;
  AnimSet  anim;
  Config   cfg;

  DragMode  drag_mode;
  PaneId    drag_pane;
  MouseMode mouse_mode;

  bool running;
  bool exec_once_done;

  int                     ipc_fd;
  struct wl_event_source *ipc_src;

  int                     inotify_fd;
  struct wl_event_source *inotify_src;
} TrixieServer;

typedef struct TrixieOutput {
  struct wl_list           link;
  TrixieServer            *server;
  struct wlr_output       *wlr_output;
  struct wlr_scene_output *scene_output;

  struct wl_listener frame;
  struct wl_listener request_state;
  struct wl_listener destroy;

  TrixieBar  *bar;
  TrixieDeco *deco;
  int         width, height;
} TrixieOutput;

typedef struct TrixieView {
  struct wl_list           link;
  TrixieServer            *server;
  struct wlr_xdg_toplevel *xdg_toplevel;
  struct wlr_scene_tree   *scene_tree;
  PaneId                   pane_id;
  bool                     mapped;
  bool                     initial_configure_sent;

  struct wl_listener map;
  struct wl_listener unmap;
  struct wl_listener destroy;
  struct wl_listener commit;
  struct wl_listener request_fullscreen;
  struct wl_listener request_maximize;
  struct wl_listener set_app_id;
  struct wl_listener set_title;
} TrixieView;

typedef struct TrixieKeyboard {
  struct wl_list       link;
  TrixieServer        *server;
  struct wlr_keyboard *wlr_keyboard;

  struct wl_listener modifiers;
  struct wl_listener key;
  struct wl_listener destroy;
} TrixieKeyboard;

void server_init_input(TrixieServer *s);
void server_init_output(TrixieServer *s);
void server_init_shell(TrixieServer *s);
void server_init_ipc(TrixieServer *s);
void server_init_config_watch(TrixieServer *s);

TrixieView *view_from_pane(TrixieServer *s, PaneId id);
TrixieView *view_at(TrixieServer        *s,
                    double               lx,
                    double               ly,
                    struct wlr_surface **surf,
                    double              *sx,
                    double              *sy);
void        server_focus_pane(TrixieServer *s, PaneId id);
void        server_sync_focus(TrixieServer *s);
void        server_sync_windows(TrixieServer *s);
void        server_apply_anim(TrixieServer *s, PaneId id);
void        server_request_redraw(TrixieServer *s);
void        server_spawn(TrixieServer *s, const char *cmd);
void        server_dispatch_action(TrixieServer *s, Action *a);

void server_scratch_toggle(TrixieServer *s, const char *name);
void server_float_toggle(TrixieServer *s);

void server_apply_config_reload(TrixieServer *s);

void ipc_dispatch(TrixieServer *s, const char *line, char *reply, size_t reply_sz);

void layout_compute(Layout l, Rect area, int n, float ratio, int gap, Rect *out);

#endif /* TRIXIE_H */
