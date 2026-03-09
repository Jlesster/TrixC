/* lua.c — LuaJIT scripting layer for the Trixie compositor.
 *
 * ── API exposed to Lua ───────────────────────────────────────────────────────
 *
 * Actions
 * ───────
 *   trixie.spawn(cmd)                    -- exec a shell command
 *   trixie.focus("left"|"right"|"up"|"down")
 *   trixie.workspace(n)                  -- switch to workspace n (1-based)
 *   trixie.move_to_workspace(n)          -- move focused pane to workspace n
 *   trixie.layout("bsp"|"spiral"|"columns"|"rows"|"threecol"|"monocle")
 *   trixie.layout_next()
 *   trixie.layout_prev()
 *   trixie.close()                       -- close focused window
 *   trixie.float()                       -- toggle float on focused window
 *   trixie.fullscreen()                  -- toggle fullscreen
 *   trixie.scratchpad(name)              -- toggle named scratchpad
 *   trixie.grow_main()
 *   trixie.shrink_main()
 *   trixie.ratio(r)                      -- set main_ratio (0.1–0.9)
 *   trixie.swap()                        -- swap pane forward
 *   trixie.swap_back()
 *   trixie.swap_main()
 *   trixie.float_move(dx, dy)
 *   trixie.float_resize(dw, dh)
 *   trixie.reload()                      -- hot-reload config + re-exec init.lua
 *   trixie.quit()
 *
 * Keybinds
 * ────────
 *   trixie.bind("super+return", fn)
 *   trixie.bind("super+shift+q", fn)
 *   trixie.bind("ctrl+alt+t", fn)
 *   -- modifiers: super ctrl alt shift (case-insensitive, any order, + separated)
 *   -- key names: xkb key names (return, space, tab, a–z, 0–9, F1–F12, …)
 *
 * Window rules
 * ────────────
 *   trixie.rule({ app_id = "firefox" }, {
 *     workspace  = 2,        -- send to this workspace on open
 *     follow     = true,     -- also switch focus to that workspace
 *     float      = true,     -- open as floating
 *     fullscreen = false,
 *     noborder   = false,
 *     notitle    = false,
 *     opacity    = 0.95,
 *     x = 100, y = 100,      -- initial float position
 *     w = 1200, h = 800,     -- initial float size
 *   })
 *   trixie.rule({ title = "nvim" }, { workspace = 1 })
 *
 * Event hooks
 * ───────────
 *   trixie.on("window_open",      function(ev) end)
 *   trixie.on("window_close",     function(ev) end)
 *   trixie.on("focus_changed",    function(ev) end)
 *   trixie.on("workspace_changed",function(ev) end)
 *   trixie.on("title_changed",    function(ev) end)
 *
 *   Event table fields:
 *     ev.pane_id   (number)
 *     ev.app_id    (string)
 *     ev.title     (string)
 *     ev.workspace (number, 1-based)
 *     ev.floating  (boolean)
 *
 * Query
 * ─────
 *   trixie.get_workspace()   → number
 *   trixie.get_focused()     → { id, title, app_id, floating, fullscreen,
 *                                workspace, rect={x,y,w,h} }  or nil
 *   trixie.get_panes()       → array of pane tables (same fields as above)
 *   trixie.get_panes_all()   → all panes across all workspaces
 *
 * Config overrides  (take effect immediately, survive reload only if re-set)
 * ─────────────────
 *   trixie.set("gap",              4)
 *   trixie.set("outer_gap",        8)
 *   trixie.set("border_width",     2)
 *   trixie.set("theme",            "catppuccin-mocha")
 *   trixie.set("background_color", "#1e1e2e")
 *   trixie.set("active_border",    "#94e2d5")
 *   trixie.set("inactive_border",  "#313244")
 *   trixie.set("bar_accent",       "#94e2d5")
 *   trixie.set("font_size",        14.0)
 *   trixie.set("smart_gaps",       true)
 *
 * Logging
 * ───────
 *   trixie.log("message")    -- emits via wlr_log at WLR_INFO level
 *   trixie.warn("message")   -- emits via wlr_log at WLR_ERROR level
 */

#define _POSIX_C_SOURCE 200809L
#include "trixie.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* LuaJIT headers */
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#if LUA_VERSION_NUM >= 502
#define lua_objlen(L, i) lua_rawlen(L, i)
#endif

/* ── Constants ──────────────────────────────────────────────────────────────── */

#define MAX_LUA_HOOKS 8   /* max callbacks per event type           */
#define MAX_LUA_BINDS 256 /* max Lua-registered keybinds            */
#define MAX_LUA_RULES 64  /* max Lua-registered window rules        */

/* Registry keys for the server pointer and hook tables stored in Lua registry */
#define LUA_REG_SERVER "trixie_server"
#define LUA_REG_HOOKS  "trixie_hooks" /* table: event_name → {fn, ...}  */

/* ── LuaServer — wraps TrixieServer for the Lua state ──────────────────────── */

