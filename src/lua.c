/* lua.c — Lua API for Trixie (AwesomeWM-style, Wayland)
 *
 * ── SIGNALS
 *   trixie.connect_signal(name, fn)   trixie.on(name, fn)
 *   trixie.disconnect_signal(name, fn)
 *   trixie.emit_signal(name, ...)
 *
 * ── CLIENT OBJECT  (passed to signals, returned from queries)
 *   c.id, c.title, c.app_id, c.workspace      (read)
 *   c.floating, c.fullscreen, c.ontop         (read/write)
 *   c.border_width, c.border_color            (read/write)
 *   c.opacity                                 (read/write)
 *   c.focused                                 (read)
 *   c.rect  → {x,y,w,h}                       (read)
 *   c:kill()  c:focus()  c:raise()
 *   c:move_to_workspace(n)
 *   c:connect_signal(name, fn)
 *   c:emit_signal(name, ...)
 *
 * ── SCREEN OBJECT
 *   s.index, s.geometry, s.workarea           (read)
 *   s.workspace                               (active ws, read/write)
 *   s:connect_signal(name, fn)
 *   trixie.screen.focused  trixie.screen.count  trixie.screen[n]
 *
 * ── WORKSPACE
 *   trixie.workspace(n)           switch to workspace n
 *   trixie.workspace.focused      current workspace index
 *   trixie.workspace.count
 *   trixie.workspace.layout       current layout enum value
 *
 * ── KEYBINDS
 *   trixie.key({mods}, key, fn [, fn_release])
 *
 * ── RULES
 *   trixie.rules.rules = { {rule={app_id=,name=}, properties={...}}, ... }
 *   properties: floating, fullscreen, workspace, opacity, border_width,
 * callback
 *
 * ── LAYOUTS
 *   trixie.layout.dwindle / columns / rows / threecol / monocle
 *   trixie.layout.set(layout)   trixie.layout.next()   trixie.layout.prev()
 *
 * ── WIBOX
 *   local wb = trixie.wibox({screen, x, y, width, height, position})
 *   wb:set_draw(function(canvas) ... end)  wb:redraw()  wb.visible
 *   canvas:fill_rect(x,y,w,h, argb)   canvas:draw_text(x,y,text, argb) → width
 *   canvas:clear(argb)   canvas:measure(text) → int
 *   canvas:width()   canvas:height()   canvas:font_height()
 * canvas:font_ascender()
 *
 * ── ACTIONS
 *   trixie.spawn(cmd)
 *   trixie.focus("left"|"right"|"up"|"down")
 *   trixie.close()   trixie.float()   trixie.fullscreen()
 *   trixie.scratchpad(name)
 *   trixie.grow_main()   trixie.shrink_main()   trixie.ratio(r)
 *   trixie.inc_master()  trixie.dec_master()
 *   trixie.swap()   trixie.swap_back()   trixie.swap_main()
 *   trixie.float_move(dx, dy)   trixie.float_resize(dw, dh)
 *   trixie.reload()   trixie.quit()
 *   trixie.dpms(true|false|"on"|"off")
 *   trixie.notify(summary [, body])
 *
 * ── SCRATCHPAD REGISTRATION
 *   trixie.register_scratchpad(name, {app_id, exec, width, height})
 *
 * ── CONFIG
 *   trixie.set(key, value)
 *     gap, outer_gap, border_width, border_color, active_border,
 *     inactive_border, background, font, font_size, smart_gaps,
 *     cursor_size, cursor_theme, kb_layout, kb_variant, kb_options,
 *     repeat_rate, repeat_delay, workspaces, xwayland, saturation,
 *     theme, idle_timeout
 *   trixie.get(key) → value   (reads back current config values)
 *
 * ── QUERIES
 *   trixie.focused()        → client or nil
 *   trixie.clients()        → array (active workspace)
 *   trixie.clients_all()    → array (all workspaces)
 *
 * ── TIMER
 *   local t = trixie.timer(interval_ms, fn [, repeating])
 *   t:cancel()
 *
 * ── SIGNALS EMITTED BY THE COMPOSITOR
 *   "startup"            on first start
 *   "reload"             after hot-reload completes
 *   "manage"    (c)      new window mapped
 *   "unmanage"  (c)      window closed
 *   "focus"     (c)      focus changed (c may be nil)
 *   "title_changed" (c)  window title updated
 *   "workspace_changed" (n, count)
 *   "screen_added"  (s)  new output connected
 *   "screen_removed"(s)  output disconnected
 *
 * ── LOGGING
 *   trixie.log(msg)   trixie.warn(msg)
 *   trixie.version    → string
 */

#define _POSIX_C_SOURCE 200809L
#include "trixie.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* ── Registry keys ─────────────────────────────────────────────────────── */
#define REG_SERVER "trixie_server"
#define REG_RULES "trixie_rules" /* array of {rule,properties} tables */

/* Metatable names */
#define MT_CLIENT "TrixieClient"
#define MT_SCREEN "TrixieScreen"
#define MT_WIBOX "TrixieWibox"
#define MT_CANVAS "TrixieCanvas"
#define MT_TIMER "TrixieTimer"

#if LUA_VERSION_NUM >= 502
#define lua_objlen(L, i) lua_rawlen(L, i)
#endif

/* ══════════════════════════════════════════════════════════════════════════
 * §0  Signal system (C implementation)
 * ══════════════════════════════════════════════════════════════════════════ */

void lua_signal_init(LuaSignalTable *t) {
  memset(t, 0, sizeof(*t));
  for (int i = 0; i < MAX_SIGNALS; i++)
    for (int j = 0; j < MAX_SIGNAL_CBS; j++)
      t->signals[i].fn_refs[j] = LUA_NOREF;
}

static LuaSignal *signal_find(LuaSignalTable *t, const char *name,
                              bool create) {
  for (int i = 0; i < t->count; i++)
    if (!strcmp(t->signals[i].name, name))
      return &t->signals[i];
  if (!create || t->count >= MAX_SIGNALS)
    return NULL;
  LuaSignal *s = &t->signals[t->count++];
  strncpy(s->name, name, MAX_SIGNAL_NAME - 1);
  s->count = 0;
  for (int j = 0; j < MAX_SIGNAL_CBS; j++)
    s->fn_refs[j] = LUA_NOREF;
  return s;
}

int lua_signal_connect(lua_State *L, LuaSignalTable *t, const char *name) {
  LuaSignal *s = signal_find(t, name, true);
  if (!s) {
    lua_pop(L, 1);
    return LUA_NOREF;
  }
  if (s->count >= MAX_SIGNAL_CBS) {
    lua_pop(L, 1);
    return LUA_NOREF;
  }
  /* find empty slot */
  for (int i = 0; i < MAX_SIGNAL_CBS; i++) {
    if (s->fn_refs[i] == LUA_NOREF) {
      lua_pushvalue(L, -1);
      s->fn_refs[i] = luaL_ref(L, LUA_REGISTRYINDEX);
      lua_pop(L, 1); /* pop original */
      s->count++;
      return s->fn_refs[i];
    }
  }
  lua_pop(L, 1);
  return LUA_NOREF;
}

void lua_signal_disconnect(lua_State *L, LuaSignalTable *t, const char *name,
                           int fn_ref) {
  LuaSignal *s = signal_find(t, name, false);
  if (!s)
    return;
  for (int i = 0; i < MAX_SIGNAL_CBS; i++) {
    if (s->fn_refs[i] == fn_ref) {
      luaL_unref(L, LUA_REGISTRYINDEX, fn_ref);
      s->fn_refs[i] = LUA_NOREF;
      s->count--;
      return;
    }
  }
}

void lua_signal_emit(lua_State *L, LuaSignalTable *t, const char *name,
                     int nargs) {
  LuaSignal *s = signal_find(t, name, false);
  if (!s || s->count == 0) {
    lua_pop(L, nargs);
    return;
  }
  /* Copy args so we can call multiple handlers */
  for (int i = 0; i < MAX_SIGNAL_CBS; i++) {
    if (s->fn_refs[i] == LUA_NOREF)
      continue;
    lua_rawgeti(L, LUA_REGISTRYINDEX, s->fn_refs[i]);
    /* push copies of args */
    for (int a = 0; a < nargs; a++)
      lua_pushvalue(L, -(nargs + 1));
    if (lua_pcall(L, nargs, 0, 0) != LUA_OK) {
      wlr_log(WLR_ERROR, "[lua] signal '%s': %s", name, lua_tostring(L, -1));
      lua_pop(L, 1);
    }
  }
  lua_pop(L, nargs);
}

/* Per-object signal table stored in Lua registry.
 * obj_ref: registry index of { signal_name = {fn_ref,...}, ... }
 * Pass LUA_NOREF to auto-create. */
void lua_signal_emit_obj(lua_State *L, int *obj_ref, const char *name,
                         int nargs) {
  if (*obj_ref == LUA_NOREF) {
    lua_pop(L, nargs);
    return;
  }
  lua_rawgeti(L, LUA_REGISTRYINDEX, *obj_ref);
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1 + nargs);
    return;
  }
  lua_getfield(L, -1, name);
  if (!lua_istable(L, -1)) {
    lua_pop(L, 2 + nargs);
    return;
  }
  int tbl = lua_gettop(L);
  int n = (int)lua_objlen(L, tbl);
  for (int i = 1; i <= n; i++) {
    lua_rawgeti(L, tbl, i);
    if (!lua_isfunction(L, -1)) {
      lua_pop(L, 1);
      continue;
    }
    for (int a = 0; a < nargs; a++)
      lua_pushvalue(L, -(nargs + 2)); /* args sit below tbl+fn */
    if (lua_pcall(L, nargs, 0, 0) != LUA_OK) {
      wlr_log(WLR_ERROR, "[lua] obj signal '%s': %s", name,
              lua_tostring(L, -1));
      lua_pop(L, 1);
    }
  }
  lua_pop(L, 2 + nargs); /* tbl + obj table + args */
}

/* ══════════════════════════════════════════════════════════════════════════
 * §1  Helpers
 * ══════════════════════════════════════════════════════════════════════════ */

static TrixieServer *get_server(lua_State *L) {
  lua_pushstring(L, REG_SERVER);
  lua_rawget(L, LUA_REGISTRYINDEX);
  TrixieServer *s = lua_touserdata(L, -1);
  lua_pop(L, 1);
  return s;
}

