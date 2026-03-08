/* trixie.h — Shared types, structs, and function declarations.
 *
 * Version history
 * ───────────────
 * 0.1.0  Initial
 * 0.2.0  Async bar workers, XWayland completion, TUI overlay, multi-monitor
 *        stubs, cursor-shape protocol, theme system, powerline separators.
 * 0.2.1  Fix: modifier keybinds (base-level sym lookup + mod masking)
 *        Fix: window rule exact-match via rule_matches()
 *        Fix: modules_left/center/right reset-in-loop bug
 *        Feat: per-workspace layout/ratio config blocks
 *        Feat: ACTION_RESIZE_RATIO, ACTION_FOCUS_URGENT
 *        Feat: ws_urgent_mask for bar urgency dots
 */
#pragma once

#define TRIXIE_VERSION_MAJOR 0
#define TRIXIE_VERSION_MINOR 2
#define TRIXIE_VERSION_PATCH 1
#define TRIXIE_VERSION_STR   "0.2.1"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/session.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

#ifdef HAS_XWAYLAND
#include <wlr/xwayland.h>
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Constants
 * ═══════════════════════════════════════════════════════════════════════════ */

#define MAX_PANES           256
#define MAX_WORKSPACES      16
#define MAX_MONITORS        8
#define MAX_KEYBINDS        256
#define MAX_WIN_RULES       64
#define MAX_SCRATCHPADS     16
#define MAX_BAR_MODS        16
#define MAX_BAR_MODULE_CFGS 32

#define TITLE_BAR_H 22

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  Basic types
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef uint32_t PaneId;

typedef struct {
  int x, y, w, h;
} Rect;

typedef struct {
  uint8_t r, g, b, a;
} Color;

static inline Color color_hex(uint32_t rgb) {
  return (Color){
    .r = (rgb >> 16) & 0xff,
    .g = (rgb >> 8) & 0xff,
    .b = rgb & 0xff,
    .a = 0xff,
  };
}

static inline bool rect_empty(Rect r) {
  return r.w <= 0 || r.h <= 0;
}
static inline bool rect_contains(Rect r, int x, int y) {
  return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}
static inline Rect rect_inset(Rect r, int n) {
  return (Rect){ r.x + n, r.y + n, r.w - n * 2, r.h - n * 2 };
}
static inline float fclampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  Layout
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
  LAYOUT_BSP,
  LAYOUT_SPIRAL,
  LAYOUT_COLUMNS,
  LAYOUT_ROWS,
  LAYOUT_THREECOL,
  LAYOUT_MONOCLE,
  LAYOUT_COUNT,
} Layout;

const char *layout_label(Layout l);
Layout      layout_next(Layout l);
Layout      layout_prev(Layout l);
void layout_compute(Layout l, Rect area, int n, float ratio, int gap, Rect *out);

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  Animation
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
  ANIM_OPEN,
  ANIM_CLOSE,
  ANIM_FLOAT_OPEN,
  ANIM_FLOAT_CLOSE,
  ANIM_SCRATCH_OPEN,
  ANIM_SCRATCH_CLOSE,
  ANIM_MORPH,
  ANIM_FADE_IN,
  ANIM_FADE_OUT,
} AnimKind;

typedef enum { WS_DIR_LEFT, WS_DIR_RIGHT } WsDir;

typedef struct {
  AnimKind        kind;
  int             from[4];
  Rect            target;
  int             duration_ms;
  bool            active;
  struct timespec start;
  float           opacity_from;
  float           opacity_to;
} PaneAnim;

typedef struct {
  PaneId   id;
  PaneAnim anim;
} AnimEntry;

typedef struct {
  bool            active;
  WsDir           dir;
  int             duration_ms;
  int             screen_w;
  struct timespec start;
} WsAnim;

typedef struct {
  AnimEntry entries[MAX_PANES];
  int       count;
  int       screen_w, screen_h;
  WsAnim    ws;
} AnimSet;