typedef struct {
  lua_State    *L;
  TrixieServer *server;
  bool          loaded;

  /* Lua-registered keybinds — stored here and also injected into cfg.keybinds
   * so the existing keyboard handler fires them. We use ACTION_LUA_CALLBACK
   * with a unique cookie that maps back to the Lua function registry reference. */
  struct {
    uint32_t     mods;
    xkb_keysym_t sym;
    int          fn_ref; /* luaL_ref index in LUA_REGISTRYINDEX */
  } binds[MAX_LUA_BINDS];
  int bind_count;

  /* Lua-registered window rules — merged on top of cfg.win_rules at rule-check
   * time in lua_apply_window_rules().                                         */
  struct {
    char  app_id[128]; /* empty = match any                             */
    char  title[128];  /* if starts with "title:" match title substring */
    int   workspace;   /* 0 = not set                                   */
    bool  follow;      /* switch to workspace when window opens         */
    bool  set_float;
    bool  float_val;
    bool  set_fullscreen;
    bool  fullscreen_val;
    bool  noborder;
    bool  notitle;
    float opacity; /* 0 = not set                                   */
    int   x, y;    /* 0 = not set                                   */
    int   w, h;
  } rules[MAX_LUA_RULES];
  int rule_count;
} LuaServer;

/* Global singleton — one compositor, one Lua state */
static LuaServer g_lua;

/* ── Forward declarations ───────────────────────────────────────────────────── */

static TrixieServer *get_server(lua_State *L);
static void lua_push_pane_table(lua_State *L, TrixieServer *s, Pane *p, int ws_idx);

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static TrixieServer *get_server(lua_State *L) {
  lua_pushstring(L, LUA_REG_SERVER);
  lua_rawget(L, LUA_REGISTRYINDEX);
  TrixieServer *s = lua_touserdata(L, -1);
  lua_pop(L, 1);
  return s;
}

/* Push a full pane-info table onto the Lua stack. */
static void lua_push_pane_table(lua_State *L, TrixieServer *s, Pane *p, int ws_idx) {
  lua_createtable(L, 0, 10);

  lua_pushnumber(L, p->id);
  lua_setfield(L, -2, "id");
  lua_pushstring(L, p->title);
  lua_setfield(L, -2, "title");
  lua_pushstring(L, p->app_id);
  lua_setfield(L, -2, "app_id");
  lua_pushboolean(L, p->floating);
  lua_setfield(L, -2, "floating");
  lua_pushboolean(L, p->fullscreen);
  lua_setfield(L, -2, "fullscreen");
  lua_pushnumber(L, ws_idx + 1);
  lua_setfield(L, -2, "workspace");

  /* rect sub-table */
  lua_createtable(L, 0, 4);
  lua_pushnumber(L, p->rect.x);
  lua_setfield(L, -2, "x");
  lua_pushnumber(L, p->rect.y);
  lua_setfield(L, -2, "y");
  lua_pushnumber(L, p->rect.w);
  lua_setfield(L, -2, "w");
  lua_pushnumber(L, p->rect.h);
  lua_setfield(L, -2, "h");
  lua_setfield(L, -2, "rect");

  bool is_focused = (twm_focused_id(&s->twm) == p->id);
  lua_pushboolean(L, is_focused);
  lua_setfield(L, -2, "focused");
}

/* Parse "super+shift+return" → mods bitmask + xkb_keysym_t.
 * Returns false and sets errbuf on parse failure. */
static bool parse_bind_string(const char   *spec,
                              uint32_t     *mods_out,
                              xkb_keysym_t *sym_out,
                              char         *errbuf,
                              size_t        errsz) {
  char     buf[256];
  uint32_t mods = 0;
  strncpy(buf, spec, sizeof(buf) - 1);

  /* split on '+', last token is the key name */
  char *parts[16];
  int   np = 0;
  char *p  = buf;
  while(p && np < 15) {
    char *plus = strchr(p, '+');
    if(plus) *plus = '\0';
    parts[np++] = p;
    p           = plus ? plus + 1 : NULL;
  }
  if(np < 1) {
    snprintf(errbuf, errsz, "empty bind spec");
    return false;
  }

  /* all but last token are modifiers */
  for(int i = 0; i < np - 1; i++) {
    if(!strcasecmp(parts[i], "super"))
      mods |= MOD_SUPER;
    else if(!strcasecmp(parts[i], "ctrl") || !strcasecmp(parts[i], "control"))
      mods |= MOD_CTRL;
    else if(!strcasecmp(parts[i], "alt"))
      mods |= MOD_ALT;
    else if(!strcasecmp(parts[i], "shift"))
      mods |= MOD_SHIFT;
    else {
      snprintf(errbuf, errsz, "unknown modifier '%s'", parts[i]);
      return false;
    }
  }

  /* last token is key name — look up via xkb */
  xkb_keysym_t sym =
      xkb_keysym_from_name(parts[np - 1], XKB_KEYSYM_CASE_INSENSITIVE);
  if(sym == XKB_KEY_NoSymbol) {
    snprintf(errbuf, errsz, "unknown key '%s'", parts[np - 1]);
    return false;
  }
  *mods_out = mods;
  *sym_out  = sym;
  return true;
}

/* Find which workspace a pane lives on (-1 if not found). */
static int pane_workspace(TrixieServer *s, PaneId id) {
  for(int i = 0; i < s->twm.ws_count; i++)
    for(int j = 0; j < s->twm.workspaces[i].pane_count; j++)
      if(s->twm.workspaces[i].panes[j] == id) return i;
  return -1;
}