static uint32_t parse_hex_color(const char *s) {
  if (!s || !*s)
    return 0;
  if (*s == '#')
    s++;
  return (uint32_t)strtoul(s, NULL, 16);
}

static Color argb_to_color(uint32_t argb) {
  /* Lua passes 0xAARRGGBB from bit.bor(bit.lshift(a,24), r<<16, g<<8, b) */
  return (Color){
      .a = (argb >> 24) & 0xff,
      .r = (argb >> 16) & 0xff,
      .g = (argb >> 8) & 0xff,
      .b = argb & 0xff,
  };
}

static uint32_t color_to_argb(Color c) {
  return ((uint32_t)c.r << 24) | ((uint32_t)c.g << 16) | ((uint32_t)c.b << 8) |
         (uint32_t)c.a;
}

static int layout_from_name(const char *n) {
  if (!strcasecmp(n, "dwindle"))
    return LAYOUT_DWINDLE;
  if (!strcasecmp(n, "columns"))
    return LAYOUT_COLUMNS;
  if (!strcasecmp(n, "rows"))
    return LAYOUT_ROWS;
  if (!strcasecmp(n, "threecol"))
    return LAYOUT_THREECOL;
  if (!strcasecmp(n, "monocle"))
    return LAYOUT_MONOCLE;
  return -1;
}

/* Push a mods table from a Lua array of strings {"Mod4","Shift",...} */
static uint32_t mods_from_table(lua_State *L, int idx) {
  uint32_t mods = 0;
  if (!lua_istable(L, idx))
    return 0;
  int n = (int)lua_objlen(L, idx);
  for (int i = 1; i <= n; i++) {
    lua_rawgeti(L, idx, i);
    const char *m = lua_tostring(L, -1);
    if (m) {
      if (!strcasecmp(m, "Mod4") || !strcasecmp(m, "Super"))
        mods |= MOD_SUPER;
      else if (!strcasecmp(m, "Control") || !strcasecmp(m, "Ctrl"))
        mods |= MOD_CTRL;
      else if (!strcasecmp(m, "Mod1") || !strcasecmp(m, "Alt"))
        mods |= MOD_ALT;
      else if (!strcasecmp(m, "Shift"))
        mods |= MOD_SHIFT;
    }
    lua_pop(L, 1);
  }
  return mods;
}

/* ══════════════════════════════════════════════════════════════════════════
 * §2  Client userdata
 *
 * Pushed as a full userdata containing just the PaneId.
 * Properties are resolved live via twm_pane_by_id on every access.
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
  PaneId id;
} ClientUD;

static ClientUD *client_check(lua_State *L, int idx) {
  return (ClientUD *)luaL_checkudata(L, idx, MT_CLIENT);
}

/* Push a client userdata for pane id. If id == 0, pushes nil. */
static void push_client(lua_State *L, PaneId id) {
  if (!id) {
    lua_pushnil(L);
    return;
  }
  ClientUD *ud = (ClientUD *)lua_newuserdata(L, sizeof(ClientUD));
  ud->id = id;
  luaL_setmetatable(L, MT_CLIENT);
}

/* __index for client objects */
static int client_index(lua_State *L) {
  ClientUD *ud = client_check(L, 1);
  const char *k = luaL_checkstring(L, 2);
  TrixieServer *s = get_server(L);
  Pane *p = twm_pane_by_id(&s->twm, ud->id);
  if (!p) {
    lua_pushnil(L);
    return 1;
  }

  if (!strcmp(k, "id")) {
    lua_pushinteger(L, p->id);
    return 1;
  }
  if (!strcmp(k, "title")) {
    lua_pushstring(L, p->title);
    return 1;
  }
  if (!strcmp(k, "app_id")) {
    lua_pushstring(L, p->app_id);
    return 1;
  }
  if (!strcmp(k, "floating")) {
    lua_pushboolean(L, p->floating);
    return 1;
  }
  if (!strcmp(k, "fullscreen")) {
    lua_pushboolean(L, p->fullscreen);
    return 1;
  }
  if (!strcmp(k, "opacity")) {
    lua_pushnumber(L, p->rule_opacity > 0 ? p->rule_opacity : 1.f);
    return 1;
  }
  if (!strcmp(k, "border_width")) {
    lua_pushinteger(L, s->cfg.border_width);
    return 1;
  }
  if (!strcmp(k, "border_color")) {
    lua_pushinteger(L, (lua_Integer)color_to_argb(s->cfg.colors.active_border));
    return 1;
  }
  if (!strcmp(k, "workspace")) {
    for (int i = 0; i < s->twm.ws_count; i++)
      for (int j = 0; j < s->twm.workspaces[i].pane_count; j++)
        if (s->twm.workspaces[i].panes[j] == ud->id) {
          lua_pushinteger(L, i + 1);
          return 1;
        }
    lua_pushnil(L);
    return 1;
  }
  if (!strcmp(k, "rect")) {
    lua_createtable(L, 0, 4);
    lua_pushinteger(L, p->rect.x);
    lua_setfield(L, -2, "x");
    lua_pushinteger(L, p->rect.y);
    lua_setfield(L, -2, "y");
    lua_pushinteger(L, p->rect.w);
    lua_setfield(L, -2, "w");
    lua_pushinteger(L, p->rect.h);
    lua_setfield(L, -2, "h");
    return 1;
  }
  if (!strcmp(k, "focused")) {
    lua_pushboolean(L, twm_focused_id(&s->twm) == ud->id);
    return 1;
  }

  /* Methods — look up in metatable */
  luaL_getmetatable(L, MT_CLIENT);
  lua_getfield(L, -1, k);
  return 1;
}

/* __newindex for client objects — writable properties */
static int client_newindex(lua_State *L) {
  ClientUD *ud = client_check(L, 1);
  const char *k = luaL_checkstring(L, 2);
  TrixieServer *s = get_server(L);
  Pane *p = twm_pane_by_id(&s->twm, ud->id);
  if (!p)
    return 0;

  if (!strcmp(k, "floating")) {
    bool want = lua_toboolean(L, 3);
    if (p->floating != want) {
      twm_set_focused(&s->twm, ud->id);
      server_float_toggle(s);
    }
    return 0;
  }
  if (!strcmp(k, "fullscreen")) {
    p->fullscreen = lua_toboolean(L, 3);
    twm_reflow(&s->twm);
    server_sync_windows(s);
    server_request_redraw(s);
    return 0;
  }
  if (!strcmp(k, "opacity")) {
    p->rule_opacity = (float)luaL_checknumber(L, 3);
    server_sync_windows(s);
    return 0;
  }
  if (!strcmp(k, "border_width")) {
    s->cfg.border_width = s->twm.border_w = (int)luaL_checkinteger(L, 3);
    server_sync_windows(s);
    return 0;
  }
  if (!strcmp(k, "border_color")) {
    Color c = argb_to_color((uint32_t)luaL_checkinteger(L, 3));
    s->cfg.colors.active_border = c;
    server_request_redraw(s);
    return 0;
  }
  if (!strcmp(k, "workspace")) {
    int n = (int)luaL_checkinteger(L, 3);
    twm_set_focused(&s->twm, ud->id);
    twm_move_to_ws(&s->twm, n - 1);
    server_sync_windows(s);
    return 0;
  }
  return 0;
}

/* __tostring */
static int client_tostring(lua_State *L) {
  ClientUD *ud = client_check(L, 1);
  TrixieServer *s = get_server(L);
  Pane *p = twm_pane_by_id(&s->twm, ud->id);
  if (p)
    lua_pushfstring(L, "client(%d: %s)", (int)ud->id, p->title);
  else
    lua_pushfstring(L, "client(%d: <gone>)", (int)ud->id);
  return 1;
}

/* __eq */
static int client_eq(lua_State *L) {
  ClientUD *a = client_check(L, 1);
  ClientUD *b = client_check(L, 2);
  lua_pushboolean(L, a->id == b->id);
  return 1;
}

/* Methods */
static int client_kill(lua_State *L) {
  ClientUD *ud = client_check(L, 1);
  TrixieServer *s = get_server(L);
  TrixieView *v = view_from_pane(s, ud->id);
  if (v && v->xdg_toplevel)
    wlr_xdg_toplevel_send_close(v->xdg_toplevel);
  return 0;
}

static int client_focus(lua_State *L) {
  ClientUD *ud = client_check(L, 1);
  TrixieServer *s = get_server(L);
  twm_set_focused(&s->twm, ud->id);
  server_focus_pane(s, ud->id);
  server_request_redraw(s);
  return 0;
}

static int client_raise(lua_State *L) {
  ClientUD *ud = client_check(L, 1);
  TrixieServer *s = get_server(L);
  TrixieView *v = view_from_pane(s, ud->id);
  if (v)
    wlr_scene_node_raise_to_top(&v->scene_tree->node);
  return 0;
}

static int client_move_to_workspace(lua_State *L) {
  ClientUD *ud = client_check(L, 1);
  int n = (int)luaL_checkinteger(L, 2);
  TrixieServer *s = get_server(L);
  twm_set_focused(&s->twm, ud->id);
  twm_move_to_ws(&s->twm, n - 1);
  server_sync_windows(s);
  return 0;
}

static int client_connect_signal(lua_State *L) {
  ClientUD *ud = client_check(L, 1);
  const char *name = luaL_checkstring(L, 2);
  luaL_checktype(L, 3, LUA_TFUNCTION);
  TrixieServer *s = get_server(L);
  Pane *p = twm_pane_by_id(&s->twm, ud->id);
  if (!p)
    return 0;

  /* Ensure per-client signal table exists in registry */
  if (p->lua_signals_ref == LUA_NOREF) {
    lua_newtable(L);
    p->lua_signals_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  }
  lua_rawgeti(L, LUA_REGISTRYINDEX, p->lua_signals_ref);
  /* signals[name] = signals[name] or {} */
  lua_getfield(L, -1, name);
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, name);
  }
  int n = (int)lua_objlen(L, -1);
  lua_pushvalue(L, 3);
  lua_rawseti(L, -2, n + 1);
  lua_pop(L, 2);
  return 0;
}