void  anim_set_resize(AnimSet *a, int w, int h);
void  anim_open(AnimSet *a, uint32_t id, Rect r);
void  anim_close(AnimSet *a, uint32_t id, Rect r);
void  anim_float_open(AnimSet *a, uint32_t id, Rect r);
void  anim_float_close(AnimSet *a, uint32_t id, Rect r);
void  anim_scratch_open(AnimSet *a, uint32_t id, Rect r);
void  anim_scratch_close(AnimSet *a, uint32_t id, Rect r);
void  anim_morph(AnimSet *a, uint32_t id, Rect from, Rect to);
void  anim_fade_in(AnimSet *a, uint32_t id, int duration_ms);
void  anim_fade_out(AnimSet *a, uint32_t id, int duration_ms);
void  anim_workspace_transition(AnimSet *a, WsDir dir);
bool  anim_tick(AnimSet *a);
Rect  anim_get_rect(AnimSet *a, uint32_t id, Rect fallback);
float anim_get_opacity(AnimSet *a, uint32_t id, float fallback);
bool  anim_is_closing(AnimSet *a, uint32_t id);
bool  anim_any(AnimSet *a);
int   anim_ws_incoming_x(AnimSet *a);
int   anim_ws_outgoing_x(AnimSet *a);

/* ═══════════════════════════════════════════════════════════════════════════
 * §5  Config
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum { BAR_TOP, BAR_BOTTOM } BarPosition;

typedef enum {
  MOD_SUPER = 1 << 0,
  MOD_CTRL  = 1 << 1,
  MOD_ALT   = 1 << 2,
  MOD_SHIFT = 1 << 3,
} ModKey;

typedef enum {
  ACTION_QUIT,
  ACTION_RELOAD,
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
  ACTION_MOVE_UP,
  ACTION_MOVE_DOWN,
  ACTION_SWAP_MAIN,
  ACTION_WORKSPACE,
  ACTION_MOVE_TO_WS,
  ACTION_NEXT_WS,
  ACTION_PREV_WS,
  ACTION_NEXT_LAYOUT,
  ACTION_PREV_LAYOUT,
  ACTION_GROW_MAIN,
  ACTION_SHRINK_MAIN,
  ACTION_SCRATCHPAD,
  ACTION_SWITCH_VT,
  ACTION_EMERGENCY_QUIT,
  ACTION_TOGGLE_OVERLAY,
  ACTION_RESIZE_RATIO, /* keyboard-driven main ratio nudge  (Feature 3) */
  ACTION_FOCUS_URGENT, /* jump to most-recently-urgent pane (Feature 4) */
} ActionKind;

typedef struct {
  ActionKind kind;
  char       exec_cmd[256];
  char       name[64];
  int        n;
  float      ratio_delta; /* used by ACTION_RESIZE_RATIO */
} Action;

typedef struct {
  uint32_t     mods;
  xkb_keysym_t sym;
  Action       action;
} Keybind;

typedef struct {
  char  app_id[128];
  bool  float_rule;
  bool  fullscreen_rule;
  bool  noborder;
  bool  notitle;
  int   forced_ws;
  int   forced_w, forced_h;
  int   forced_x, forced_y;
  float opacity;
} WinRule;

typedef struct {
  char  name[64];
  int   width, height, refresh;
  int   pos_x, pos_y;
  float scale;
} MonitorCfg;

typedef struct {
  char  name[64];
  char  app_id[64];
  float width_pct, height_pct;
} ScratchpadCfg;

typedef struct {
  char  name[64];
  char  exec[256];
  char  icon[32];
  char  format[64];
  int   interval;
  Color color;
  bool  has_color;
} BarModuleCfg;

typedef enum {
  SEP_NONE  = 0,
  SEP_PIPE  = 1,
  SEP_BLOCK = 2,
  SEP_ARROW = 3
} BarSepStyle;

typedef struct {
  BarPosition  position;
  int          height;
  int          padding;
  int          glyph_y_offset;
  int          item_spacing;
  int          pill_radius;
  float        font_size;
  Color        bg, fg, accent, dim;
  Color        active_ws_fg, active_ws_bg;
  Color        occupied_ws_fg, inactive_ws_fg;
  bool         separator;
  bool         separator_top;
  Color        separator_color;
  BarSepStyle  sep_style;
  char         modules_left[MAX_BAR_MODS][64];
  int          modules_left_n;
  char         modules_center[MAX_BAR_MODS][64];
  int          modules_center_n;
  char         modules_right[MAX_BAR_MODS][64];
  int          modules_right_n;
  BarModuleCfg module_cfgs[MAX_BAR_MODULE_CFGS];
  int          module_cfg_count;
} BarCfg;