/* Translate a layout name string to Layout enum (-1 on failure). */
static int layout_from_name(const char *name) {
  if(!strcasecmp(name, "bsp")) return LAYOUT_BSP;
  if(!strcasecmp(name, "spiral")) return LAYOUT_SPIRAL;
  if(!strcasecmp(name, "columns")) return LAYOUT_COLUMNS;
  if(!strcasecmp(name, "rows")) return LAYOUT_ROWS;
  if(!strcasecmp(name, "threecol")) return LAYOUT_THREECOL;
  if(!strcasecmp(name, "monocle")) return LAYOUT_MONOCLE;
  return -1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  trixie.* action bindings
 * ═══════════════════════════════════════════════════════════════════════════ */

static int l_spawn(lua_State *L) {
  const char   *cmd = luaL_checkstring(L, 1);
  TrixieServer *s   = get_server(L);
  if(s) server_spawn(s, cmd);
  return 0;
}

static int l_focus(lua_State *L) {
  const char   *dir = luaL_checkstring(L, 1);
  TrixieServer *s   = get_server(L);
  if(!s) return 0;
  Action a = { 0 };
  if(!strcmp(dir, "left"))
    a.kind = ACTION_FOCUS_LEFT;
  else if(!strcmp(dir, "right"))
    a.kind = ACTION_FOCUS_RIGHT;
  else if(!strcmp(dir, "up"))
    a.kind = ACTION_FOCUS_UP;
  else if(!strcmp(dir, "down"))
    a.kind = ACTION_FOCUS_DOWN;
  else
    luaL_error(L, "trixie.focus: invalid direction '%s'", dir);
  server_dispatch_action(s, &a);
  return 0;
}

static int l_workspace(lua_State *L) {
  int           n = (int)luaL_checkinteger(L, 1);
  TrixieServer *s = get_server(L);
  if(!s) return 0;
  if(n < 1 || n > s->twm.ws_count) luaL_error(L, "workspace %d out of range", n);
  int old = s->twm.active_ws;
  twm_switch_ws(&s->twm, n - 1);
  if(s->twm.active_ws != old)
    anim_workspace_transition(&s->anim,
                              s->twm.active_ws > old ? WS_DIR_RIGHT : WS_DIR_LEFT);
  server_sync_focus(s);
  server_sync_windows(s);
  ipc_push_workspace_changed(s);
  return 0;
}

static int l_move_to_workspace(lua_State *L) {
  int           n = (int)luaL_checkinteger(L, 1);
  TrixieServer *s = get_server(L);
  if(!s) return 0;
  if(n < 1 || n > s->twm.ws_count) luaL_error(L, "workspace %d out of range", n);
  twm_move_to_ws(&s->twm, n - 1);
  server_sync_windows(s);
  return 0;
}

static int l_layout(lua_State *L) {
  const char   *name = luaL_checkstring(L, 1);
  TrixieServer *s    = get_server(L);
  if(!s) return 0;
  int lay = layout_from_name(name);
  if(lay < 0) luaL_error(L, "unknown layout '%s'", name);
  s->twm.workspaces[s->twm.active_ws].layout = (Layout)lay;
  twm_reflow(&s->twm);
  server_sync_windows(s);
  return 0;
}

static int l_layout_next(lua_State *L) {
  TrixieServer *s = get_server(L);
  if(!s) return 0;
  Action a = { .kind = ACTION_NEXT_LAYOUT };
  server_dispatch_action(s, &a);
  return 0;
}

static int l_layout_prev(lua_State *L) {
  TrixieServer *s = get_server(L);
  if(!s) return 0;
  Action a = { .kind = ACTION_PREV_LAYOUT };
  server_dispatch_action(s, &a);
  return 0;
}

static int l_close(lua_State *L) {
  TrixieServer *s = get_server(L);
  if(!s) return 0;
  Action a = { .kind = ACTION_CLOSE };
  server_dispatch_action(s, &a);
  return 0;
}

static int l_float(lua_State *L) {
  TrixieServer *s = get_server(L);
  if(!s) return 0;
  server_float_toggle(s);
  return 0;
}

static int l_fullscreen(lua_State *L) {
  TrixieServer *s = get_server(L);
  if(!s) return 0;
  Action a = { .kind = ACTION_FULLSCREEN };
  server_dispatch_action(s, &a);
  return 0;
}

static int l_scratchpad(lua_State *L) {
  const char   *name = luaL_checkstring(L, 1);
  TrixieServer *s    = get_server(L);
  if(!s) return 0;
  server_scratch_toggle(s, name);
  return 0;
}

static int l_grow_main(lua_State *L) {
  TrixieServer *s = get_server(L);
  if(!s) return 0;
  Action a = { .kind = ACTION_GROW_MAIN };
  server_dispatch_action(s, &a);
  return 0;
}

static int l_shrink_main(lua_State *L) {
  TrixieServer *s = get_server(L);
  if(!s) return 0;
  Action a = { .kind = ACTION_SHRINK_MAIN };
  server_dispatch_action(s, &a);
  return 0;
}

static int l_ratio(lua_State *L) {
  float         r = (float)luaL_checknumber(L, 1);
  TrixieServer *s = get_server(L);
  if(!s) return 0;
  if(r < 0.1f) r = 0.1f;
  if(r > 0.9f) r = 0.9f;
  s->twm.workspaces[s->twm.active_ws].main_ratio = r;
  twm_reflow(&s->twm);
  server_sync_windows(s);
  return 0;
}

static int l_swap(lua_State *L) {
  TrixieServer *s = get_server(L);
  if(!s) return 0;
  twm_swap(&s->twm, true);
  server_sync_windows(s);
  return 0;
}

static int l_swap_back(lua_State *L) {
  TrixieServer *s = get_server(L);
  if(!s) return 0;
  twm_swap(&s->twm, false);
  server_sync_windows(s);
  return 0;
}

static int l_swap_main(lua_State *L) {
  TrixieServer *s = get_server(L);
  if(!s) return 0;
  twm_swap_main(&s->twm);
  server_sync_windows(s);
  return 0;
}

static int l_float_move(lua_State *L) {
  int           dx = (int)luaL_checkinteger(L, 1);
  int           dy = (int)luaL_checkinteger(L, 2);
  TrixieServer *s  = get_server(L);
  if(!s) return 0;
  PaneId id = twm_focused_id(&s->twm);
  if(id) {
    twm_float_move(&s->twm, id, dx, dy);
    server_sync_windows(s);
  }
  return 0;
}

static int l_float_resize(lua_State *L) {
  int           dw = (int)luaL_checkinteger(L, 1);
  int           dh = (int)luaL_checkinteger(L, 2);
  TrixieServer *s  = get_server(L);
  if(!s) return 0;
  PaneId id = twm_focused_id(&s->twm);
  if(id) {
    twm_float_resize(&s->twm, id, dw, dh);
    server_sync_windows(s);
  }
  return 0;
}

static int l_reload(lua_State *L) {
  TrixieServer *s = get_server(L);
  if(!s) return 0;
  server_apply_config_reload(s);
  /* Re-exec init.lua so config overrides survive the reload */
  lua_reload(s);
  return 0;
}

static int l_quit(lua_State *L) {
  TrixieServer *s = get_server(L);
  if(!s) return 0;
  s->running = false;
  wl_display_terminate(s->display);
  return 0;
}

static int l_log(lua_State *L) {
  const char *msg = luaL_checkstring(L, 1);
  wlr_log(WLR_INFO, "[lua] %s", msg);
  return 0;
}

static int l_warn(lua_State *L) {
  const char *msg = luaL_checkstring(L, 1);
  wlr_log(WLR_ERROR, "[lua] WARN: %s", msg);
  return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  trixie.bind — register Lua keybinds
 * ═══════════════════════════════════════════════════════════════════════════ */

static int l_bind(lua_State *L) {
  const char *spec = luaL_checkstring(L, 1);
  luaL_checktype(L, 2, LUA_TFUNCTION);

  if(g_lua.bind_count >= MAX_LUA_BINDS)
    luaL_error(L, "trixie.bind: too many binds (max %d)", MAX_LUA_BINDS);

  uint32_t     mods;
  xkb_keysym_t sym;
  char         errbuf[128];
  if(!parse_bind_string(spec, &mods, &sym, errbuf, sizeof(errbuf)))
    luaL_error(L, "trixie.bind: %s", errbuf);

  /* Store function in Lua registry */
  lua_pushvalue(L, 2);
  int ref = luaL_ref(L, LUA_REGISTRYINDEX);

  /* Check for existing bind with same mods+sym and replace it */
  for(int i = 0; i < g_lua.bind_count; i++) {
    if(g_lua.binds[i].mods == mods && g_lua.binds[i].sym == sym) {
      luaL_unref(L, LUA_REGISTRYINDEX, g_lua.binds[i].fn_ref);
      g_lua.binds[i].fn_ref = ref;
      return 0;
    }
  }

  g_lua.binds[g_lua.bind_count].mods   = mods;
  g_lua.binds[g_lua.bind_count].sym    = sym;
  g_lua.binds[g_lua.bind_count].fn_ref = ref;
  g_lua.bind_count++;

  wlr_log(WLR_INFO, "[lua] registered bind: %s", spec);
  return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  trixie.rule — register Lua window rules
 * ═══════════════════════════════════════════════════════════════════════════ */

static int l_rule(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE); /* match criteria */
  luaL_checktype(L, 2, LUA_TTABLE); /* rule actions   */

  if(g_lua.rule_count >= MAX_LUA_RULES)
    luaL_error(L, "trixie.rule: too many rules (max %d)", MAX_LUA_RULES);

  __typeof__(g_lua.rules[0]) r;
  memset(&r, 0, sizeof(r));

  /* Parse match criteria */
  lua_getfield(L, 1, "app_id");
  if(lua_isstring(L, -1))
    strncpy(r.app_id, lua_tostring(L, -1), sizeof(r.app_id) - 1);
  lua_pop(L, 1);

  lua_getfield(L, 1, "title");
  if(lua_isstring(L, -1)) strncpy(r.title, lua_tostring(L, -1), sizeof(r.title) - 1);
  lua_pop(L, 1);

  /* Parse rule actions */
  lua_getfield(L, 2, "workspace");
  if(lua_isnumber(L, -1)) r.workspace = (int)lua_tonumber(L, -1);
  lua_pop(L, 1);

  lua_getfield(L, 2, "follow");
  if(lua_isboolean(L, -1)) r.follow = lua_toboolean(L, -1);
  lua_pop(L, 1);

  lua_getfield(L, 2, "float");
  if(lua_isboolean(L, -1)) {
    r.set_float = true;
    r.float_val = lua_toboolean(L, -1);
  }
  lua_pop(L, 1);

  lua_getfield(L, 2, "fullscreen");
  if(lua_isboolean(L, -1)) {
    r.set_fullscreen = true;
    r.fullscreen_val = lua_toboolean(L, -1);
  }
  lua_pop(L, 1);

  lua_getfield(L, 2, "noborder");
  if(lua_isboolean(L, -1)) r.noborder = lua_toboolean(L, -1);
  lua_pop(L, 1);

  lua_getfield(L, 2, "notitle");
  if(lua_isboolean(L, -1)) r.notitle = lua_toboolean(L, -1);
  lua_pop(L, 1);

  lua_getfield(L, 2, "opacity");
  if(lua_isnumber(L, -1)) r.opacity = (float)lua_tonumber(L, -1);
  lua_pop(L, 1);

  lua_getfield(L, 2, "x");
  if(lua_isnumber(L, -1)) r.x = (int)lua_tonumber(L, -1);
  lua_pop(L, 1);

  lua_getfield(L, 2, "y");
  if(lua_isnumber(L, -1)) r.y = (int)lua_tonumber(L, -1);
  lua_pop(L, 1);

  lua_getfield(L, 2, "w");
  if(lua_isnumber(L, -1)) r.w = (int)lua_tonumber(L, -1);
  lua_pop(L, 1);

  lua_getfield(L, 2, "h");
  if(lua_isnumber(L, -1)) r.h = (int)lua_tonumber(L, -1);
  lua_pop(L, 1);

  g_lua.rules[g_lua.rule_count++] = r;
  return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §5  trixie.on — event hook registration
 * ═══════════════════════════════════════════════════════════════════════════ */

static int l_on(lua_State *L) {
  const char *event = luaL_checkstring(L, 1);
  luaL_checktype(L, 2, LUA_TFUNCTION);

  /* Validate event name */
  static const char *valid_events[] = { "window_open",   "window_close",
                                        "focus_changed", "workspace_changed",
                                        "title_changed", NULL };
  bool               found          = false;
  for(int i = 0; valid_events[i]; i++)
    if(!strcmp(event, valid_events[i])) {
      found = true;
      break;
    }
  if(!found) luaL_error(L, "trixie.on: unknown event '%s'", event);

  /* hooks table: LUA_REGISTRYINDEX[LUA_REG_HOOKS][event] = {fn, ...} */
  lua_pushstring(L, LUA_REG_HOOKS);
  lua_rawget(L, LUA_REGISTRYINDEX); /* hooks table */

  lua_getfield(L, -1, event); /* hooks[event] */
  if(lua_isnil(L, -1)) {
    lua_pop(L, 1);
    lua_createtable(L, MAX_LUA_HOOKS, 0);
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, event); /* hooks[event] = {} */
  }

  /* append fn to array */
  int n = (int)lua_objlen(L, -1);
  lua_pushvalue(L, 2);
  lua_rawseti(L, -2, n + 1);

  lua_pop(L, 2); /* hooks[event], hooks */
  return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §6  trixie.get_* query functions
 * ═══════════════════════════════════════════════════════════════════════════ */

static int l_get_workspace(lua_State *L) {
  TrixieServer *s = get_server(L);
  if(!s) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushinteger(L, s->twm.active_ws + 1);
  return 1;
}

static int l_get_focused(lua_State *L) {
  TrixieServer *s = get_server(L);
  if(!s) {
    lua_pushnil(L);
    return 1;
  }
  PaneId id = twm_focused_id(&s->twm);
  if(!id) {
    lua_pushnil(L);
    return 1;
  }
  Pane *p = twm_pane_by_id(&s->twm, id);
  if(!p) {
    lua_pushnil(L);
    return 1;
  }
  int ws = pane_workspace(s, id);
  lua_push_pane_table(L, s, p, ws);
  return 1;
}

static int l_get_panes(lua_State *L) {
  TrixieServer *s = get_server(L);
  if(!s) {
    lua_createtable(L, 0, 0);
    return 1;
  }
  Workspace *ws = &s->twm.workspaces[s->twm.active_ws];
  lua_createtable(L, ws->pane_count, 0);
  for(int i = 0; i < ws->pane_count; i++) {
    Pane *p = twm_pane_by_id(&s->twm, ws->panes[i]);
    if(!p) continue;
    lua_push_pane_table(L, s, p, s->twm.active_ws);
    lua_rawseti(L, -2, i + 1);
  }
  return 1;
}

static int l_get_panes_all(lua_State *L) {
  TrixieServer *s = get_server(L);
  if(!s) {
    lua_createtable(L, 0, 0);
    return 1;
  }
  lua_createtable(L, s->twm.pane_count, 0);
  int idx = 1;
  for(int wi = 0; wi < s->twm.ws_count; wi++) {
    Workspace *ws = &s->twm.workspaces[wi];
    for(int j = 0; j < ws->pane_count; j++) {
      Pane *p = twm_pane_by_id(&s->twm, ws->panes[j]);
      if(!p) continue;
      lua_push_pane_table(L, s, p, wi);
      lua_rawseti(L, -2, idx++);
    }
  }
  return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §7  trixie.set — live config overrides
 * ═══════════════════════════════════════════════════════════════════════════ */

static int l_set(lua_State *L) {
  const char   *key = luaL_checkstring(L, 1);
  TrixieServer *s   = get_server(L);
  if(!s) return 0;
  Config *c = &s->cfg;

  if(!strcmp(key, "gap")) {
    s->twm.gap = (int)luaL_checkinteger(L, 2);
    twm_reflow(&s->twm);
    server_sync_windows(s);

  } else if(!strcmp(key, "outer_gap")) {
    s->twm.padding = (int)luaL_checkinteger(L, 2);
    c->outer_gap   = s->twm.padding;
    twm_set_bar_height(&s->twm, c->bar.height, c->bar.position == BAR_BOTTOM);
    server_sync_windows(s);

  } else if(!strcmp(key, "border_width")) {
    int bw          = (int)luaL_checkinteger(L, 2);
    c->border_width = bw;
    s->twm.border_w = bw;
    server_sync_windows(s);

  } else if(!strcmp(key, "smart_gaps")) {
    luaL_checktype(L, 2, LUA_TBOOLEAN);
    c->smart_gaps = s->twm.smart_gaps = lua_toboolean(L, 2);
    twm_reflow(&s->twm);
    server_sync_windows(s);

  } else if(!strcmp(key, "font_size")) {
    c->font_size = (float)luaL_checknumber(L, 2);

  } else if(!strcmp(key, "theme")) {
    /* apply_theme is static in config.c — we call config_reload workaround:
     * write the theme key into cfg then trigger the internal apply path via
     * config_set_theme(), which we expose from config.c. */
    const char *theme = luaL_checkstring(L, 2);
    config_set_theme(c, theme);
    /* Propagate colours to bar and deco immediately */
    TrixieOutput *o;
    wl_list_for_each(o, &s->outputs, link) if(o->bar) bar_mark_dirty(o->bar);
    server_request_redraw(s);

  } else if(!strcmp(key, "background_color") || !strcmp(key, "background")) {
    const char *hex = luaL_checkstring(L, 2);
    /* Parse hex manually — color_from_str is static in config.c, so we inline */
    uint32_t    rgb = (uint32_t)strtoul(hex[0] == '#' ? hex + 1 : hex, NULL, 16);
    c->colors.background = color_hex(rgb);
    if(s->bg_rect) {
      Color bc    = c->colors.background;
      float fc[4] = { bc.r / 255.f, bc.g / 255.f, bc.b / 255.f, bc.a / 255.f };
      wlr_scene_rect_set_color(s->bg_rect, fc);
    }

  } else if(!strcmp(key, "active_border")) {
    uint32_t rgb = (uint32_t)strtoul(
        luaL_checkstring(L, 2) + (*(luaL_checkstring(L, 2)) == '#'), NULL, 16);
    c->colors.active_border = color_hex(rgb);
    server_request_redraw(s);

  } else if(!strcmp(key, "inactive_border")) {
    uint32_t rgb = (uint32_t)strtoul(
        luaL_checkstring(L, 2) + (*(luaL_checkstring(L, 2)) == '#'), NULL, 16);
    c->colors.inactive_border = color_hex(rgb);
    server_request_redraw(s);

  } else if(!strcmp(key, "bar_accent")) {
    uint32_t rgb = (uint32_t)strtoul(
        luaL_checkstring(L, 2) + (*(luaL_checkstring(L, 2)) == '#'), NULL, 16);
    c->colors.bar_accent = c->bar.accent = color_hex(rgb);
    TrixieOutput *o;
    wl_list_for_each(o, &s->outputs, link) if(o->bar) bar_mark_dirty(o->bar);

  } else {
    wlr_log(WLR_ERROR, "[lua] trixie.set: unknown key '%s'", key);
    lua_pushstring(L, "unknown key");
    return lua_error(L);
  }

  return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §8  Module registration
 * ═══════════════════════════════════════════════════════════════════════════ */

static const luaL_Reg trixie_lib[] = {
  /* actions */
  { "spawn",             l_spawn             },
  { "focus",             l_focus             },
  { "workspace",         l_workspace         },
  { "move_to_workspace", l_move_to_workspace },
  { "layout",            l_layout            },
  { "layout_next",       l_layout_next       },
  { "layout_prev",       l_layout_prev       },
  { "close",             l_close             },
  { "float",             l_float             },
  { "fullscreen",        l_fullscreen        },
  { "scratchpad",        l_scratchpad        },
  { "grow_main",         l_grow_main         },
  { "shrink_main",       l_shrink_main       },
  { "ratio",             l_ratio             },
  { "swap",              l_swap              },
  { "swap_back",         l_swap_back         },
  { "swap_main",         l_swap_main         },
  { "float_move",        l_float_move        },
  { "float_resize",      l_float_resize      },
  { "reload",            l_reload            },
  { "quit",              l_quit              },
  /* keybinds + rules */
  { "bind",              l_bind              },
  { "rule",              l_rule              },
  /* hooks */
  { "on",                l_on                },
  /* query */
  { "get_workspace",     l_get_workspace     },
  { "get_focused",       l_get_focused       },
  { "get_panes",         l_get_panes         },
  { "get_panes_all",     l_get_panes_all     },
  /* config */
  { "set",               l_set               },
  /* logging */
  { "log",               l_log               },
  { "warn",              l_warn              },
  { NULL,                NULL                }
};

static int luaopen_trixie(lua_State *L) {
  luaL_newlib(L, trixie_lib); /* creates table + registers all funcs */

  lua_pushstring(L, TRIXIE_VERSION_STR);
  lua_setfield(L, -2, "version");

  return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §9  Public API — init / reload / dispatch
 * ═══════════════════════════════════════════════════════════════════════════ */

void lua_init(TrixieServer *s) {
  memset(&g_lua, 0, sizeof(g_lua));
  g_lua.server = s;

  lua_State *L = luaL_newstate();
  if(!L) {
    wlr_log(WLR_ERROR, "[lua] failed to create LuaJIT state");
    return;
  }
  luaL_openlibs(L);
  g_lua.L = L;

  /* Store server pointer in Lua registry */
  lua_pushstring(L, LUA_REG_SERVER);
  lua_pushlightuserdata(L, s);
  lua_rawset(L, LUA_REGISTRYINDEX);

  /* Create hooks table in registry */
  lua_pushstring(L, LUA_REG_HOOKS);
  lua_createtable(L, 0, 8);
  lua_rawset(L, LUA_REGISTRYINDEX);

  /* Register trixie module as a global */
  luaopen_trixie(L);
  lua_setglobal(L, "trixie");

  /* Load init.lua */
  lua_reload(s);
}

void lua_reload(TrixieServer *s) {
  lua_State *L = g_lua.L;
  if(!L) return;

  /* Clear Lua-registered rules and binds — they will be re-registered by the
   * script.  Free function references first to avoid leaking Lua objects. */
  for(int i = 0; i < g_lua.bind_count; i++)
    luaL_unref(L, LUA_REGISTRYINDEX, g_lua.binds[i].fn_ref);
  g_lua.bind_count = 0;
  g_lua.rule_count = 0;

  /* Clear hooks table */
  lua_pushstring(L, LUA_REG_HOOKS);
  lua_createtable(L, 0, 8);
  lua_rawset(L, LUA_REGISTRYINDEX);

  /* Locate init.lua */
  char        path[512];
  const char *xdg = getenv("XDG_CONFIG_HOME");
  if(xdg)
    snprintf(path, sizeof(path), "%s/trixie/init.lua", xdg);
  else {
    const char *home = getenv("HOME");
    if(!home) home = "/root";
    snprintf(path, sizeof(path), "%s/.config/trixie/init.lua", home);
  }

  if(access(path, R_OK) != 0) {
    /* No init.lua is fine — Lua layer is optional */
    wlr_log(WLR_INFO, "[lua] no init.lua found at %s (optional)", path);
    return;
  }

  if(luaL_loadfile(L, path) != 0 || lua_pcall(L, 0, 0, 0) != 0) {
    const char *err = lua_tostring(L, -1);
    wlr_log(WLR_ERROR, "[lua] error in %s: %s", path, err ? err : "(nil)");
    lua_pop(L, 1);
    return;
  }

  g_lua.loaded = true;
  wlr_log(WLR_INFO,
          "[lua] loaded %s (%d binds, %d rules)",
          path,
          g_lua.bind_count,
          g_lua.rule_count);
}

void lua_destroy(TrixieServer *s) {
  (void)s;
  if(!g_lua.L) return;
  lua_State *L = g_lua.L;
  for(int i = 0; i < g_lua.bind_count; i++)
    luaL_unref(L, LUA_REGISTRYINDEX, g_lua.binds[i].fn_ref);
  lua_close(L);
  memset(&g_lua, 0, sizeof(g_lua));
}

/* ── lua_dispatch_key: called from the keyboard handler ────────────────────── */

bool lua_dispatch_key(TrixieServer *s, uint32_t mods, xkb_keysym_t sym) {
  if(!g_lua.L || g_lua.bind_count == 0) return false;
  lua_State *L = g_lua.L;

  for(int i = 0; i < g_lua.bind_count; i++) {
    if(g_lua.binds[i].mods != mods || g_lua.binds[i].sym != sym) continue;

    lua_rawgeti(L, LUA_REGISTRYINDEX, g_lua.binds[i].fn_ref);
    if(lua_pcall(L, 0, 0, 0) != 0) {
      const char *err = lua_tostring(L, -1);
      wlr_log(WLR_ERROR, "[lua] bind error: %s", err ? err : "(nil)");
      lua_pop(L, 1);
    }
    return true; /* consumed */
  }
  return false;
}

/* ── lua_fire_event: fire a named hook with an event table ─────────────────── */

static void fire_hooks(lua_State *L, const char *event) {
  /* hooks table is at top of stack — iterate array */
  int n = (int)lua_objlen(L, -1);
  for(int i = 1; i <= n; i++) {
    lua_rawgeti(L, -1, i);
    if(!lua_isfunction(L, -1)) {
      lua_pop(L, 1);
      continue;
    }
    lua_pushvalue(L, -3); /* push event table (3rd from top: hooks, array, fn) */
    if(lua_pcall(L, 1, 0, 0) != 0) {
      const char *err = lua_tostring(L, -1);
      wlr_log(WLR_ERROR, "[lua] %s hook error: %s", event, err ? err : "(nil)");
      lua_pop(L, 1);
    }
  }
}

void lua_on_window_open(TrixieServer *s, PaneId id) {
  if(!g_lua.L) return;
  lua_State *L = g_lua.L;
  Pane      *p = twm_pane_by_id(&s->twm, id);
  if(!p) return;
  int ws = pane_workspace(s, id);

  /* Build event table */
  lua_push_pane_table(L, s, p, ws);
  lua_pushstring(L, "window_open");
  lua_setfield(L, -2, "event");

  /* Fire hooks */
  lua_pushstring(L, LUA_REG_HOOKS);
  lua_rawget(L, LUA_REGISTRYINDEX);
  lua_getfield(L, -1, "window_open");
  if(lua_istable(L, -1)) {
    lua_pushvalue(L, -4); /* event table */
    lua_insert(L, -2);
    fire_hooks(L, "window_open");
    lua_pop(L, 1);
  } else {
    lua_pop(L, 1);
  }
  lua_pop(L, 2); /* hooks table + event table */
}

void lua_on_window_close(TrixieServer *s, PaneId id) {
  if(!g_lua.L) return;
  lua_State *L = g_lua.L;
  Pane      *p = twm_pane_by_id(&s->twm, id);

  lua_createtable(L, 0, 4);
  lua_pushnumber(L, id);
  lua_setfield(L, -2, "pane_id");
  lua_pushstring(L, p ? p->title : "");
  lua_setfield(L, -2, "title");
  lua_pushstring(L, p ? p->app_id : "");
  lua_setfield(L, -2, "app_id");
  lua_pushstring(L, "window_close");
  lua_setfield(L, -2, "event");

  lua_pushstring(L, LUA_REG_HOOKS);
  lua_rawget(L, LUA_REGISTRYINDEX);
  lua_getfield(L, -1, "window_close");
  if(lua_istable(L, -1)) {
    lua_pushvalue(L, -4);
    lua_insert(L, -2);
    fire_hooks(L, "window_close");
    lua_pop(L, 1);
  } else {
    lua_pop(L, 1);
  }
  lua_pop(L, 2);
}

void lua_on_focus_changed(TrixieServer *s) {
  if(!g_lua.L) return;
  lua_State *L  = g_lua.L;
  PaneId     id = twm_focused_id(&s->twm);
  Pane      *p  = id ? twm_pane_by_id(&s->twm, id) : NULL;
  int        ws = id ? pane_workspace(s, id) : s->twm.active_ws;

  if(p)
    lua_push_pane_table(L, s, p, ws);
  else {
    lua_createtable(L, 0, 2);
    lua_pushnumber(L, 0);
    lua_setfield(L, -2, "pane_id");
  }
  lua_pushstring(L, "focus_changed");
  lua_setfield(L, -2, "event");

  lua_pushstring(L, LUA_REG_HOOKS);
  lua_rawget(L, LUA_REGISTRYINDEX);
  lua_getfield(L, -1, "focus_changed");
  if(lua_istable(L, -1)) {
    lua_pushvalue(L, -4);
    lua_insert(L, -2);
    fire_hooks(L, "focus_changed");
    lua_pop(L, 1);
  } else {
    lua_pop(L, 1);
  }
  lua_pop(L, 2);
}

void lua_on_workspace_changed(TrixieServer *s) {
  if(!g_lua.L) return;
  lua_State *L = g_lua.L;

  lua_createtable(L, 0, 3);
  lua_pushnumber(L, s->twm.active_ws + 1);
  lua_setfield(L, -2, "workspace");
  lua_pushnumber(L, s->twm.workspaces[s->twm.active_ws].pane_count);
  lua_setfield(L, -2, "pane_count");
  lua_pushstring(L, "workspace_changed");
  lua_setfield(L, -2, "event");

  lua_pushstring(L, LUA_REG_HOOKS);
  lua_rawget(L, LUA_REGISTRYINDEX);
  lua_getfield(L, -1, "workspace_changed");
  if(lua_istable(L, -1)) {
    lua_pushvalue(L, -4);
    lua_insert(L, -2);
    fire_hooks(L, "workspace_changed");
    lua_pop(L, 1);
  } else {
    lua_pop(L, 1);
  }
  lua_pop(L, 2);
}

void lua_on_title_changed(TrixieServer *s, PaneId id) {
  if(!g_lua.L) return;
  lua_State *L = g_lua.L;
  Pane      *p = twm_pane_by_id(&s->twm, id);
  if(!p) return;
  int ws = pane_workspace(s, id);

  lua_push_pane_table(L, s, p, ws);
  lua_pushstring(L, "title_changed");
  lua_setfield(L, -2, "event");

  lua_pushstring(L, LUA_REG_HOOKS);
  lua_rawget(L, LUA_REGISTRYINDEX);
  lua_getfield(L, -1, "title_changed");
  if(lua_istable(L, -1)) {
    lua_pushvalue(L, -4);
    lua_insert(L, -2);
    fire_hooks(L, "title_changed");
    lua_pop(L, 1);
  } else {
    lua_pop(L, 1);
  }
  lua_pop(L, 2);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §10  lua_apply_window_rules — called from view_do_map, after C rules
 * ═══════════════════════════════════════════════════════════════════════════ */

void lua_apply_window_rules(TrixieServer *s,
                            Pane         *p,
                            const char   *app_id,
                            const char   *title) {
  if(!g_lua.L || g_lua.rule_count == 0) return;

  for(int i = 0; i < g_lua.rule_count; i++) {
    __typeof__(g_lua.rules[0]) *r = &g_lua.rules[i];

    /* Match criteria — must match both if both specified */
    bool match = true;
    if(r->app_id[0] && !strstr(app_id, r->app_id)) match = false;
    if(r->title[0] && !strstr(title, r->title)) match = false;
    if(!match) continue;

    /* Apply rule actions */
    if(r->set_float) p->floating = r->float_val;
    if(r->set_fullscreen) p->fullscreen = r->fullscreen_val;
    if(r->noborder) { /* injected into cfg.win_rules if possible; handled in deco */
    }
    if(r->opacity >
       0.f) { /* deco/sync_windows reads from cfg.win_rules — skip for now */
    }

    if(r->w > 0 && r->h > 0) {
      p->float_rect.w = r->w;
      p->float_rect.h = r->h;
      if(r->x == 0 && r->y == 0) {
        p->float_rect.x = (s->twm.screen_w - r->w) / 2;
        p->float_rect.y = (s->twm.screen_h - r->h) / 2;
      }
      p->floating = true;
    }
    if(r->x != 0 || r->y != 0) {
      p->float_rect.x = r->x;
      p->float_rect.y = r->y;
    }

    if(r->workspace >= 1 && r->workspace <= s->twm.ws_count) {
      twm_move_to_ws(&s->twm, r->workspace - 1);
      if(r->follow) {
        int old = s->twm.active_ws;
        twm_switch_ws(&s->twm, r->workspace - 1);
        if(s->twm.active_ws != old)
          anim_workspace_transition(
              &s->anim, s->twm.active_ws > old ? WS_DIR_RIGHT : WS_DIR_LEFT);
      }
    }
  }
}