static int client_disconnect_signal(lua_State *L) {
  ClientUD *ud = client_check(L, 1);
  const char *name = luaL_checkstring(L, 2);
  luaL_checktype(L, 3, LUA_TFUNCTION);
  TrixieServer *s = get_server(L);
  Pane *p = twm_pane_by_id(&s->twm, ud->id);
  if (!p || p->lua_signals_ref == LUA_NOREF)
    return 0;

  lua_rawgeti(L, LUA_REGISTRYINDEX, p->lua_signals_ref);
  lua_getfield(L, -1, name);
  if (!lua_istable(L, -1)) {
    lua_pop(L, 2);
    return 0;
  }
  /* Remove matching function */
  int n = (int)lua_objlen(L, -1);
  for (int i = 1; i <= n; i++) {
    lua_rawgeti(L, -1, i);
    if (lua_rawequal(L, -1, 3)) {
      /* shift table down */
      lua_pop(L, 1);
      for (int j = i; j < n; j++) {
        lua_rawgeti(L, -1, j + 1);
        lua_rawseti(L, -2, j);
      }
      lua_pushnil(L);
      lua_rawseti(L, -2, n);
      break;
    }
    lua_pop(L, 1);
  }
  lua_pop(L, 2);
  return 0;
}

static int client_emit_signal(lua_State *L) {
  ClientUD *ud = client_check(L, 1);
  const char *name = luaL_checkstring(L, 2);
  TrixieServer *s = get_server(L);
  Pane *p = twm_pane_by_id(&s->twm, ud->id);
  if (!p)
    return 0;
  int nargs = lua_gettop(L) - 2;
  /* push client as first arg if not already in args */
  push_client(L, ud->id);
  for (int i = 0; i < nargs; i++)
    lua_pushvalue(L, 3 + i);
  lua_signal_emit_obj(L, &p->lua_signals_ref, name, nargs + 1);
  return 0;
}

static const luaL_Reg client_methods[] = {
    {"kill", client_kill},
    {"focus", client_focus},
    {"raise", client_raise},
    {"move_to_workspace", client_move_to_workspace},
    {"connect_signal", client_connect_signal},
    {"disconnect_signal", client_disconnect_signal},
    {"emit_signal", client_emit_signal},
    {NULL, NULL}};

/* ══════════════════════════════════════════════════════════════════════════
 * §3  Screen userdata
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
  TrixieOutput *out;
} ScreenUD;

static ScreenUD *screen_check(lua_State *L, int idx) {
  return (ScreenUD *)luaL_checkudata(L, idx, MT_SCREEN);
}

static void push_screen(lua_State *L, TrixieOutput *o) {
  if (!o) {
    lua_pushnil(L);
    return;
  }
  ScreenUD *ud = (ScreenUD *)lua_newuserdata(L, sizeof(ScreenUD));
  ud->out = o;
  luaL_setmetatable(L, MT_SCREEN);
}

static int screen_index(lua_State *L) {
  ScreenUD *ud = screen_check(L, 1);
  const char *k = luaL_checkstring(L, 2);
  TrixieServer *s = get_server(L);
  TrixieOutput *o = ud->out;

  if (!strcmp(k, "geometry")) {
    lua_createtable(L, 0, 4);
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "x");
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "y");
    lua_pushinteger(L, o->logical_w);
    lua_setfield(L, -2, "w");
    lua_pushinteger(L, o->logical_h);
    lua_setfield(L, -2, "h");
    return 1;
  }
  if (!strcmp(k, "workarea")) {
    /* workarea excludes bar insets */
    Rect cr = s->twm.content_rect;
    lua_createtable(L, 0, 4);
    lua_pushinteger(L, cr.x);
    lua_setfield(L, -2, "x");
    lua_pushinteger(L, cr.y);
    lua_setfield(L, -2, "y");
    lua_pushinteger(L, cr.w);
    lua_setfield(L, -2, "w");
    lua_pushinteger(L, cr.h);
    lua_setfield(L, -2, "h");
    return 1;
  }
  if (!strcmp(k, "workspace")) {
    lua_pushinteger(L, s->twm.active_ws + 1);
    return 1;
  }
  if (!strcmp(k, "index")) {
    int idx = 1;
    TrixieOutput *it;
    wl_list_for_each(it, &s->outputs, link) {
      if (it == o)
        break;
      idx++;
    }
    lua_pushinteger(L, idx);
    return 1;
  }

  /* Methods */
  luaL_getmetatable(L, MT_SCREEN);
  lua_getfield(L, -1, k);
  return 1;
}

static int screen_newindex(lua_State *L) {
  ScreenUD *ud = screen_check(L, 1);
  const char *k = luaL_checkstring(L, 2);
  TrixieServer *s = get_server(L);
  (void)ud;

  if (!strcmp(k, "workspace")) {
    int n = (int)luaL_checkinteger(L, 3);
    int old = s->twm.active_ws;
    twm_switch_ws(&s->twm, n - 1);
    if (s->twm.active_ws != old)
      anim_workspace_transition(&s->anim, s->twm.active_ws > old ? WS_DIR_RIGHT
                                                                 : WS_DIR_LEFT);
    server_sync_focus(s);
    server_sync_windows(s);
    ipc_push_workspace_changed(s);
  }
  return 0;
}

static int screen_connect_signal(lua_State *L) {
  ScreenUD *ud = screen_check(L, 1);
  const char *name = luaL_checkstring(L, 2);
  luaL_checktype(L, 3, LUA_TFUNCTION);
  TrixieOutput *o = ud->out;
  if (o->lua_signals_ref == LUA_NOREF) {
    lua_newtable(L);
    o->lua_signals_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  }
  lua_rawgeti(L, LUA_REGISTRYINDEX, o->lua_signals_ref);
  lua_getfield(L, -1, name);
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, name);
  }
  int n = (int)lua_objlen(L, -1);
  lua_pushvalue(L, 3);
  lua_rawseti(L, -2, n + 1);
  lua_pop(L, 2);
  return 0;
}

static int screen_tostring(lua_State *L) {
  ScreenUD *ud = screen_check(L, 1);
  lua_pushfstring(L, "screen(%p)", (void *)ud->out);
  return 1;
}

static const luaL_Reg screen_methods[] = {
    {"connect_signal", screen_connect_signal}, {NULL, NULL}};

/* ══════════════════════════════════════════════════════════════════════════
 * §4  Canvas userdata
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
  Canvas *c;
} CanvasUD;

static CanvasUD *canvas_check(lua_State *L, int idx) {
  return (CanvasUD *)luaL_checkudata(L, idx, MT_CANVAS);
}

static int lcanvas_fill_rect(lua_State *L) {
  CanvasUD *ud = canvas_check(L, 1);
  int x = (int)luaL_checkinteger(L, 2);
  int y = (int)luaL_checkinteger(L, 3);
  int w = (int)luaL_checkinteger(L, 4);
  int h = (int)luaL_checkinteger(L, 5);
  Color col = argb_to_color((uint32_t)luaL_checkinteger(L, 6));
  canvas_fill_rect(ud->c, x, y, w, h, col);
  return 0;
}

static int lcanvas_draw_text(lua_State *L) {
  CanvasUD *ud = canvas_check(L, 1);
  int x = (int)luaL_checkinteger(L, 2);
  int y = (int)luaL_checkinteger(L, 3);
  const char *txt = luaL_checkstring(L, 4);
  Color col = argb_to_color((uint32_t)luaL_checkinteger(L, 5));
  int adv = canvas_draw_text(ud->c, x, y, txt, col);
  lua_pushinteger(L, adv);
  return 1;
}

static int lcanvas_clear(lua_State *L) {
  CanvasUD *ud = canvas_check(L, 1);
  Color col = argb_to_color((uint32_t)luaL_checkinteger(L, 2));
  canvas_clear(ud->c, col);
  return 0;
}

static int lcanvas_measure(lua_State *L) {
  canvas_check(L, 1);
  lua_pushinteger(L, canvas_measure(luaL_checkstring(L, 2)));
  return 1;
}

static int lcanvas_width(lua_State *L) {
  lua_pushinteger(L, canvas_check(L, 1)->c->w);
  return 1;
}
static int lcanvas_height(lua_State *L) {
  lua_pushinteger(L, canvas_check(L, 1)->c->h);
  return 1;
}
static int lcanvas_font_height(lua_State *L) {
  (void)canvas_check(L, 1);
  lua_pushinteger(L, canvas_font_height());
  return 1;
}
static int lcanvas_font_ascender(lua_State *L) {
  (void)canvas_check(L, 1);
  lua_pushinteger(L, canvas_font_ascender());
  return 1;
}

static const luaL_Reg canvas_methods[] = {
    {"fill_rect", lcanvas_fill_rect},
    {"draw_text", lcanvas_draw_text},
    {"clear", lcanvas_clear},
    {"measure", lcanvas_measure},
    {"width", lcanvas_width},
    {"height", lcanvas_height},
    {"font_height", lcanvas_font_height},
    {"font_ascender", lcanvas_font_ascender},
    {NULL, NULL}};

/* ══════════════════════════════════════════════════════════════════════════
 * §5  Wibox userdata
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
  TrixieWibox *wb;
} WiboxUD;

static WiboxUD *wibox_ud_check(lua_State *L, int idx) {
  return (WiboxUD *)luaL_checkudata(L, idx, MT_WIBOX);
}

static int lwibox_set_draw(lua_State *L) {
  WiboxUD *ud = wibox_ud_check(L, 1);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  if (!ud->wb)
    return 0;
  if (ud->wb->lua_draw_ref != LUA_NOREF)
    luaL_unref(L, LUA_REGISTRYINDEX, ud->wb->lua_draw_ref);
  lua_pushvalue(L, 2);
  ud->wb->lua_draw_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  /* Mark dirty immediately so the very first frame picks it up */
  ud->wb->dirty = true;
  TrixieServer *s = get_server(L);
  server_request_redraw(s);
  return 0;
}

static int lwibox_redraw(lua_State *L) {
  WiboxUD *ud = wibox_ud_check(L, 1);
  if (!ud->wb)
    return 0;
  wibox_mark_dirty(ud->wb);
  TrixieServer *s = get_server(L);
  server_request_redraw(s);
  return 0;
}