typedef struct {
  char kb_layout[32];
  char kb_variant[32];
  char kb_options[64];
  int  repeat_rate;
  int  repeat_delay;
} KeyboardCfg;

typedef struct {
  Color active_border, inactive_border;
  Color active_title, inactive_title;
  Color pane_bg;
  Color bar_bg, bar_fg, bar_accent;
  Color focus_ring;
  Color background;
} ColorSet;

typedef enum {
  THEME_CUSTOM = 0,
  THEME_CATPPUCCIN_MOCHA,
  THEME_CATPPUCCIN_LATTE,
  THEME_GRUVBOX,
  THEME_NORD,
  THEME_TOKYO_NIGHT,
} ThemeId;

typedef struct {
  /* Fonts */
  char          font_path[256];
  char          font_path_bold[256];
  char          font_path_italic[256];
  float         font_size;
  /* Layout */
  int           gap;
  int           outer_gap;
  int           border_width;
  int           corner_radius;
  bool          smart_gaps;
  /* Cursor */
  char          cursor_theme[64];
  int           cursor_size;
  /* Session */
  int           workspaces;
  char          seat_name[32];
  int           idle_timeout;
  bool          xwayland;
  /* Colors */
  ColorSet      colors;
  ThemeId       theme;
  /* Bar */
  BarCfg        bar;
  /* Input */
  KeyboardCfg   keyboard;
  /* Keybinds */
  Keybind       keybinds[MAX_KEYBINDS];
  int           keybind_count;
  /* Window rules */
  WinRule       win_rules[MAX_WIN_RULES];
  int           win_rule_count;
  /* Monitors */
  MonitorCfg    monitors[MAX_MONITORS];
  int           monitor_count;
  /* Scratchpads */
  ScratchpadCfg scratchpads[MAX_SCRATCHPADS];
  int           scratchpad_count;
  /* Per-workspace layout overrides — set by "workspace N { }" blocks (Feature 1) */
  Layout        ws_layout[MAX_WORKSPACES];
  bool          ws_layout_set[MAX_WORKSPACES];
  float         ws_ratio[MAX_WORKSPACES];
  bool          ws_ratio_set[MAX_WORKSPACES];
  /* Exec */
  char          exec_once[16][256];
  int           exec_once_count;
  char          exec[16][256];
  int           exec_count;
} Config;

void config_defaults(Config *c);
void config_load(Config *c, const char *path);
void config_reload(Config *c);
void config_apply_fallback_keybinds(Config *c);
void config_set_theme(Config *c, const char *name);

/* ═══════════════════════════════════════════════════════════════════════════
 * §5a  Window rule matching helper  (Fix 2)
 *
 * Centralises all matching logic so every caller (deco.c, main.c, bar.c)
 * uses identical semantics:
 *   • "title:foo"   — substring match against p->title
 *   • "*foo*"       — substring glob against p->app_id
 *   • "foo"         — exact match against p->app_id
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Forward-declare Pane here so rule_matches can reference it.
 * Full definition is in §6 below. */
typedef struct Pane_s Pane;

#include <string.h>
static inline bool rule_matches(const WinRule *wr, const Pane *p);

/* ═══════════════════════════════════════════════════════════════════════════
 * §6  TWM state
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum { PANE_SHELL, PANE_XWAYLAND } PaneKind;

struct Pane_s {
  PaneId   id;
  PaneKind kind;
  Rect     rect;
  Rect     float_rect;
  bool     floating;
  bool     fullscreen;
  char     title[256];
  char     app_id[128];
};
/* Keep the plain 'Pane' name available (typedef above covers it). */

typedef struct {
  int    index;
  Layout layout;
  float  main_ratio;
  int    gap;
  PaneId panes[MAX_PANES];
  int    pane_count;
  PaneId focused;
  bool   has_focused;
} Workspace;

typedef struct {
  char   name[32];
  char   app_id[64];
  float  width_pct, height_pct;
  PaneId pane_id;
  bool   has_pane;
  bool   visible;
} Scratchpad;