static int lwibox_index(lua_State *L) {
  WiboxUD *ud = wibox_ud_check(L, 1);
  if (!ud->wb) {
    lua_pushnil(L);
    return 1;
  } // ← add guard
  const char *k = luaL_checkstring(L, 2);
  if (!strcmp(k, "visible")) {
    lua_pushboolean(L, ud->wb->visible);
    return 1;
  }
  if (!strcmp(k, "x")) {
    lua_pushinteger(L, ud->wb->x);
    return 1;
  }
  if (!strcmp(k, "y")) {
    lua_pushinteger(L, ud->wb->y);
    return 1;
  }
  if (!strcmp(k, "width")) {
    lua_pushinteger(L, ud->wb->w);
    return 1;
  }
  if (!strcmp(k, "height")) {
    lua_pushinteger(L, ud->wb->h);
    return 1;
  }
  luaL_getmetatable(L, MT_WIBOX);
  lua_getfield(L, -1, k);
  return 1;
}

static int lwibox_newindex(lua_State *L) {
  WiboxUD *ud = wibox_ud_check(L, 1);
  if (!ud->wb)
    return 0; // ← add guard
  const char *k = luaL_checkstring(L, 2);
  if (!strcmp(k, "visible")) {
    ud->wb->visible = lua_toboolean(L, 3);
    wlr_scene_node_set_enabled(&ud->wb->scene_buf->node, ud->wb->visible);
  }
  return 0;
}

static int lwibox_gc(lua_State *L) {
  WiboxUD *ud = wibox_ud_check(L, 1);
  if (ud->wb) {
    ud->wb->lua_ud_wb_ptr = NULL;
    ud->wb->lua_draw_ref = LUA_NOREF;
    ud->wb->L = NULL;
    ud->wb = NULL;
  }
  return 0;
}

static const luaL_Reg wibox_methods[] = {
    {"set_draw", lwibox_set_draw}, {"redraw", lwibox_redraw}, {NULL, NULL}};

/* ══════════════════════════════════════════════════════════════════════════
 * §6  Timer userdata
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct LuaTimer {
  struct wl_event_source *src;
  lua_State *L;
  int fn_ref;
  int interval_ms;
  bool repeating;
  struct LuaTimer *next;
  struct LuaTimer **ud_ptr; /* ← NEW: points to the Lua userdata's pointer so
                           lua_reload can null it before freeing */
} LuaTimer;

static LuaTimer *g_timers = NULL;

static int timer_cb(void *data) {
  LuaTimer *t = data;
  lua_rawgeti(t->L, LUA_REGISTRYINDEX, t->fn_ref);
  if (lua_pcall(t->L, 0, 0, 0) != LUA_OK) {
    wlr_log(WLR_ERROR, "[lua] timer: %s", lua_tostring(t->L, -1));
    lua_pop(t->L, 1);
  }
  if (t->repeating)
    wl_event_source_timer_update(t->src, t->interval_ms);
  return 0;
}

static int ltimer_cancel(lua_State *L) {
  LuaTimer **ud = (LuaTimer **)luaL_checkudata(L, 1, MT_TIMER);
  if (ud && *ud) {
    LuaTimer *t = *ud;
    *ud = NULL;       /* null first — makes __gc a no-op if called again */
    t->ud_ptr = NULL; /* clear back-pointer */
    if (t->src)
      wl_event_source_remove(t->src);
    luaL_unref(L, LUA_REGISTRYINDEX, t->fn_ref);
    LuaTimer **pp = &g_timers;
    while (*pp) {
      if (*pp == t) {
        *pp = t->next;
        break;
      }
      pp = &(*pp)->next;
    }
    free(t);
  }
  return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * §7  trixie.* functions
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── Signals ───────────────────────────────────────────────────────────── */

static int l_connect_signal(lua_State *L) {
  const char *name = luaL_checkstring(L, 1);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  TrixieServer *s = get_server(L);
  lua_pushvalue(L, 2);
  lua_signal_connect(L, &s->signals, name);
  return 0;
}

static int l_disconnect_signal(lua_State *L) {
  const char *name = luaL_checkstring(L, 1);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  TrixieServer *s = get_server(L);
  /* find fn_ref by comparing function values */
  LuaSignal *sig = NULL;
  for (int i = 0; i < s->signals.count; i++)
    if (!strcmp(s->signals.signals[i].name, name)) {
      sig = &s->signals.signals[i];
      break;
    }
  if (!sig)
    return 0;
  for (int i = 0; i < MAX_SIGNAL_CBS; i++) {
    if (sig->fn_refs[i] == LUA_NOREF)
      continue;
    lua_rawgeti(L, LUA_REGISTRYINDEX, sig->fn_refs[i]);
    if (lua_rawequal(L, -1, 2)) {
      lua_pop(L, 1);
      lua_signal_disconnect(L, &s->signals, name, sig->fn_refs[i]);
      return 0;
    }
    lua_pop(L, 1);
  }
  return 0;
}

static int l_emit_signal(lua_State *L) {
  const char *name = luaL_checkstring(L, 1);
  TrixieServer *s = get_server(L);
  int nargs = lua_gettop(L) - 1;
  for (int i = 0; i < nargs; i++)
    lua_pushvalue(L, 2 + i);
  lua_signal_emit(L, &s->signals, name, nargs);
  return 0;
}

/* alias */
static int l_on(lua_State *L) { return l_connect_signal(L); }

/* ── Actions ───────────────────────────────────────────────────────────── */

static int l_spawn(lua_State *L) {
  server_spawn(get_server(L), luaL_checkstring(L, 1));
  return 0;
}

static int l_focus(lua_State *L) {
  const char *dir = luaL_checkstring(L, 1);
  TrixieServer *s = get_server(L);
  Action a = {0};
  if (!strcmp(dir, "left"))
    a.kind = ACTION_FOCUS_LEFT;
  else if (!strcmp(dir, "right"))
    a.kind = ACTION_FOCUS_RIGHT;
  else if (!strcmp(dir, "up"))
    a.kind = ACTION_FOCUS_UP;
  else if (!strcmp(dir, "down"))
    a.kind = ACTION_FOCUS_DOWN;
  else
    luaL_error(L, "focus: unknown direction '%s'", dir);
  server_dispatch_action(s, &a);
  return 0;
}

static int l_close(lua_State *L) {
  TrixieServer *s = get_server(L);
  Action a = {.kind = ACTION_CLOSE};
  server_dispatch_action(s, &a);
  return 0;
}
static int l_float(lua_State *L) {
  server_float_toggle(get_server(L));
  return 0;
}
static int l_fullscreen(lua_State *L) {
  TrixieServer *s = get_server(L);
  Action a = {.kind = ACTION_FULLSCREEN};
  server_dispatch_action(s, &a);
  return 0;
}
static int l_scratchpad(lua_State *L) {
  server_scratch_toggle(get_server(L), luaL_checkstring(L, 1));
  return 0;
}
static int l_grow_main(lua_State *L) {
  TrixieServer *s = get_server(L);
  Action a = {.kind = ACTION_GROW_MAIN};
  server_dispatch_action(s, &a);
  return 0;
}
static int l_shrink_main(lua_State *L) {
  TrixieServer *s = get_server(L);
  Action a = {.kind = ACTION_SHRINK_MAIN};
  server_dispatch_action(s, &a);
  return 0;
}
static int l_ratio(lua_State *L) {
  float r = (float)luaL_checknumber(L, 1);
  TrixieServer *s = get_server(L);
  if (r < 0.1f)
    r = 0.1f;
  if (r > 0.9f)
    r = 0.9f;
  s->twm.workspaces[s->twm.active_ws].main_ratio = r;
  twm_reflow(&s->twm);
  server_sync_windows(s);
  return 0;
}
static int l_swap(lua_State *L) {
  TrixieServer *s = get_server(L);
  twm_swap(&s->twm, true);
  server_sync_windows(s);
  return 0;
}
static int l_swap_back(lua_State *L) {
  TrixieServer *s = get_server(L);
  twm_swap(&s->twm, false);
  server_sync_windows(s);
  return 0;
}
static int l_swap_main(lua_State *L) {
  TrixieServer *s = get_server(L);
  twm_swap_main(&s->twm);
  server_sync_windows(s);
  return 0;
}
static int l_float_move(lua_State *L) {
  int dx = (int)luaL_checkinteger(L, 1), dy = (int)luaL_checkinteger(L, 2);
  TrixieServer *s = get_server(L);
  PaneId id = twm_focused_id(&s->twm);
  if (id) {
    twm_float_move(&s->twm, id, dx, dy);
    server_sync_windows(s);
  }
  return 0;
}
static int l_float_resize(lua_State *L) {
  int dw = (int)luaL_checkinteger(L, 1), dh = (int)luaL_checkinteger(L, 2);
  TrixieServer *s = get_server(L);
  PaneId id = twm_focused_id(&s->twm);
  if (id) {
    twm_float_resize(&s->twm, id, dw, dh);
    server_sync_windows(s);
  }
  return 0;
}
static int l_reload(lua_State *L) {
  TrixieServer *s = get_server(L);
  server_schedule_reload(s);
  return 0;
}

static int l_quit(lua_State *L) {
  TrixieServer *s = get_server(L);
  s->running = false;
  wl_display_terminate(s->display);
  return 0;
}

/* ── trixie.register_scratchpad(name, opts) ────────────────────────────────
 * opts: { app_id, exec, width, height }
 * Registers a named scratchpad so the C TWM can track it by app_id.
 * Lua's scratchpad.lua manages show/hide; this just tells the TWM the name. */
static int l_register_scratchpad(lua_State *L) {
  const char *name = luaL_checkstring(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);
  TrixieServer *s = get_server(L);
  char app_id[128] = {0}, exec[256] = {0};
  float wpct = 0.6f, hpct = 0.6f;
  lua_getfield(L, 2, "app_id");
  if (lua_isstring(L, -1))
    strncpy(app_id, lua_tostring(L, -1), sizeof(app_id) - 1);
  lua_pop(L, 1);
  lua_getfield(L, 2, "exec");
  if (lua_isstring(L, -1))
    strncpy(exec, lua_tostring(L, -1), sizeof(exec) - 1);
  lua_pop(L, 1);
  lua_getfield(L, 2, "width");
  if (lua_isnumber(L, -1)) {
    wpct = (float)lua_tonumber(L, -1);
    if (wpct > 1.f)
      wpct /= 100.f;
  }
  lua_pop(L, 1);
  lua_getfield(L, 2, "height");
  if (lua_isnumber(L, -1)) {
    hpct = (float)lua_tonumber(L, -1);
    if (hpct > 1.f)
      hpct /= 100.f;
  }
  lua_pop(L, 1);
  twm_register_scratch(&s->twm, name, app_id, exec, wpct, hpct);
  return 0;
}

/* ── trixie.get(key) → value ───────────────────────────────────────────────
 * Read current config values back from Lua.  Useful for bars and modules
 * that want to react to whatever the user set. */
static int l_get(lua_State *L) {
  const char *key = luaL_checkstring(L, 1);
  TrixieServer *s = get_server(L);
  Config *c = &s->cfg;
  if (!strcmp(key, "gap")) {
    lua_pushinteger(L, s->twm.gap);
    return 1;
  }
  if (!strcmp(key, "outer_gap")) {
    lua_pushinteger(L, c->outer_gap);
    return 1;
  }
  if (!strcmp(key, "border_width")) {
    lua_pushinteger(L, s->twm.border_w);
    return 1;
  }
  if (!strcmp(key, "smart_gaps")) {
    lua_pushboolean(L, s->twm.smart_gaps);
    return 1;
  }
  if (!strcmp(key, "workspaces")) {
    lua_pushinteger(L, s->twm.ws_count);
    return 1;
  }
  if (!strcmp(key, "font")) {
    lua_pushstring(L, c->font_path);
    return 1;
  }
  if (!strcmp(key, "font_size")) {
    lua_pushnumber(L, c->font_size);
    return 1;
  }
  if (!strcmp(key, "cursor_theme")) {
    lua_pushstring(L, c->cursor_theme);
    return 1;
  }
  if (!strcmp(key, "cursor_size")) {
    lua_pushinteger(L, c->cursor_size);
    return 1;
  }
  if (!strcmp(key, "kb_layout")) {
    lua_pushstring(L, c->kb_layout);
    return 1;
  }
  if (!strcmp(key, "repeat_rate")) {
    lua_pushinteger(L, c->repeat_rate);
    return 1;
  }
  if (!strcmp(key, "repeat_delay")) {
    lua_pushinteger(L, c->repeat_delay);
    return 1;
  }
  if (!strcmp(key, "active_workspace")) {
    lua_pushinteger(L, s->twm.active_ws + 1);
    return 1;
  }
  if (!strcmp(key, "screen_w")) {
    lua_pushinteger(L, s->twm.screen_w);
    return 1;
  }
  if (!strcmp(key, "screen_h")) {
    lua_pushinteger(L, s->twm.screen_h);
    return 1;
  }
  lua_pushnil(L);
  return 1;
}

/* ── trixie.notify(summary [, body]) ───────────────────────────────────────
 * Send a desktop notification via notify-send (best-effort, non-blocking). */
static int l_notify(lua_State *L) {
  const char *summary = luaL_checkstring(L, 1);
  const char *body = lua_isstring(L, 2) ? lua_tostring(L, 2) : NULL;
  TrixieServer *s = get_server(L);
  char cmd[1024];
  if (body)
    snprintf(cmd, sizeof(cmd), "notify-send -a trixie %s %s", summary, body);
  else
    snprintf(cmd, sizeof(cmd), "notify-send -a trixie %s", summary);
  server_spawn(s, cmd);
  return 0;
}

/* ── trixie.dpms(on|off) ───────────────────────────────────────────────────
 * Toggle display power from Lua, e.g. from an idle signal handler. */
static int l_dpms(lua_State *L) {
  bool on = true;
  if (lua_isboolean(L, 1))
    on = lua_toboolean(L, 1);
  else if (lua_isstring(L, 1))
    on = strcmp(lua_tostring(L, 1), "off") != 0;
  TrixieServer *s = get_server(L);
  TrixieOutput *o;
  wl_list_for_each(o, &s->outputs, link) {
    struct wlr_output_state st;
    wlr_output_state_init(&st);
    wlr_output_state_set_enabled(&st, on);
    wlr_output_commit_state(o->wlr_output, &st);
    wlr_output_state_finish(&st);
  }
  return 0;
}

/* ── trixie.inc_master() / dec_master() ────────────────────────────────────
 * Add/remove panes from the master column (columns/rows layouts). */
static int l_inc_master(lua_State *L) {
  TrixieServer *s = get_server(L);
  Workspace *ws = &s->twm.workspaces[s->twm.active_ws];
  /* master count lives in dwindle tree — expose via main_ratio bump as proxy
   * until a proper master count field is added to Workspace */
  ws->main_ratio += 0.05f;
  if (ws->main_ratio > 0.85f)
    ws->main_ratio = 0.85f;
  twm_reflow(&s->twm);
  server_sync_windows(s);
  return 0;
}
static int l_dec_master(lua_State *L) {
  TrixieServer *s = get_server(L);
  Workspace *ws = &s->twm.workspaces[s->twm.active_ws];
  ws->main_ratio -= 0.05f;
  if (ws->main_ratio < 0.15f)
    ws->main_ratio = 0.15f;
  twm_reflow(&s->twm);
  server_sync_windows(s);
  return 0;
}

/* ── Workspace ─────────────────────────────────────────────────────────── */

static int l_workspace(lua_State *L) {
  /* trixie.workspace(n) → switch */
  if (lua_isnumber(L, 1)) {
    int n = (int)lua_tointeger(L, 1);
    TrixieServer *s = get_server(L);
    int old = s->twm.active_ws;
    twm_switch_ws(&s->twm, n - 1);
    if (s->twm.active_ws != old)
      anim_workspace_transition(&s->anim, s->twm.active_ws > old ? WS_DIR_RIGHT
                                                                 : WS_DIR_LEFT);
    server_sync_focus(s);
    server_sync_windows(s);
    ipc_push_workspace_changed(s);
    lua_emit_workspace_changed(s);
  }
  return 0;
}

/* trixie.workspace table with .focused, .count fields */
static int ws_index(lua_State *L) {
  const char *k = lua_tostring(L, 2);
  TrixieServer *s = get_server(L);
  if (k) {
    if (!strcmp(k, "focused")) {
      lua_pushinteger(L, s->twm.active_ws + 1);
      return 1;
    }
    if (!strcmp(k, "count")) {
      lua_pushinteger(L, s->twm.ws_count);
      return 1;
    }
    if (!strcmp(k, "layout")) {
      Workspace *ws = &s->twm.workspaces[s->twm.active_ws];
      lua_pushinteger(L, (lua_Integer)ws->layout);
      return 1;
    }
  }
  /* integer index → switch */
  if (lua_isnumber(L, 2)) {
    int n = (int)lua_tointeger(L, 2);
    int old = s->twm.active_ws;
    twm_switch_ws(&s->twm, n - 1);
    if (s->twm.active_ws != old)
      anim_workspace_transition(&s->anim, s->twm.active_ws > old ? WS_DIR_RIGHT
                                                                 : WS_DIR_LEFT);
    server_sync_focus(s);
    server_sync_windows(s);
  }
  return 0;
}

/* ── Layout table ──────────────────────────────────────────────────────── */

static int l_layout_set(lua_State *L) {
  TrixieServer *s = get_server(L);
  int lay = -1;
  if (lua_isstring(L, 1))
    lay = layout_from_name(lua_tostring(L, 1));
  else if (lua_isnumber(L, 1))
    lay = (int)lua_tointeger(L, 1);
  if (lay < 0 || lay >= LAYOUT_COUNT)
    luaL_error(L, "unknown layout");
  s->twm.workspaces[s->twm.active_ws].layout = (Layout)lay;
  twm_reflow(&s->twm);
  server_sync_windows(s);
  return 0;
}
static int l_layout_next(lua_State *L) {
  TrixieServer *s = get_server(L);
  Action a = {.kind = ACTION_NEXT_LAYOUT};
  server_dispatch_action(s, &a);
  return 0;
}
static int l_layout_prev(lua_State *L) {
  TrixieServer *s = get_server(L);
  Action a = {.kind = ACTION_PREV_LAYOUT};
  server_dispatch_action(s, &a);
  return 0;
}

/* ── Screen table ──────────────────────────────────────────────────────── */

static int screen_table_index(lua_State *L) {
  TrixieServer *s = get_server(L);
  const char *k = lua_tostring(L, 2);
  if (k) {
    if (!strcmp(k, "focused")) {
      /* return first output for now — multi-monitor aware later */
      if (!wl_list_empty(&s->outputs)) {
        TrixieOutput *o = wl_container_of(s->outputs.next, o, link);
        push_screen(L, o);
        return 1;
      }
      lua_pushnil(L);
      return 1;
    }
    if (!strcmp(k, "count")) {
      int n = 0;
      TrixieOutput *o;
      wl_list_for_each(o, &s->outputs, link) n++;
      lua_pushinteger(L, n);
      return 1;
    }
  }
  if (lua_isnumber(L, 2)) {
    int want = (int)lua_tointeger(L, 2), idx = 1;
    TrixieOutput *o;
    wl_list_for_each(o, &s->outputs, link) {
      if (idx++ == want) {
        push_screen(L, o);
        return 1;
      }
    }
    lua_pushnil(L);
    return 1;
  }
  lua_pushnil(L);
  return 1;
}

/* ── Keybinds ──────────────────────────────────────────────────────────── */

static int l_key(lua_State *L) {
  /* trixie.key({ mods }, "key", fn [, fn_release]) */
  luaL_checktype(L, 1, LUA_TTABLE);
  const char *keystr = luaL_checkstring(L, 2);
  luaL_checktype(L, 3, LUA_TFUNCTION);

  TrixieServer *s = get_server(L);
  if (s->lua_bind_count >= MAX_KEYBINDS)
    luaL_error(L, "trixie.key: too many binds");

  uint32_t mods = mods_from_table(L, 1);
  xkb_keysym_t sym = xkb_keysym_from_name(keystr, XKB_KEYSYM_CASE_INSENSITIVE);
  if (sym == XKB_KEY_NoSymbol)
    luaL_error(L, "trixie.key: unknown key '%s'", keystr);

  /* replace existing bind */
  for (int i = 0; i < s->lua_bind_count; i++) {
    if (s->lua_binds[i].mods == mods && s->lua_binds[i].sym == sym) {
      luaL_unref(L, LUA_REGISTRYINDEX, s->lua_binds[i].fn_ref);
      lua_pushvalue(L, 3);
      s->lua_binds[i].fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);
      return 0;
    }
  }
  lua_pushvalue(L, 3);
  s->lua_binds[s->lua_bind_count].mods = mods;
  s->lua_binds[s->lua_bind_count].sym = sym;
  s->lua_binds[s->lua_bind_count].fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  s->lua_bind_count++;
  return 0;
}