typedef struct {
  int        screen_w, screen_h;
  Rect       content_rect;
  Rect       bar_rect;
  int        gap;
  int        border_w;
  int        padding;
  bool       smart_gaps;
  bool       bar_visible;
  Workspace  workspaces[MAX_WORKSPACES];
  int        ws_count;
  int        active_ws;
  Pane       panes[MAX_PANES];
  int        pane_count;
  Scratchpad scratchpads[MAX_SCRATCHPADS];
  int        scratch_count;
  /* Bitmask: bit i set → workspace i has at least one urgent pane.
   * Set in view_handle_set_urgent; cleared in twm_set_focused / twm_switch_ws.
   * (Feature 4 / Feature 5) */
  uint32_t   ws_urgent_mask;
} TwmState;

/* ── rule_matches inline body (needs Pane to be complete) ── */
static inline bool rule_matches(const WinRule *wr, const Pane *p) {
  const char *pat = wr->app_id;
  if(!pat[0]) return false;

  if(!strncmp(pat, "title:", 6)) return strstr(p->title, pat + 6) != NULL;

  if(strchr(pat, '*')) {
    size_t pl        = strlen(pat);
    bool   ls        = pat[0] == '*';
    bool   ts        = pat[pl - 1] == '*';
    char   core[128] = { 0 };
    size_t cl        = 0;
    for(size_t i = (ls ? 1 : 0); i < pl - (ts ? 1 : 0) && cl < 127; i++)
      core[cl++] = pat[i];
    return strstr(p->app_id, core) != NULL;
  }

  return strcmp(p->app_id, pat) == 0;
}

PaneId new_pane_id(void);

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
PaneId twm_open_ex(TwmState *t, const char *app_id, bool floating, bool fullscreen);
void   twm_close(TwmState *t, PaneId id);
void   twm_set_title(TwmState *t, PaneId id, const char *title);

PaneId twm_focused_id(TwmState *t);
Pane  *twm_focused(TwmState *t);
Pane  *twm_pane_by_id(TwmState *t, PaneId id);

void twm_set_focused(TwmState *t, PaneId id);
void twm_focus_dir(TwmState *t, int dx, int dy);
void twm_toggle_float(TwmState *t);
void twm_float_move(TwmState *t, PaneId id, int dx, int dy);
void twm_float_resize(TwmState *t, PaneId id, int dw, int dh);
void twm_swap(TwmState *t, bool forward);
void twm_swap_main(TwmState *t);
void twm_switch_ws(TwmState *t, int n);
void twm_move_to_ws(TwmState *t, int n);

void twm_register_scratch(
    TwmState *t, const char *name, const char *app_id, float wpct, float hpct);
bool twm_try_assign_scratch(TwmState *t, PaneId id, const char *app_id);
void twm_toggle_scratch(TwmState *t, const char *name);

static inline int deco_title_h(int border_w, bool notitle) {
  (void)notitle;
  return border_w > 0 ? border_w : 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §7  Bar worker
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
  char    text[256];
  bool    valid;
  int64_t updated_ms;
} BarModuleSlot;

#define BAR_SLOT_BATTERY       0
#define BAR_SLOT_NETWORK       1
#define BAR_SLOT_VOLUME        2
#define BAR_SLOT_CPU           3
#define BAR_SLOT_MEMORY        4
#define BAR_SLOT_BUILTIN_COUNT 5
#define BAR_SLOT_EXEC_BASE     5
#define BAR_SLOT_MAX           (BAR_SLOT_EXEC_BASE + MAX_BAR_MODULE_CFGS)

typedef struct {
  _Atomic(uint64_t) generation;
  BarModuleSlot     slots[BAR_SLOT_MAX];
  pthread_mutex_t   mu;
  pthread_cond_t    wake;
  pthread_t         threads[BAR_SLOT_MAX];
  bool              running;
  const Config     *cfg;
} BarWorkerPool;

void bar_workers_init(BarWorkerPool *pool, const Config *cfg);
void bar_workers_sync_exec(BarWorkerPool *pool, const Config *cfg);
void bar_workers_stop(BarWorkerPool *pool);