/* ── Rules ─────────────────────────────────────────────────────────────── */

/* trixie.rules.rules = { { rule={class="..."}, properties={...} }, ... }
 * We process this table lazily — read it back in lua_apply_window_rules.
 * The table is stored at registry[REG_RULES]. */

static int rules_newindex(lua_State *L) {
  const char *k = lua_tostring(L, 2);
  if (k && !strcmp(k, "rules")) {
    luaL_checktype(L, 3, LUA_TTABLE);
    lua_pushvalue(L, 3);
    lua_setfield(L, LUA_REGISTRYINDEX, REG_RULES);
  }
  return 0;
}

/* ── Config ────────────────────────────────────────────────────────────── */

static int l_set(lua_State *L) {
  const char *key = luaL_checkstring(L, 1);
  TrixieServer *s = get_server(L);
  Config *c = &s->cfg;

  if (!strcmp(key, "gap")) {
    s->twm.gap = (int)luaL_checkinteger(L, 2);
    twm_reflow(&s->twm);
    server_sync_windows(s);
  } else if (!strcmp(key, "outer_gap")) {
    s->twm.padding = c->outer_gap = (int)luaL_checkinteger(L, 2);
    server_sync_windows(s);
  } else if (!strcmp(key, "border_width")) {
    c->border_width = s->twm.border_w = (int)luaL_checkinteger(L, 2);
    server_sync_windows(s);
  } else if (!strcmp(key, "smart_gaps")) {
    luaL_checktype(L, 2, LUA_TBOOLEAN);
    c->smart_gaps = s->twm.smart_gaps = lua_toboolean(L, 2);
    twm_reflow(&s->twm);
    server_sync_windows(s);
  } else if (!strcmp(key, "font") || !strcmp(key, "font_family")) {
    strncpy(c->font_path, luaL_checkstring(L, 2), sizeof(c->font_path) - 1);
    canvas_font_reload(c->font_path, c->font_path_bold, c->font_path_italic,
                       c->font_size);
    server_request_redraw(s);
  } else if (!strcmp(key, "font_size")) {
    c->font_size = (float)luaL_checknumber(L, 2);
    canvas_font_reload(c->font_path, c->font_path_bold, c->font_path_italic,
                       c->font_size);
    server_request_redraw(s);
  } else if (!strcmp(key, "theme")) {
    /* Inline theme presets — config_set_theme removed from config.c */
    const char *name = luaL_checkstring(L, 2);
    if (!strcasecmp(name, "catppuccin-mocha") || !strcasecmp(name, "mocha")) {
      c->colors.active_border = color_hex(0xcba6f7);
      c->colors.inactive_border = color_hex(0x313244);
      c->colors.background = color_hex(0x1e1e2e);
    } else if (!strcasecmp(name, "gruvbox") ||
               !strcasecmp(name, "gruvbox-dark")) {
      c->colors.active_border = color_hex(0xd79921);
      c->colors.inactive_border = color_hex(0x504945);
      c->colors.background = color_hex(0x1d2021);
    } else if (!strcasecmp(name, "nord")) {
      c->colors.active_border = color_hex(0x88c0d0);
      c->colors.inactive_border = color_hex(0x3b4252);
      c->colors.background = color_hex(0x2e3440);
    } else if (!strcasecmp(name, "tokyo-night") ||
               !strcasecmp(name, "tokyonight")) {
      c->colors.active_border = color_hex(0x7aa2f7);
      c->colors.inactive_border = color_hex(0x292e42);
      c->colors.background = color_hex(0x1a1b26);
    } else {
      wlr_log(WLR_ERROR, "[lua] trixie.set: unknown theme '%s'", name);
    }
    server_request_redraw(s);
  } else if (!strcmp(key, "active_border") || !strcmp(key, "border_color")) {
    c->colors.active_border =
        color_hex(parse_hex_color(luaL_checkstring(L, 2)));
    server_request_redraw(s);
  } else if (!strcmp(key, "inactive_border")) {
    c->colors.inactive_border =
        color_hex(parse_hex_color(luaL_checkstring(L, 2)));
    server_request_redraw(s);
  } else if (!strcmp(key, "background") || !strcmp(key, "background_color")) {
    Color bc = color_hex(parse_hex_color(luaL_checkstring(L, 2)));
    c->colors.background = bc;
    if (s->bg_rect) {
      float fc[4] = {bc.r / 255.f, bc.g / 255.f, bc.b / 255.f, bc.a / 255.f};
      wlr_scene_rect_set_color(s->bg_rect, fc);
    }
  } else if (!strcmp(key, "cursor_size")) {
    c->cursor_size = (int)luaL_checkinteger(L, 2);
    if (s->xcursor_mgr)
      wlr_xcursor_manager_load(s->xcursor_mgr, c->cursor_size);
  } else if (!strcmp(key, "kb_layout")) {
    strncpy(c->kb_layout, luaL_checkstring(L, 2), sizeof(c->kb_layout) - 1);
  } else if (!strcmp(key, "kb_variant")) {
    strncpy(c->kb_variant, luaL_checkstring(L, 2), sizeof(c->kb_variant) - 1);
  } else if (!strcmp(key, "kb_options")) {
    strncpy(c->kb_options, luaL_checkstring(L, 2), sizeof(c->kb_options) - 1);
  } else if (!strcmp(key, "repeat_rate")) {
    c->repeat_rate = (int)luaL_checkinteger(L, 2);
  } else if (!strcmp(key, "repeat_delay")) {
    c->repeat_delay = (int)luaL_checkinteger(L, 2);
  } else if (!strcmp(key, "xwayland")) {
    luaL_checktype(L, 2, LUA_TBOOLEAN);
    c->xwayland = lua_toboolean(L, 2);
  } else if (!strcmp(key, "workspaces")) {
    c->workspaces = (int)luaL_checkinteger(L, 2);
  } else if (!strcmp(key, "cursor_theme")) {
    strncpy(c->cursor_theme, luaL_checkstring(L, 2),
            sizeof(c->cursor_theme) - 1);
    if (s->xcursor_mgr)
      wlr_xcursor_manager_load(s->xcursor_mgr, c->cursor_size);
  } else {
    wlr_log(WLR_ERROR, "[lua] trixie.set: unknown key '%s'", key);
  }
  return 0;
}

/* ── Queries ───────────────────────────────────────────────────────────── */

static int l_focused(lua_State *L) {
  TrixieServer *s = get_server(L);
  push_client(L, twm_focused_id(&s->twm));
  return 1;
}

static int l_clients(lua_State *L) {
  TrixieServer *s = get_server(L);
  Workspace *ws = &s->twm.workspaces[s->twm.active_ws];
  lua_createtable(L, ws->pane_count, 0);
  for (int i = 0; i < ws->pane_count; i++) {
    push_client(L, ws->panes[i]);
    lua_rawseti(L, -2, i + 1);
  }
  return 1;
}

static int l_clients_all(lua_State *L) {
  TrixieServer *s = get_server(L);
  lua_createtable(L, s->twm.pane_count, 0);
  int idx = 1;
  for (int wi = 0; wi < s->twm.ws_count; wi++) {
    Workspace *ws = &s->twm.workspaces[wi];
    for (int j = 0; j < ws->pane_count; j++) {
      push_client(L, ws->panes[j]);
      lua_rawseti(L, -2, idx++);
    }
  }
  return 1;
}

/* ── Wibox constructor ─────────────────────────────────────────────────── */

static int l_wibox(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  TrixieServer *s = get_server(L);

  TrixieOutput *out = NULL;
  lua_getfield(L, 1, "screen");
  if (lua_isuserdata(L, -1)) {
    ScreenUD *sud = (ScreenUD *)luaL_testudata(L, -1, MT_SCREEN);
    if (sud)
      out = sud->out;
  }
  lua_pop(L, 1);
  if (!out && !wl_list_empty(&s->outputs))
    out = wl_container_of(s->outputs.next, out, link);
  if (!out)
    luaL_error(L, "trixie.wibox: no screen available");

  lua_getfield(L, 1, "width");
  int w = lua_isnumber(L, -1) ? (int)lua_tointeger(L, -1) : out->logical_w;
  lua_pop(L, 1);

  lua_getfield(L, 1, "height");
  int h = lua_isnumber(L, -1) ? (int)lua_tointeger(L, -1) : 24;
  lua_pop(L, 1);

  lua_getfield(L, 1, "x");
  int x = lua_isnumber(L, -1) ? (int)lua_tointeger(L, -1) : 0;
  lua_pop(L, 1);

  lua_getfield(L, 1, "y");
  int y_arg = lua_isnumber(L, -1) ? (int)lua_tointeger(L, -1) : -1;
  lua_pop(L, 1);

  lua_getfield(L, 1, "position");
  const char *pos_str = lua_isstring(L, -1) ? lua_tostring(L, -1) : "bottom";
  BarPos pos = (!strcmp(pos_str, "top")) ? BAR_TOP : BAR_BOTTOM;
  lua_pop(L, 1);

  if (y_arg < 0)
    y_arg = (pos == BAR_TOP) ? 0 : out->logical_h - h;

  /* ── Destroy any existing wibox at this position on this output.
   * On reload, lua_reload calls wibox_reset_output (keeps scene nodes)
   * then rewrap_wiboxes, then init.lua calls trixie.wibox() again.
   * Without this, wiboxes accumulate and the slot fills up. */
  for (int wi = 0; wi < out->wibox_count; wi++) {
    TrixieWibox *old = out->wiboxes[wi];
    if (!old)
      continue;
    if (old->position == pos) {
      /* Null the Lua userdata back-pointer so the old WiboxUD __gc is a no-op
       */
      if (old->lua_ud_wb_ptr)
        *old->lua_ud_wb_ptr = NULL;
      wibox_destroy(old);
      /* Compact the slot */
      out->wiboxes[wi] = out->wiboxes[--out->wibox_count];
      out->wiboxes[out->wibox_count] = NULL;
      break;
    }
  }

  TrixieWibox *wb = wibox_create(s, out, x, y_arg, w, h);
  if (!wb)
    luaL_error(L, "trixie.wibox: failed to create surface");
  wb->position = pos;
  wb->lua_draw_ref = LUA_NOREF;

  if (out->wibox_count < MAX_WIBOXES)
    out->wiboxes[out->wibox_count++] = wb;

  WiboxUD *ud = (WiboxUD *)lua_newuserdata(L, sizeof(WiboxUD));
  ud->wb = wb;
  wb->lua_ud_wb_ptr = (void **)&ud->wb;
  luaL_setmetatable(L, MT_WIBOX);

  /* ── Re-register bar inset so twm content_rect excludes the bar.
   * Previously this only happened via layer-shell's exclusive zone;
   * wibox bars need to do it explicitly. */
  server_set_bar_inset(s, h, pos == BAR_BOTTOM);

  return 1;
}

/* ── Timer ─────────────────────────────────────────────────────────────── */

static int l_timer(lua_State *L) {
  int interval_ms = (int)luaL_checkinteger(L, 1);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  bool repeating = lua_isboolean(L, 3) ? lua_toboolean(L, 3) : true;
  TrixieServer *s = get_server(L);

  LuaTimer *t = calloc(1, sizeof(*t));
  t->L = L;
  lua_pushvalue(L, 2);
  t->fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  t->interval_ms = interval_ms;
  t->repeating = repeating;

  struct wl_event_loop *el = wl_display_get_event_loop(s->display);
  t->src = wl_event_loop_add_timer(el, timer_cb, t);
  wl_event_source_timer_update(t->src, interval_ms);

  t->next = g_timers;
  g_timers = t;

  LuaTimer **ud = (LuaTimer **)lua_newuserdata(L, sizeof(LuaTimer *));
  *ud = t;
  t->ud_ptr = ud; /* ← NEW: store back-pointer */

  luaL_newmetatable(L, MT_TIMER);
  lua_newtable(L);
  lua_pushcfunction(L, ltimer_cancel);
  lua_setfield(L, -2, "cancel");
  lua_setfield(L, -2, "__index");
  lua_pushcfunction(L, ltimer_cancel);
  lua_setfield(L, -2, "__gc");
  lua_setmetatable(L, -2);
  return 1;
}

/* ── Logging ───────────────────────────────────────────────────────────── */

static int l_log(lua_State *L) {
  wlr_log(WLR_INFO, "[lua] %s", luaL_checkstring(L, 1));
  return 0;
}
static int l_warn(lua_State *L) {
  wlr_log(WLR_ERROR, "[lua] WARN: %s", luaL_checkstring(L, 1));
  return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * §8  Module registration
 * ══════════════════════════════════════════════════════════════════════════ */

static const luaL_Reg trixie_lib[] = {
    /* Signals */
    {"connect_signal", l_connect_signal},
    {"disconnect_signal", l_disconnect_signal},
    {"emit_signal", l_emit_signal},
    {"on", l_on},
    /* Actions */
    {"spawn", l_spawn},
    {"focus", l_focus},
    {"close", l_close},
    {"float", l_float},
    {"fullscreen", l_fullscreen},
    {"scratchpad", l_scratchpad},
    {"grow_main", l_grow_main},
    {"shrink_main", l_shrink_main},
    {"inc_master", l_inc_master},
    {"dec_master", l_dec_master},
    {"ratio", l_ratio},
    {"swap", l_swap},
    {"swap_back", l_swap_back},
    {"swap_main", l_swap_main},
    {"float_move", l_float_move},
    {"float_resize", l_float_resize},
    {"reload", l_reload},
    {"quit", l_quit},
    {"dpms", l_dpms},
    {"notify", l_notify},
    /* Scratchpad registration */
    {"register_scratchpad", l_register_scratchpad},
    /* Keybinds */
    {"key", l_key},
    /* Config */
    {"set", l_set},
    {"get", l_get},
    /* Queries */
    {"focused", l_focused},
    {"clients", l_clients},
    {"clients_all", l_clients_all},
    /* Constructor */
    {"wibox", l_wibox},
    {"timer", l_timer},
    /* Logging */
    {"log", l_log},
    {"warn", l_warn},
    {NULL, NULL}};

static int luaopen_trixie(lua_State *L) {
  luaL_newlib(L, trixie_lib);

  /* trixie.version */
  lua_pushstring(L, TRIXIE_VERSION_STR);
  lua_setfield(L, -2, "version");

  /* trixie.workspace — callable table */
  lua_createtable(L, 0, 2);
  {
    lua_createtable(L, 0, 2); /* metatable */
    lua_pushcfunction(L, l_workspace);
    lua_setfield(L, -2, "__call");
    lua_pushcfunction(L, ws_index);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
  }
  lua_setfield(L, -2, "workspace");

  /* trixie.layout — table with named constants + helpers */
  lua_createtable(L, 0, 8);
  lua_pushinteger(L, LAYOUT_DWINDLE);
  lua_setfield(L, -2, "dwindle");
  lua_pushinteger(L, LAYOUT_COLUMNS);
  lua_setfield(L, -2, "columns");
  lua_pushinteger(L, LAYOUT_ROWS);
  lua_setfield(L, -2, "rows");
  lua_pushinteger(L, LAYOUT_THREECOL);
  lua_setfield(L, -2, "threecol");
  lua_pushinteger(L, LAYOUT_MONOCLE);
  lua_setfield(L, -2, "monocle");
  lua_pushcfunction(L, l_layout_set);
  lua_setfield(L, -2, "set");
  lua_pushcfunction(L, l_layout_next);
  lua_setfield(L, -2, "next");
  lua_pushcfunction(L, l_layout_prev);
  lua_setfield(L, -2, "prev");
  lua_setfield(L, -2, "layout");

  /* trixie.screen — table with __index for .focused / .count / [n] */
  lua_createtable(L, 0, 0);
  {
    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, screen_table_index);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
  }
  lua_setfield(L, -2, "screen");

  /* trixie.rules — table with .rules property */
  lua_createtable(L, 0, 0);
  {
    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, rules_newindex);
    lua_setfield(L, -2, "__newindex");
    lua_setmetatable(L, -2);
  }
  lua_setfield(L, -2, "rules");

  return 1;
}

/* ══════════════════════════════════════════════════════════════════════════
 * §9  Metatables setup
 * ══════════════════════════════════════════════════════════════════════════ */

static void register_metatables(lua_State *L) {
  /* Client */
  luaL_newmetatable(L, MT_CLIENT);
  lua_pushcfunction(L, client_index);
  lua_setfield(L, -2, "__index");
  lua_pushcfunction(L, client_newindex);
  lua_setfield(L, -2, "__newindex");
  lua_pushcfunction(L, client_tostring);
  lua_setfield(L, -2, "__tostring");
  lua_pushcfunction(L, client_eq);
  lua_setfield(L, -2, "__eq");
  /* Embed methods directly into the metatable __index fallback */
  for (int i = 0; client_methods[i].name; i++) {
    lua_pushcfunction(L, client_methods[i].func);
    lua_setfield(L, -2, client_methods[i].name);
  }
  lua_pop(L, 1);

  /* Screen */
  luaL_newmetatable(L, MT_SCREEN);
  lua_pushcfunction(L, screen_index);
  lua_setfield(L, -2, "__index");
  lua_pushcfunction(L, screen_newindex);
  lua_setfield(L, -2, "__newindex");
  lua_pushcfunction(L, screen_tostring);
  lua_setfield(L, -2, "__tostring");
  for (int i = 0; screen_methods[i].name; i++) {
    lua_pushcfunction(L, screen_methods[i].func);
    lua_setfield(L, -2, screen_methods[i].name);
  }
  lua_pop(L, 1);

  /* Canvas */
  luaL_newmetatable(L, MT_CANVAS);
  lua_newtable(L);
  luaL_setfuncs(L, canvas_methods, 0);
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);

  /* Wibox */
  luaL_newmetatable(L, MT_WIBOX);
  lua_pushcfunction(L, lwibox_index);
  lua_setfield(L, -2, "__index");
  lua_pushcfunction(L, lwibox_newindex);
  lua_setfield(L, -2, "__newindex");
  lua_pushcfunction(L, lwibox_gc);
  lua_setfield(L, -2, "__gc");
  for (int i = 0; wibox_methods[i].name; i++) {
    lua_pushcfunction(L, wibox_methods[i].func);
    lua_setfield(L, -2, wibox_methods[i].name);
  }
  lua_pop(L, 1);
}

/* ══════════════════════════════════════════════════════════════════════════
 * §10  Signal emission — called from main.c / view handlers
 * ══════════════════════════════════════════════════════════════════════════ */

void lua_emit_manage(TrixieServer *s, PaneId id) {
  if (!s->L)
    return;
  push_client(s->L, id);
  lua_signal_emit(s->L, &s->signals, "manage", 1);
  /* also fire per-client "manage" if anyone connected early */
  Pane *p = twm_pane_by_id(&s->twm, id);
  if (p) {
    push_client(s->L, id);
    lua_signal_emit_obj(s->L, &p->lua_signals_ref, "manage", 1);
  }
}

void lua_emit_unmanage(TrixieServer *s, PaneId id) {
  if (!s->L)
    return;
  push_client(s->L, id);
  lua_signal_emit(s->L, &s->signals, "unmanage", 1);
}

void lua_emit_focus(TrixieServer *s) {
  if (!s->L)
    return;
  PaneId id = twm_focused_id(&s->twm);
  push_client(s->L, id);
  lua_signal_emit(s->L, &s->signals, "focus", 1);
  if (id) {
    Pane *p = twm_pane_by_id(&s->twm, id);
    if (p) {
      push_client(s->L, id);
      lua_signal_emit_obj(s->L, &p->lua_signals_ref, "focus", 1);
    }
  }
}

void lua_emit_workspace_changed(TrixieServer *s) {
  if (!s->L)
    return;
  lua_pushinteger(s->L, s->twm.active_ws + 1);
  lua_pushinteger(s->L, s->twm.workspaces[s->twm.active_ws].pane_count);
  lua_signal_emit(s->L, &s->signals, "workspace_changed", 2);
}

void lua_emit_title_changed(TrixieServer *s, PaneId id) {
  if (!s->L)
    return;
  push_client(s->L, id);
  lua_signal_emit(s->L, &s->signals, "title_changed", 1);
  Pane *p = twm_pane_by_id(&s->twm, id);
  if (p) {
    push_client(s->L, id);
    lua_signal_emit_obj(s->L, &p->lua_signals_ref, "property::title", 1);
  }
}