/* ═══════════════════════════════════════════════════════════════════════════
 * §8  Bar renderer
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct TrixieBar TrixieBar;

TrixieBar *bar_create(struct wlr_scene_tree *layer,
                      int                    w,
                      int                    h,
                      const Config          *cfg,
                      BarWorkerPool         *pool);
void       bar_update(TrixieBar *b, TwmState *twm, const Config *cfg);
void       bar_resize(TrixieBar *b, int w, int h);
void       bar_destroy(TrixieBar *b);
void       bar_set_visible(TrixieBar *b, bool visible);
void       bar_mark_dirty(TrixieBar *b);
void       bar_sync_exec_modules(const Config *cfg);

/* ═══════════════════════════════════════════════════════════════════════════
 * §9  Decorations
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct TrixieDeco TrixieDeco;

TrixieDeco *deco_create(struct wlr_scene_tree *layer);
void deco_update(TrixieDeco *d, TwmState *twm, AnimSet *anim, const Config *cfg);
void deco_destroy(TrixieDeco *d);
void deco_mark_dirty(TrixieDeco *d);

/* ═══════════════════════════════════════════════════════════════════════════
 * §10  TUI overlay
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct TrixieOverlay TrixieOverlay;

TrixieOverlay      *
overlay_create(struct wlr_scene_tree *layer, int w, int h, const Config *cfg);
void overlay_destroy(TrixieOverlay *o);
void overlay_toggle(TrixieOverlay *o);
bool overlay_visible(TrixieOverlay *o);
bool overlay_key(TrixieOverlay *o, xkb_keysym_t sym, uint32_t mods);
void overlay_update(TrixieOverlay *o,
                    TwmState      *twm,
                    const Config  *cfg,
                    BarWorkerPool *pool);
void overlay_resize(TrixieOverlay *o, int w, int h);

/* ═══════════════════════════════════════════════════════════════════════════
 * §11  Compositor structs
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct TrixieServer TrixieServer;
typedef struct TrixieView   TrixieView;
typedef struct TrixieOutput TrixieOutput;

struct TrixieOutput {
  struct wl_list           link;
  TrixieServer            *server;
  struct wlr_output       *wlr_output;
  struct wlr_scene_output *scene_output;
  struct wl_listener       frame;
  struct wl_listener       request_state;
  struct wl_listener       destroy;
  TrixieBar               *bar;
  TrixieDeco              *deco;
  TrixieOverlay           *overlay;
  int                      width, height;
  int                      logical_w, logical_h;
  bool                     was_animating;
};

typedef struct {
  struct wl_list       link;
  TrixieServer        *server;
  struct wlr_keyboard *wlr_keyboard;
  struct wl_listener   modifiers;
  struct wl_listener   key;
  struct wl_listener   destroy;
} TrixieKeyboard;

typedef enum { DRAG_NONE, DRAG_MOVE, DRAG_RESIZE } DragMode;

struct TrixieView {
  struct wl_list link;
  TrixieServer  *server;

  struct wlr_xdg_toplevel *xdg_toplevel;

#ifdef HAS_XWAYLAND
  struct wlr_xwayland_surface *xwayland_surface;
#else
  void *xwayland_surface;
#endif
  bool is_xwayland;

  struct wlr_scene_tree *scene_tree;
  PaneId                 pane_id;
  bool                   mapped;
  bool                   urgent; /* set when app signals urgency (Feature 4) */

  struct wl_listener map;
  struct wl_listener unmap;
  struct wl_listener destroy;
  struct wl_listener commit;
  struct wl_listener request_fullscreen;
  struct wl_listener set_title;
  struct wl_listener set_app_id;
  struct wl_listener
      urgent_listener; /* repurposed xdg request_minimize (Feature 4) */

  struct wlr_foreign_toplevel_handle_v1 *foreign_handle;
  struct wl_listener                     foreign_request_activate;
  struct wl_listener                     foreign_request_close;
};

typedef struct {
  struct wl_list                     link;
  TrixieServer                      *server;
  struct wlr_layer_surface_v1       *wlr_surface;
  struct wlr_scene_layer_surface_v1 *scene_surface;
  struct wl_listener                 map;
  struct wl_listener                 unmap;
  struct wl_listener                 destroy;
} TrixieLayerSurface;

struct TrixieServer {
  struct wl_display     *display;
  struct wlr_backend    *backend;
  struct wlr_session    *session;
  struct wlr_renderer   *renderer;
  struct wlr_allocator  *allocator;
  struct wlr_compositor *compositor;

  struct wlr_scene                 *scene;
  struct wlr_scene_output_layout   *scene_layout;
  struct wlr_output_layout         *output_layout;
  struct wlr_xdg_output_manager_v1 *output_mgr;

  struct wlr_scene_tree *layer_background;
  struct wlr_scene_tree *layer_windows;
  struct wlr_scene_tree *layer_floating;
  struct wlr_scene_tree *layer_overlay;
  struct wlr_scene_rect *bg_rect;

  struct wlr_xdg_shell                 *xdg_shell;
  struct wlr_layer_shell_v1            *layer_shell;
  struct wlr_xdg_decoration_manager_v1 *deco_mgr;
  struct wlr_server_decoration_manager *srv_deco;

  struct wlr_seat            *seat;
  struct wlr_cursor          *cursor;
  struct wlr_xcursor_manager *xcursor_mgr;
  DragMode                    drag_mode;
  PaneId                      drag_pane;

  struct wlr_presentation                *presentation;
  struct wlr_fractional_scale_manager_v1 *fractional_scale_mgr;
  struct wlr_xdg_activation_v1           *xdg_activation;
  struct wlr_pointer_constraints_v1      *pointer_constraints;
  struct wlr_relative_pointer_manager_v1 *relative_pointer_mgr;
  struct wlr_foreign_toplevel_manager_v1 *foreign_toplevel_mgr;
  struct wlr_cursor_shape_manager_v1     *cursor_shape_mgr;

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
  struct wl_listener xdg_activation_request;
  struct wl_listener new_pointer_constraint;
  struct wl_listener cursor_shape_request;

#ifdef HAS_XWAYLAND
  struct wlr_xwayland *xwayland;
  struct wl_listener   xwayland_ready;
  struct wl_listener   new_xwayland_surface;
#endif

  struct wl_list views;
  struct wl_list outputs;
  struct wl_list keyboards;
  struct wl_list layer_surfaces;

  TwmState      twm;
  AnimSet       anim;
  Config        cfg;
  BarWorkerPool bar_workers;

  bool running;
  bool exec_once_done;

  int                     ipc_fd;
  struct wl_event_source *ipc_src;

#define MAX_IPC_SUBSCRIBERS 16
  int subscriber_fds[MAX_IPC_SUBSCRIBERS];
  int subscriber_count;

  int                     inotify_fd;
  struct wl_event_source *inotify_src;

  struct wl_event_source *idle_timer;
  int                     idle_timeout_ms;
};

/* ═══════════════════════════════════════════════════════════════════════════
 * §12  Server API
 * ═══════════════════════════════════════════════════════════════════════════ */

void server_spawn(TrixieServer *s, const char *cmd);
void server_focus_pane(TrixieServer *s, PaneId id);
void server_sync_focus(TrixieServer *s);
void server_sync_windows(TrixieServer *s);
void server_request_redraw(TrixieServer *s);
void server_dispatch_action(TrixieServer *s, Action *a);
void server_float_toggle(TrixieServer *s);
void server_scratch_toggle(TrixieServer *s, const char *name);
void server_apply_config_reload(TrixieServer *s);
void server_reset_idle(TrixieServer *s);
void server_init_ipc(TrixieServer *s);
void server_init_config_watch(TrixieServer *s);

TrixieView *view_from_pane(TrixieServer *s, PaneId id);
TrixieView *view_at(TrixieServer        *s,
                    double               lx,
                    double               ly,
                    struct wlr_surface **surf_out,
                    double              *sx,
                    double              *sy);

void ipc_dispatch(TrixieServer *s, const char *line, char *reply, size_t reply_sz);
bool ipc_subscribe(TrixieServer *s, int client_fd);
void ipc_push_focus_changed(TrixieServer *s);
void ipc_push_workspace_changed(TrixieServer *s);
void ipc_push_title_changed(TrixieServer *s, PaneId id);

/* ═══════════════════════════════════════════════════════════════════════════
 * §13  Lua scripting layer
 * ═══════════════════════════════════════════════════════════════════════════ */

void lua_init(TrixieServer *s);
void lua_reload(TrixieServer *s);
void lua_destroy(TrixieServer *s);
bool lua_dispatch_key(TrixieServer *s, uint32_t mods, xkb_keysym_t sym);
void lua_apply_window_rules(TrixieServer *s,
                            Pane         *p,
                            const char   *app_id,
                            const char   *title);
void lua_on_window_open(TrixieServer *s, PaneId id);
void lua_on_window_close(TrixieServer *s, PaneId id);
void lua_on_focus_changed(TrixieServer *s);
void lua_on_workspace_changed(TrixieServer *s);
void lua_on_title_changed(TrixieServer *s, PaneId id);