void lua_emit_property(TrixieServer *s, PaneId id, const char *prop) {
  if (!s->L)
    return;
  Pane *p = twm_pane_by_id(&s->twm, id);
  if (!p)
    return;
  push_client(s->L, id);
  char sig[128];
  snprintf(sig, sizeof(sig), "property::%s", prop);
  lua_signal_emit_obj(s->L, &p->lua_signals_ref, sig, 1);
}

void lua_emit_screen_added(TrixieServer *s, TrixieOutput *o) {
  if (!s->L)
    return;
  o->lua_signals_ref = LUA_NOREF;
  push_screen(s->L, o);
  lua_signal_emit(s->L, &s->signals, "screen_added", 1);
}

void lua_emit_screen_removed(TrixieServer *s, TrixieOutput *o) {
  if (!s->L)
    return;
  push_screen(s->L, o);
  lua_signal_emit(s->L, &s->signals, "screen_removed", 1);
}

void lua_emit_startup(TrixieServer *s) {
  if (!s->L)
    return;
  lua_signal_emit(s->L, &s->signals, "startup", 0);
}

/* ══════════════════════════════════════════════════════════════════════════
 * §11  Window rules (applied at manage time)
 * ══════════════════════════════════════════════════════════════════════════ */

void lua_apply_window_rules(TrixieServer *s, Pane *p, const char *app_id,
                            const char *title) {
  lua_State *L = s->L;
  if (!L)
    return;

  /* Rules from trixie.rules.rules (set in init.lua) */
  lua_getfield(L, LUA_REGISTRYINDEX, REG_RULES);
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return;
  }
  int n = (int)lua_objlen(L, -1);
  for (int i = 1; i <= n; i++) {
    lua_rawgeti(L, -1, i);
    if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      continue;
    }

    lua_getfield(L, -1, "rule");
    bool match = true;
    if (lua_istable(L, -1)) {
      lua_getfield(L, -1, "class");
      if (lua_isstring(L, -1) && !strstr(app_id, lua_tostring(L, -1)))
        match = false;
      lua_pop(L, 1);
      lua_getfield(L, -1, "name");
      if (lua_isstring(L, -1) && !strstr(title, lua_tostring(L, -1)))
        match = false;
      lua_pop(L, 1);
      lua_getfield(L, -1, "app_id");
      if (lua_isstring(L, -1) && !strstr(app_id, lua_tostring(L, -1)))
        match = false;
      lua_pop(L, 1);
    }
    lua_pop(L, 1); /* rule */

    if (match) {
      lua_getfield(L, -1, "properties");
      if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "floating");
        if (lua_isboolean(L, -1))
          p->floating = lua_toboolean(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "fullscreen");
        if (lua_isboolean(L, -1))
          p->fullscreen = lua_toboolean(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "workspace");
        if (lua_isnumber(L, -1))
          twm_move_to_ws(&s->twm, (int)lua_tointeger(L, -1) - 1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "opacity");
        if (lua_isnumber(L, -1))
          p->rule_opacity = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "border_width");
        if (lua_isnumber(L, -1))
          s->cfg.border_width = s->twm.border_w = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "callback");
        if (lua_isfunction(L, -1)) {
          push_client(L, p->id);
          if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            wlr_log(WLR_ERROR, "[lua] rule callback: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
          }
        } else
          lua_pop(L, 1);
      }
      lua_pop(L, 1); /* properties */
    }
    lua_pop(L, 1); /* rule entry */
  }
  lua_pop(L, 1); /* rules table */
}

/* ══════════════════════════════════════════════════════════════════════════
 * §12  Key dispatch
 * ══════════════════════════════════════════════════════════════════════════ */

bool lua_dispatch_key(TrixieServer *s, uint32_t mods, xkb_keysym_t sym) {
  if (!s->L || !s->lua_bind_count)
    return false;
  for (int i = 0; i < s->lua_bind_count; i++) {
    if (s->lua_binds[i].mods != mods || s->lua_binds[i].sym != sym)
      continue;
    lua_rawgeti(s->L, LUA_REGISTRYINDEX, s->lua_binds[i].fn_ref);
    if (lua_pcall(s->L, 0, 0, 0) != LUA_OK) {
      wlr_log(WLR_ERROR, "[lua] bind: %s", lua_tostring(s->L, -1));
      lua_pop(s->L, 1);
    }
    return true;
  }
  return false;
}

/* ══════════════════════════════════════════════════════════════════════════
 * §13  Init / reload / destroy
 * ══════════════════════════════════════════════════════════════════════════ */

void lua_init(TrixieServer *s) {
  lua_State *L = luaL_newstate();
  if (!L) {
    wlr_log(WLR_ERROR, "[lua] failed to create state");
    return;
  }
  luaL_openlibs(L);
  s->L = L;

  lua_signal_init(&s->signals);
  lua_register_reload(s);

  /* Store server pointer */
  lua_pushstring(L, REG_SERVER);
  lua_pushlightuserdata(L, s);
  lua_rawset(L, LUA_REGISTRYINDEX);

  /* Empty rules table */
  lua_newtable(L);
  lua_setfield(L, LUA_REGISTRYINDEX, REG_RULES);

  register_metatables(L);

  /* Register trixie global */
  luaopen_trixie(L);
  lua_setglobal(L, "trixie");

  lua_reload(s);
}

static void rewrap_wiboxes(TrixieServer *s) {
  lua_State *L = s->L;

  /* _trixie_wiboxes = {} */
  lua_newtable(L);

  TrixieOutput *o;
  int oi = 1;
  wl_list_for_each(o, &s->outputs, link) {
    lua_newtable(L); /* per-output array */

    for (int wi = 0; wi < o->wibox_count; wi++) {
      TrixieWibox *wb = o->wiboxes[wi];
      if (!wb)
        continue;

      WiboxUD *ud = (WiboxUD *)lua_newuserdata(L, sizeof(WiboxUD));
      ud->wb = wb;
      wb->lua_ud_wb_ptr = (void **)&ud->wb;
      wb->L = L;
      luaL_setmetatable(L, MT_WIBOX);
      lua_rawseti(L, -2, wi + 1);
    }

    lua_rawseti(L, -2, oi++);
  }

  lua_setglobal(L, "_trixie_wiboxes");
}

void lua_reload(TrixieServer *s) {
  lua_State *L = s->L;
  if (!L)
    return;

  /* ── Step 0: reset wibox callbacks (keeps scene nodes alive) ─────────
   * wibox_reset_output unrefs draw callbacks and nulls back-pointers
   * WITHOUT destroying scene nodes, so wlr_scene_node_destroy is never
   * called from inside the event dispatch stack. */
  {
    TrixieOutput *o;
    wl_list_for_each(o, &s->outputs, link) wibox_reset_output(o, L);
  }

  /* ── Step 1: cancel timers, null userdata back-pointers ──────────────── */
  {
    LuaTimer *t = g_timers;
    while (t) {
      LuaTimer *nx = t->next;
      if (t->ud_ptr) {
        *t->ud_ptr = NULL;
        t->ud_ptr = NULL;
      }
      if (t->src)
        wl_event_source_remove(t->src);
      luaL_unref(L, LUA_REGISTRYINDEX, t->fn_ref);
      free(t);
      t = nx;
    }
    g_timers = NULL;
  }

  /* ── Step 2: clear keybinds ──────────────────────────────────────────── */
  for (int i = 0; i < s->lua_bind_count; i++)
    luaL_unref(L, LUA_REGISTRYINDEX, s->lua_binds[i].fn_ref);
  s->lua_bind_count = 0;

  /* ── Step 3: reset signals and rules ─────────────────────────────────── */
  lua_signal_init(&s->signals);
  for (int i = 0; i < s->twm.pane_count; i++) {
    Pane *p = &s->twm.panes[i];
    if (p->lua_signals_ref != LUA_NOREF) {
      luaL_unref(s->L, LUA_REGISTRYINDEX, p->lua_signals_ref);
      p->lua_signals_ref = LUA_NOREF;
    }
  }
  lua_newtable(L);
  lua_setfield(L, LUA_REGISTRYINDEX, REG_RULES);

  /* ── Step 4: re-wrap surviving wiboxes as fresh Lua userdata ──────────── */
  rewrap_wiboxes(s);

  /* ── Step 5: locate and run init.lua ─────────────────────────────────── */
  char path[512];
  const char *xdg = getenv("XDG_CONFIG_HOME");
  if (xdg)
    snprintf(path, sizeof(path), "%s/trixie/init.lua", xdg);
  else {
    const char *home = getenv("HOME");
    if (!home)
      home = "/root";
    snprintf(path, sizeof(path), "%s/.config/trixie/init.lua", home);
  }
  if (access(path, R_OK) != 0) {
    wlr_log(WLR_INFO, "[lua] no init.lua at %s", path);
    return;
  }
  // --- inject config dir into Lua package.path ---
  lua_getglobal(L, "package");
  lua_getfield(L, -1, "path");

  const char *old_path = lua_tostring(L, -1);

  // extract directory from path (strip "init.lua")
  char config_dir[512];
  strncpy(config_dir, path, sizeof(config_dir) - 1);
  char *last_slash = strrchr(config_dir, '/');
  if (last_slash)
    *last_slash = '\0';

  char new_path[1024];
  snprintf(new_path, sizeof(new_path), "%s/?.lua;%s/?/init.lua;%s", config_dir,
           config_dir, old_path);

  // set new path
  lua_pop(L, 1);
  lua_pushstring(L, new_path);
  lua_setfield(L, -2, "path");
  lua_pop(L, 1);
  if (luaL_loadfile(L, path) != 0 || lua_pcall(L, 0, 0, 0) != 0) {
    wlr_log(WLR_ERROR, "[lua] %s: %s", path, lua_tostring(L, -1));
    lua_pop(L, 1);
    return;
  }
  wlr_log(WLR_INFO, "[lua] loaded %s (%d binds)", path, s->lua_bind_count);
}

void lua_destroy(TrixieServer *s) {
  if (!s->L)
    return;
  lua_State *L = s->L;
  for (int i = 0; i < s->lua_bind_count; i++)
    luaL_unref(L, LUA_REGISTRYINDEX, s->lua_binds[i].fn_ref);
  LuaTimer *t = g_timers;
  while (t) {
    LuaTimer *nx = t->next;
    if (t->src)
      wl_event_source_remove(t->src);
    luaL_unref(L, LUA_REGISTRYINDEX, t->fn_ref);
    free(t);
    t = nx;
  }
  g_timers = NULL;
  lua_close(L);
  s->L = NULL;
}
