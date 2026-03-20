/* lua.c — Lua API for Trixie (AwesomeWM-style, Wayland)
 *
 * ── SIGNALS
 *   trixie.connect_signal(name, fn)   trixie.on(name, fn)
 *   trixie.disconnect_signal(name, fn)
 *   trixie.emit_signal(name, ...)
 *
 * ── CLIENT OBJECT
 *   c.id, c.title, c.app_id, c.workspace      (read)
 *   c.floating, c.fullscreen, c.ontop         (read/write → fires property::*)
 *   c.minimized, c.sticky                     (read/write → fires property::*)
 *   c.x, c.y, c.width, c.height               (read/write → auto-floats)
 *   c.border_width, c.border_color            (read/write)
 *   c.opacity                                 (read/write)
 *   c.focused                                 (read)
 *   c.rect → {x,y,w,h}                        (read)
 *   c:kill()  c:focus()  c:raise()
 *   c:geometry() → {x,y,width,height}
 *   c:geometry({x,y,width,height})            (write — auto-floats)
 *   c:minimize()  c:unminimize()
 *   c:move_to_workspace(n)
 *   c:connect_signal(name, fn)
 *   c:emit_signal(name, ...)
 *
 * ── SIGNALS FIRED ON CLIENT PROPERTY WRITES
 *   property::floating, property::fullscreen, property::ontop
 *   property::minimized, property::sticky, property::opacity
 *   property::border_width, property::border_color
 *   property::workspace, property::geometry
 *   property::title (fired by compositor on title change)
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
 * ── INPUT BINDINGS
 *   trixie.key({mods}, key, fn [, fn_release])
 *   trixie.button({mods}, btn, fn [, fn_release])   btn=1/2/3
 *   trixie.gesture(spec, fn)
 *     spec: "swipe:3:left" | "swipe:3:right" | "swipe:3:up" | "swipe:3:down"
 *           "swipe:4:left" etc. | "pinch:2:in" | "pinch:2:out"
 *
 * ── RULES
 *   trixie.rules.rules = { {rule={app_id=,name=}, properties={...}}, ... }
 *   properties: floating, fullscreen, workspace, opacity, border_width,
 * callback
 *
 * ── LAYOUTS
 *   trixie.layout.dwindle / columns / rows / threecol / monocle
 *   trixie.layout.set(layout)   trixie.layout.next()   trixie.layout.prev()
 *   trixie.layout.register(name, fn)
 *     fn(clients, geometry) called at reflow
 *     clients = array of client objects for the workspace
 *     geometry = {x, y, width, height, gap}
 *     fn writes c.x/y/width/height on each client to position it
 *
 * ── WIBOX
 *   local wb = trixie.wibox({screen, x, y, width, height, position})
 *   wb:set_draw(function(canvas) ... end)  wb:redraw()  wb.visible
 *   canvas:fill_rect(x,y,w,h, argb)
 *   canvas:draw_text(x,y,text, argb) → width
 *   canvas:clear(argb)
 *   canvas:measure(text) → int
 *   canvas:width()   canvas:height()
 *   canvas:font_height()   canvas:font_ascender()
 *
 * ── WIDGET SYSTEM
 *   Composable primitives for use inside any wibox set_draw callback.
 *   All constructors take an opts table.  Widgets are full GC'd userdata.
 *
 *   trixie.widget.text({ text, text_fn, fg, align, padding_x, padding_y })
 *     text_fn  – function() → string  (called each draw; overrides text)
 *     fg       – 0xAARRGGBB colour (default 0xFFCDD6F4)
 *     align    – "left"|"center"|"right"  (default "left")
 *
 *   trixie.widget.image({ pixels_fn, src_w, src_h, width, height, padding })
 *     pixels_fn – function() → {pixels=<string>,w=N,h=N}
 *
 *   trixie.widget.progress({ value, value_fn, fg, bg, border, padding })
 *     value    – 0.0..1.0 static, or supply value_fn
 *     border   – optional colour; if set draws a 1-px frame
 *
 *   trixie.widget.separator({ thickness, color, padding })
 *
 *   trixie.widget.spacer()
 *     Zero-desired-width; flex-fills remaining horizontal/vertical space.
 *
 *   trixie.widget.container({ children, layout, spacing, padding,
 *                              bg, width, height })
 *     layout   – "horizontal" (default) | "vertical" | "fixed"
 *     children – array of widget userdata (or callables for custom draws)
 *     spacer() inside children causes flex distribution of leftover space.
 *
 *   Widget methods (on every widget type):
 *     w:measure(canvas)           → desired_w, desired_h
 *     w:draw(canvas, x, y, w, h)  → (side-effects only)
 *     w:set(key, value)           → mutate a field without recreating
 *
 * ── ACTIONS
 *   trixie.spawn(cmd)
 *   trixie.exec_once(cmd)                    (deduped across reloads)
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
 * ── SCRATCHPAD
 *   trixie.register_scratchpad(name, {app_id, exec, width, height})
 *
 * ── CONFIG
 *   trixie.set(key, value)
 *     gap, outer_gap, border_width, border_color, active_border,
 *     inactive_border, background, font, font_size, smart_gaps,
 *     cursor_size, cursor_theme, kb_layout, kb_variant, kb_options,
 *     repeat_rate, repeat_delay, workspaces, xwayland, saturation,
 *     idle_timeout, theme,
 *     monitor = {name, width, height, refresh, scale, x, y}
 *   trixie.get(key) → value
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
 *   "startup"                on first start
 *   "reload"                 after hot-reload completes
 *   "manage"       (c)       new window mapped
 *   "unmanage"     (c)       window closed
 *   "focus"        (c)       focus changed
 *   "title_changed"(c)       window title updated
 *   "workspace_changed" (n, count)
 *   "screen_added" (s)       new output connected
 *   "screen_removed"(s)      output disconnected
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
#define REG_SERVER           "trixie_server"
#define REG_RULES            "trixie_rules"           /* array of {rule,properties} tables */
#define REG_PENDING_MONITORS "trixie_pending_monitors" /* monitor configs written before outputs exist */

/* Metatable names */
#define MT_CLIENT "TrixieClient"
#define MT_SCREEN "TrixieScreen"
#define MT_WIBOX "TrixieWibox"
#define MT_CANVAS "TrixieCanvas"
#define MT_TIMER  "TrixieTimer"
#define MT_WIDGET "TrixieWidget"

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
  /* geometry shortcuts */
  if (!strcmp(k, "x")) {
    lua_pushinteger(L, p->rect.x);
    return 1;
  }
  if (!strcmp(k, "y")) {
    lua_pushinteger(L, p->rect.y);
    return 1;
  }
  if (!strcmp(k, "width")) {
    lua_pushinteger(L, p->rect.w);
    return 1;
  }
  if (!strcmp(k, "height")) {
    lua_pushinteger(L, p->rect.h);
    return 1;
  }
  if (!strcmp(k, "minimized")) {
    lua_pushboolean(L, p->minimized);
    return 1;
  }
  if (!strcmp(k, "sticky")) {
    lua_pushboolean(L, p->sticky);
    return 1;
  }
  if (!strcmp(k, "above")) {
    lua_pushboolean(L, p->ontop);
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
    lua_emit_property(s, ud->id, "floating");
    return 0;
  }
  if (!strcmp(k, "fullscreen")) {
    bool want = lua_toboolean(L, 3);
    if (p->fullscreen != want) {
      p->fullscreen = want;
      TrixieView *v = view_from_pane(s, ud->id);
      if (v) {
        struct wlr_scene_tree *tgt =
            want ? s->layer_floating : s->layer_windows;
        wlr_scene_node_reparent(&v->scene_tree->node, tgt);
        if (!v->is_xwayland && v->xdg_toplevel)
          wlr_xdg_toplevel_set_fullscreen(v->xdg_toplevel, want);
      }
      twm_reflow(&s->twm);
      server_sync_windows(s);
      server_request_redraw(s);
    }
    lua_emit_property(s, ud->id, "fullscreen");
    return 0;
  }
  if (!strcmp(k, "opacity")) {
    p->rule_opacity = (float)luaL_checknumber(L, 3);
    server_sync_windows(s);
    lua_emit_property(s, ud->id, "opacity");
    return 0;
  }
  if (!strcmp(k, "border_width")) {
    s->cfg.border_width = s->twm.border_w = (int)luaL_checkinteger(L, 3);
    server_sync_windows(s);
    lua_emit_property(s, ud->id, "border_width");
    return 0;
  }
  if (!strcmp(k, "border_color")) {
    Color c = argb_to_color((uint32_t)luaL_checkinteger(L, 3));
    s->cfg.colors.active_border = c;
    server_request_redraw(s);
    lua_emit_property(s, ud->id, "border_color");
    return 0;
  }
  if (!strcmp(k, "workspace")) {
    int n = (int)luaL_checkinteger(L, 3);
    twm_set_focused(&s->twm, ud->id);
    twm_move_to_ws(&s->twm, n - 1);
    server_sync_windows(s);
    lua_emit_property(s, ud->id, "workspace");
    return 0;
  }
  if (!strcmp(k, "ontop") || !strcmp(k, "above")) {
    p->ontop = lua_toboolean(L, 3);
    TrixieView *v = view_from_pane(s, ud->id);
    if (v && p->ontop)
      wlr_scene_node_raise_to_top(&v->scene_tree->node);
    server_request_redraw(s);
    lua_emit_property(s, ud->id, "ontop");
    return 0;
  }
  /* geometry: absolute float position/size */
  if (!strcmp(k, "x") || !strcmp(k, "y") || !strcmp(k, "width") ||
      !strcmp(k, "height")) {
    if (!p->floating) {
      /* auto-float on geometry write, AwesomeWM behaviour */
      twm_set_focused(&s->twm, ud->id);
      if (!p->floating)
        server_float_toggle(s);
    }
    if (!strcmp(k, "x")) {
      p->float_rect.x = (int)luaL_checkinteger(L, 3);
    }
    if (!strcmp(k, "y")) {
      p->float_rect.y = (int)luaL_checkinteger(L, 3);
    }
    if (!strcmp(k, "width")) {
      p->float_rect.w = (int)luaL_checkinteger(L, 3);
    }
    if (!strcmp(k, "height")) {
      p->float_rect.h = (int)luaL_checkinteger(L, 3);
    }
    p->rect = p->float_rect;
    TrixieView *v = view_from_pane(s, ud->id);
    if (v)
      view_apply_geom_pub(s, v, p);
    server_sync_windows(s);
    lua_emit_property(s, ud->id, "geometry");
    return 0;
  }
  if (!strcmp(k, "minimized")) {
    p->minimized = lua_toboolean(L, 3);
    TrixieView *v = view_from_pane(s, ud->id);
    if (v)
      wlr_scene_node_set_enabled(&v->scene_tree->node, !p->minimized);
    server_request_redraw(s);
    lua_emit_property(s, ud->id, "minimized");
    return 0;
  }
  if (!strcmp(k, "sticky")) {
    p->sticky = lua_toboolean(L, 3);
    /* sticky: window appears on all workspaces — sync_windows checks this */
    server_sync_windows(s);
    lua_emit_property(s, ud->id, "sticky");
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

/* c:geometry() → {x,y,width,height}
 * c:geometry({x,y,width,height}) → set (auto-floats if tiled) */
static int client_geometry(lua_State *L) {
  ClientUD *ud = client_check(L, 1);
  TrixieServer *s = get_server(L);
  Pane *p = twm_pane_by_id(&s->twm, ud->id);
  if (!p) {
    lua_pushnil(L);
    return 1;
  }
  if (lua_istable(L, 2)) {
    /* write mode */
    if (!p->floating) {
      twm_set_focused(&s->twm, ud->id);
      server_float_toggle(s);
    }
    lua_getfield(L, 2, "x");
    if (lua_isnumber(L, -1))
      p->float_rect.x = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 2, "y");
    if (lua_isnumber(L, -1))
      p->float_rect.y = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 2, "width");
    if (lua_isnumber(L, -1))
      p->float_rect.w = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 2, "height");
    if (lua_isnumber(L, -1))
      p->float_rect.h = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    p->rect = p->float_rect;
    TrixieView *v = view_from_pane(s, ud->id);
    if (v)
      view_apply_geom_pub(s, v, p);
    server_sync_windows(s);
    lua_emit_property(s, ud->id, "geometry");
    return 0;
  }
  /* read mode */
  lua_createtable(L, 0, 4);
  lua_pushinteger(L, p->rect.x);
  lua_setfield(L, -2, "x");
  lua_pushinteger(L, p->rect.y);
  lua_setfield(L, -2, "y");
  lua_pushinteger(L, p->rect.w);
  lua_setfield(L, -2, "width");
  lua_pushinteger(L, p->rect.h);
  lua_setfield(L, -2, "height");
  return 1;
}

static int client_minimize(lua_State *L) {
  ClientUD *ud = client_check(L, 1);
  TrixieServer *s = get_server(L);
  Pane *p = twm_pane_by_id(&s->twm, ud->id);
  if (!p)
    return 0;
  p->minimized = true;
  TrixieView *v = view_from_pane(s, ud->id);
  if (v)
    wlr_scene_node_set_enabled(&v->scene_tree->node, false);
  server_request_redraw(s);
  lua_emit_property(s, ud->id, "minimized");
  return 0;
}

static int client_unminimize(lua_State *L) {
  ClientUD *ud = client_check(L, 1);
  TrixieServer *s = get_server(L);
  Pane *p = twm_pane_by_id(&s->twm, ud->id);
  if (!p)
    return 0;
  p->minimized = false;
  TrixieView *v = view_from_pane(s, ud->id);
  if (v)
    wlr_scene_node_set_enabled(&v->scene_tree->node, true);
  server_request_redraw(s);
  lua_emit_property(s, ud->id, "minimized");
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
    {"geometry", client_geometry},
    {"minimize", client_minimize},
    {"unminimize", client_unminimize},
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

/* canvas:draw_image(pixels, src_w, src_h, dx, dy, dw, dh)
 *   pixels — a Lua string of raw ARGB bytes (4 bytes per pixel, 0xAARRGGBB)
 *            OR a flat Lua table of 0xAARRGGBB integers
 *   src_w, src_h — source dimensions
 *   dx, dy       — destination position on canvas
 *   dw, dh       — destination size (nearest-neighbour scale, use src_w/h for
 * 1:1)
 *
 * Loading a PNG/JPEG from Lua is done outside (e.g. via io.open + a pure-Lua
 * decoder, or write a .argb sidecar with imagemagick).  The C side only blits.
 */
static int lcanvas_draw_image(lua_State *L) {
  CanvasUD *ud = canvas_check(L, 1);
  int src_w = (int)luaL_checkinteger(L, 3);
  int src_h = (int)luaL_checkinteger(L, 4);
  int dx = (int)luaL_checkinteger(L, 5);
  int dy = (int)luaL_checkinteger(L, 6);
  int dw = lua_isnumber(L, 7) ? (int)lua_tointeger(L, 7) : src_w;
  int dh = lua_isnumber(L, 8) ? (int)lua_tointeger(L, 8) : src_h;

  if (lua_type(L, 2) == LUA_TSTRING) {
    /* Raw binary string: 4 bytes per pixel ARGB */
    size_t len;
    const char *raw = lua_tolstring(L, 2, &len);
    int needed = src_w * src_h * 4;
    if ((int)len < needed)
      return luaL_error(L, "draw_image: string too short (%d bytes, need %d)",
                        (int)len, needed);
    canvas_draw_image(ud->c, (const uint32_t *)raw, src_w, src_h, dx, dy, dw,
                      dh);
  } else if (lua_type(L, 2) == LUA_TTABLE) {
    /* Flat table of 0xAARRGGBB integers — allocate temp buffer */
    int n = src_w * src_h;
    uint32_t *buf = (uint32_t *)malloc((size_t)n * 4);
    if (!buf)
      return luaL_error(L, "draw_image: out of memory");
    for (int i = 0; i < n; i++) {
      lua_rawgeti(L, 2, i + 1);
      buf[i] = (uint32_t)lua_tointeger(L, -1);
      lua_pop(L, 1);
    }
    canvas_draw_image(ud->c, buf, src_w, src_h, dx, dy, dw, dh);
    free(buf);
  } else {
    return luaL_error(L, "draw_image: arg 2 must be string or table");
  }
  return 0;
}

static const luaL_Reg canvas_methods[] = {
    {"fill_rect", lcanvas_fill_rect},
    {"draw_text", lcanvas_draw_text},
    {"draw_image", lcanvas_draw_image},
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
 * §5b Widget system
 * ══════════════════════════════════════════════════════════════════════════
 *
 * trixie.widget — composable widget primitives usable inside any wibox.
 *
 * Widget types
 * ─────────────
 *   trixie.widget.text     { text, text_fn, fg, align, padding_x, padding_y }
 *   trixie.widget.image    { pixels, src_w, src_h, width, height, padding }
 *   trixie.widget.progress { value, value_fn, fg, bg, border, padding }
 *   trixie.widget.separator{ thickness, color, padding }
 *   trixie.widget.container{ children={}, layout, spacing, padding,
 *                             bg, width, height }
 *     layout = "horizontal" | "vertical" | "fixed"
 *
 * Widget userdata API
 * ────────────────────
 *   w:measure(canvas)          → desired_w, desired_h
 *   w:draw(canvas, x, y, w, h) → (side-effects only)
 *
 * Usage
 * ─────
 *   local W = trixie.widget
 *   local bar = W.container {
 *     layout = "horizontal",
 *     children = {
 *       W.text { text_fn = function() return "ws "..trixie.workspace.focused end,
 *                fg = 0xFFCBA6F7, padding_x = 10 },
 *       W.separator {},
 *       W.text { text_fn = function() return os.date("%H:%M") end,
 *                align = "right", padding_x = 8 },
 *     }
 *   }
 *   wb:set_draw(function(canvas)
 *     bar:draw(canvas, 0, 0, canvas:width(), canvas:height())
 *   end)
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── Widget type tags ──────────────────────────────────────────────────── */
typedef enum {
  WGT_TEXT = 1,
  WGT_IMAGE,
  WGT_PROGRESS,
  WGT_SEPARATOR,
  WGT_CONTAINER,
} WidgetKind;

/* ── Shared color helper (mirrors bar.c's argb unpacking) ──────────────── */
static Color wgt_color(lua_State *L, int idx, uint8_t def_a) {
  /* Accepts 0xAARRGGBB integer or a {a,r,g,b} table.
   * If alpha bytes are 0 and the whole value is non-zero we assume
   * the caller passed 0xRRGGBB and supply def_a. */
  Color c = {0};
  if (lua_type(L, idx) == LUA_TNUMBER) {
    uint32_t v = (uint32_t)lua_tointeger(L, idx);
    c.a = (v >> 24) & 0xff;
    c.r = (v >> 16) & 0xff;
    c.g = (v >>  8) & 0xff;
    c.b =  v        & 0xff;
    if (c.a == 0 && v != 0) c.a = def_a;
  } else if (lua_istable(L, idx)) {
    lua_getfield(L, idx, "a"); c.a = (uint8_t)lua_tointeger(L, -1); lua_pop(L, 1);
    lua_getfield(L, idx, "r"); c.r = (uint8_t)lua_tointeger(L, -1); lua_pop(L, 1);
    lua_getfield(L, idx, "g"); c.g = (uint8_t)lua_tointeger(L, -1); lua_pop(L, 1);
    lua_getfield(L, idx, "b"); c.b = (uint8_t)lua_tointeger(L, -1); lua_pop(L, 1);
  } else {
    /* fallback: fully-opaque white */
    c = (Color){.a=def_a,.r=255,.g=255,.b=255};
  }
  return c;
}

/* ── Widget userdata ───────────────────────────────────────────────────── */

/* Maximum children a container can hold without heap allocation. */
#define WGT_MAX_CHILDREN 64

typedef struct WidgetUD WidgetUD;
struct WidgetUD {
  WidgetKind kind;

  /* ── TEXT ── */
  struct {
    char   text[256];   /* static text (empty → use fn) */
    int    fn_ref;      /* LUA_NOREF or registry ref to text_fn() */
    Color  fg;
    int    align;       /* 0=left 1=center 2=right */
    int    padding_x;
    int    padding_y;
  } text;

  /* ── IMAGE ── */
  struct {
    int    fn_ref;   /* registry ref to pixels string / table, or LUA_NOREF */
    int    src_w, src_h;
    int    width, height;
    int    padding;
  } image;

  /* ── PROGRESS ── */
  struct {
    float  value;       /* 0.0..1.0 static, or use fn */
    int    fn_ref;      /* LUA_NOREF or registry ref to value_fn() */
    Color  fg;
    Color  bg;
    Color  border;
    int    padding;
    bool   has_border;
  } progress;

  /* ── SEPARATOR ── */
  struct {
    int   thickness;
    Color color;
    int   padding;
  } sep;

  /* ── CONTAINER ── */
  struct {
    int        layout;       /* 0=horizontal 1=vertical 2=fixed */
    int        spacing;
    int        padding;
    bool       has_bg;
    Color      bg;
    int        fixed_w;      /* 0 → measure children */
    int        fixed_h;
    /* child refs: strong refs in registry so GC keeps them alive */
    int        child_refs[WGT_MAX_CHILDREN];
    int        child_count;
  } container;
};

#define CLAYOUT_HORIZONTAL 0
#define CLAYOUT_VERTICAL   1
#define CLAYOUT_FIXED      2

/* Forward declaration — containers call measure/draw recursively. */
static WidgetUD *widget_check(lua_State *L, int idx);

/* ── wgt_get_pixels: pull pixel data out of a registry ref ─────────────── */
/* Returns a malloc'd buffer that the caller must free, or NULL on failure.
 * Supports: raw byte-string (ARGB8888 packed) or table of integers. */
static uint32_t *wgt_get_pixels(lua_State *L, int fn_ref,
                                int *out_w, int *out_h) {
  if (fn_ref == LUA_NOREF) return NULL;
  lua_rawgeti(L, LUA_REGISTRYINDEX, fn_ref);
  if (!lua_istable(L, -1) && !lua_isstring(L, -1)) {
    lua_pop(L, 1);
    return NULL;
  }
  /* We expect a table with fields: pixels (string or int array), w, h */
  if (lua_istable(L, -1)) {
    lua_getfield(L, -1, "w");  *out_w = (int)lua_tointeger(L, -1); lua_pop(L, 1);
    lua_getfield(L, -1, "h");  *out_h = (int)lua_tointeger(L, -1); lua_pop(L, 1);
    lua_getfield(L, -1, "pixels");
    /* pixels may be a byte string or a sequence of uint32 */
    if (lua_isstring(L, -1)) {
      size_t len = 0;
      const char *raw = lua_tolstring(L, -1, &len);
      size_t need = (size_t)(*out_w) * (size_t)(*out_h) * 4;
      if (len >= need) {
        uint32_t *buf = malloc(need);
        if (buf) memcpy(buf, raw, need);
        lua_pop(L, 2);
        return buf;
      }
    }
    lua_pop(L, 1);
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  return NULL;
}

/* ── wgt_text_value: get current text for a text widget ────────────────── */
static const char *wgt_text_value(lua_State *L, WidgetUD *ud,
                                  char *buf, int bufsz) {
  if (ud->text.fn_ref != LUA_NOREF) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, ud->text.fn_ref);
    if (lua_isfunction(L, -1)) {
      if (lua_pcall(L, 0, 1, 0) == LUA_OK) {
        const char *s = lua_tostring(L, -1);
        if (s) strncpy(buf, s, bufsz - 1);
        else   buf[0] = '\0';
        lua_pop(L, 1);
        return buf;
      }
      wlr_log(WLR_ERROR, "[widget] text_fn: %s", lua_tostring(L, -1));
      lua_pop(L, 1);
    } else {
      lua_pop(L, 1);
    }
  }
  return ud->text.text;
}

/* ── wgt_progress_value: get current value (0..1) ─────────────────────── */
static float wgt_progress_value(lua_State *L, WidgetUD *ud) {
  if (ud->progress.fn_ref != LUA_NOREF) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, ud->progress.fn_ref);
    if (lua_isfunction(L, -1)) {
      if (lua_pcall(L, 0, 1, 0) == LUA_OK) {
        float v = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
        if (v < 0.f) v = 0.f;
        if (v > 1.f) v = 1.f;
        return v;
      }
      wlr_log(WLR_ERROR, "[widget] value_fn: %s", lua_tostring(L, -1));
      lua_pop(L, 1);
    } else {
      lua_pop(L, 1);
    }
  }
  float v = ud->progress.value;
  if (v < 0.f) v = 0.f;
  if (v > 1.f) v = 1.f;
  return v;
}

/* ── measure ───────────────────────────────────────────────────────────── */

static void widget_measure(lua_State *L, WidgetUD *ud,
                            Canvas *c, int *out_w, int *out_h) {
  switch (ud->kind) {

  case WGT_TEXT: {
    char buf[256] = {0};
    const char *txt = wgt_text_value(L, ud, buf, sizeof(buf));
    int tw = canvas_measure(txt);
    *out_w = tw + ud->text.padding_x * 2;
    *out_h = canvas_font_height() + ud->text.padding_y * 2;
    break;
  }

  case WGT_IMAGE:
    *out_w = ud->image.width  + ud->image.padding * 2;
    *out_h = ud->image.height + ud->image.padding * 2;
    break;

  case WGT_PROGRESS:
    /* Progress bars expand to fill; report a minimum so callers have a floor */
    *out_w = 40 + ud->progress.padding * 2;
    *out_h = canvas_font_height() + ud->progress.padding * 2;
    break;

  case WGT_SEPARATOR:
    *out_w = ud->sep.thickness + ud->sep.padding * 2;
    *out_h = canvas_font_height();
    break;

  case WGT_CONTAINER: {
    int layout   = ud->container.layout;
    int spacing  = ud->container.spacing;
    int pad      = ud->container.padding;
    int total_w  = 0, total_h = 0;
    for (int i = 0; i < ud->container.child_count; i++) {
      lua_rawgeti(L, LUA_REGISTRYINDEX, ud->container.child_refs[i]);
      WidgetUD *ch = widget_check(L, -1);
      lua_pop(L, 1);
      if (!ch) continue;
      int cw = 0, ch_h = 0;
      widget_measure(L, ch, c, &cw, &ch_h);
      if (layout == CLAYOUT_HORIZONTAL) {
        total_w += cw + (i > 0 ? spacing : 0);
        if (ch_h > total_h) total_h = ch_h;
      } else {
        total_h += ch_h + (i > 0 ? spacing : 0);
        if (cw > total_w) total_w = cw;
      }
    }
    *out_w = (ud->container.fixed_w > 0)
              ? ud->container.fixed_w
              : total_w + pad * 2;
    *out_h = (ud->container.fixed_h > 0)
              ? ud->container.fixed_h
              : total_h + pad * 2;
    break;
  }

  default:
    *out_w = 0;
    *out_h = 0;
    break;
  }
}

/* ── draw ──────────────────────────────────────────────────────────────── */

static void widget_draw(lua_State *L, WidgetUD *ud,
                        Canvas *c, int x, int y, int w, int h) {
  switch (ud->kind) {

  case WGT_TEXT: {
    char buf[256] = {0};
    const char *txt = wgt_text_value(L, ud, buf, sizeof(buf));
    int fh = canvas_font_height();
    int fa = canvas_font_ascender();
    int ty = y + (h - fh) / 2 + fa;
    int tw = canvas_measure(txt);
    int tx;
    if      (ud->text.align == 1) tx = x + (w - tw) / 2;             /* center */
    else if (ud->text.align == 2) tx = x + w - tw - ud->text.padding_x; /* right */
    else                          tx = x + ud->text.padding_x;          /* left  */
    canvas_draw_text(c, tx, ty, txt, ud->text.fg);
    break;
  }

  case WGT_IMAGE: {
    int src_w = 0, src_h = 0;
    uint32_t *px = wgt_get_pixels(L, ud->image.fn_ref, &src_w, &src_h);
    if (px) {
      int p  = ud->image.padding;
      int dw = ud->image.width;
      int dh = ud->image.height;
      int dx = x + p + (w - dw - p * 2) / 2;
      int dy = y + (h - dh) / 2;
      canvas_draw_image(c, px, src_w, src_h, dx, dy, dw, dh);
      free(px);
    }
    break;
  }

  case WGT_PROGRESS: {
    int p    = ud->progress.padding;
    int bx   = x + p, by = y + p;
    int bw   = w - p * 2, bh = h - p * 2;
    if (bw <= 0 || bh <= 0) break;
    /* Background */
    canvas_fill_rect(c, bx, by, bw, bh, ud->progress.bg);
    /* Optional border */
    if (ud->progress.has_border) {
      /* top/bottom/left/right 1-px borders */
      canvas_fill_rect(c, bx, by, bw, 1, ud->progress.border);
      canvas_fill_rect(c, bx, by + bh - 1, bw, 1, ud->progress.border);
      canvas_fill_rect(c, bx, by, 1, bh, ud->progress.border);
      canvas_fill_rect(c, bx + bw - 1, by, 1, bh, ud->progress.border);
      bx++; by++; bw -= 2; bh -= 2;
    }
    /* Fill */
    float val = wgt_progress_value(L, ud);
    int   fw  = (int)(bw * val);
    if (fw > 0)
      canvas_fill_rect(c, bx, by, fw, bh, ud->progress.fg);
    break;
  }

  case WGT_SEPARATOR: {
    int p  = ud->sep.padding;
    int tx = x + p;
    int sh = h - p * 2;
    if (sh > 0)
      canvas_fill_rect(c, tx, y + p, ud->sep.thickness, sh, ud->sep.color);
    break;
  }

  case WGT_CONTAINER: {
    int layout  = ud->container.layout;
    int spacing = ud->container.spacing;
    int pad     = ud->container.padding;
    /* Optional background */
    if (ud->container.has_bg)
      canvas_fill_rect(c, x, y, w, h, ud->container.bg);
    int cx = x + pad, cy = y + pad;
    int avail_w = w - pad * 2;
    int avail_h = h - pad * 2;
    /* ── Horizontal layout: measure all, then draw left-to-right ──────── */
    if (layout == CLAYOUT_HORIZONTAL) {
      /* First pass: measure each child and count "fill" (0-desired-w) ones */
      int sizes[WGT_MAX_CHILDREN];
      int is_fill[WGT_MAX_CHILDREN];
      int used = 0;
      int fill_count = 0;
      for (int i = 0; i < ud->container.child_count; i++) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ud->container.child_refs[i]);
        WidgetUD *ch = widget_check(L, -1);
        lua_pop(L, 1);
        int cw = 0, ch_h = 0;
        if (ch) widget_measure(L, ch, c, &cw, &ch_h);
        sizes[i] = cw;
        /* A zero desired_w (e.g. a fill-spacer) participates in flex */
        is_fill[i] = (cw == 0) ? 1 : 0;
        if (!is_fill[i]) used += cw;
        if (i > 0) used += spacing;
        if (is_fill[i]) fill_count++;
      }
      /* Distribute leftover to fill children */
      int leftover = avail_w - used;
      int fill_each = (fill_count > 0 && leftover > 0)
                      ? leftover / fill_count : 0;
      /* Second pass: draw */
      for (int i = 0; i < ud->container.child_count; i++) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ud->container.child_refs[i]);
        WidgetUD *ch = widget_check(L, -1);
        lua_pop(L, 1);
        if (!ch) continue;
        int slot_w = is_fill[i] ? fill_each : sizes[i];
        widget_draw(L, ch, c, cx, cy, slot_w, avail_h);
        cx += slot_w + spacing;
      }
    }
    /* ── Vertical layout ─────────────────────────────────────────────── */
    else if (layout == CLAYOUT_VERTICAL) {
      int sizes[WGT_MAX_CHILDREN];
      int is_fill[WGT_MAX_CHILDREN];
      int used = 0, fill_count = 0;
      for (int i = 0; i < ud->container.child_count; i++) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ud->container.child_refs[i]);
        WidgetUD *ch = widget_check(L, -1);
        lua_pop(L, 1);
        int cw = 0, ch_h = 0;
        if (ch) widget_measure(L, ch, c, &cw, &ch_h);
        sizes[i] = ch_h;
        is_fill[i] = (ch_h == 0) ? 1 : 0;
        if (!is_fill[i]) used += ch_h;
        if (i > 0) used += spacing;
        if (is_fill[i]) fill_count++;
      }
      int leftover  = avail_h - used;
      int fill_each = (fill_count > 0 && leftover > 0)
                      ? leftover / fill_count : 0;
      for (int i = 0; i < ud->container.child_count; i++) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ud->container.child_refs[i]);
        WidgetUD *ch = widget_check(L, -1);
        lua_pop(L, 1);
        if (!ch) continue;
        int slot_h = is_fill[i] ? fill_each : sizes[i];
        widget_draw(L, ch, c, cx, cy, avail_w, slot_h);
        cy += slot_h + spacing;
      }
    }
    /* ── Fixed layout: each child given the full content area ────────── */
    else {
      for (int i = 0; i < ud->container.child_count; i++) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ud->container.child_refs[i]);
        WidgetUD *ch = widget_check(L, -1);
        lua_pop(L, 1);
        if (ch) widget_draw(L, ch, c, cx, cy, avail_w, avail_h);
      }
    }
    break;
  }

  default:
    break;
  }
}

/* ── Userdata helpers ──────────────────────────────────────────────────── */

static WidgetUD *widget_check(lua_State *L, int idx) {
  return (WidgetUD *)luaL_testudata(L, idx, MT_WIDGET);
}

static WidgetUD *widget_new(lua_State *L) {
  WidgetUD *ud = (WidgetUD *)lua_newuserdata(L, sizeof(WidgetUD));
  memset(ud, 0, sizeof(*ud));
  ud->text.fn_ref       = LUA_NOREF;
  ud->image.fn_ref      = LUA_NOREF;
  ud->progress.fn_ref   = LUA_NOREF;
  for (int i = 0; i < WGT_MAX_CHILDREN; i++)
    ud->container.child_refs[i] = LUA_NOREF;
  luaL_setmetatable(L, MT_WIDGET);
  return ud;
}

/* ── Lua methods on widget userdata ────────────────────────────────────── */

/* w:measure(canvas) → desired_w, desired_h */
static int lwidget_measure(lua_State *L) {
  WidgetUD *ud = widget_check(L, 1);
  if (!ud) return luaL_error(L, "widget expected");
  CanvasUD *cud = (CanvasUD *)luaL_checkudata(L, 2, MT_CANVAS);
  int dw = 0, dh = 0;
  widget_measure(L, ud, cud->c, &dw, &dh);
  lua_pushinteger(L, dw);
  lua_pushinteger(L, dh);
  return 2;
}

/* w:draw(canvas, x, y, w, h) */
static int lwidget_draw(lua_State *L) {
  WidgetUD *ud = widget_check(L, 1);
  if (!ud) return luaL_error(L, "widget expected");
  CanvasUD *cud = (CanvasUD *)luaL_checkudata(L, 2, MT_CANVAS);
  int x = (int)luaL_checkinteger(L, 3);
  int y = (int)luaL_checkinteger(L, 4);
  int w = (int)luaL_checkinteger(L, 5);
  int h = (int)luaL_checkinteger(L, 6);
  widget_draw(L, ud, cud->c, x, y, w, h);
  return 0;
}

/* w:set(key, value) — update mutable fields without recreating the widget.
 * Supported keys per type:
 *   text:     "text", "fg", "align", "padding_x", "padding_y"
 *   image:    "width", "height", "padding"
 *   progress: "value", "fg", "bg", "padding"
 *   sep:      "thickness", "color", "padding"
 *   container:"spacing", "padding", "bg"
 */
static int lwidget_set(lua_State *L) {
  WidgetUD *ud  = widget_check(L, 1);
  if (!ud) return luaL_error(L, "widget expected");
  const char *k = luaL_checkstring(L, 2);
  switch (ud->kind) {
  case WGT_TEXT:
    if (!strcmp(k, "text")) {
      const char *t = luaL_checkstring(L, 3);
      strncpy(ud->text.text, t, sizeof(ud->text.text) - 1);
    } else if (!strcmp(k, "fg")) {
      ud->text.fg = wgt_color(L, 3, 255);
    } else if (!strcmp(k, "align")) {
      const char *a = luaL_checkstring(L, 3);
      ud->text.align = (!strcmp(a,"center")) ? 1 : (!strcmp(a,"right")) ? 2 : 0;
    } else if (!strcmp(k, "padding_x")) {
      ud->text.padding_x = (int)luaL_checkinteger(L, 3);
    } else if (!strcmp(k, "padding_y")) {
      ud->text.padding_y = (int)luaL_checkinteger(L, 3);
    }
    break;
  case WGT_PROGRESS:
    if (!strcmp(k, "value")) {
      ud->progress.value = (float)luaL_checknumber(L, 3);
    } else if (!strcmp(k, "fg")) {
      ud->progress.fg = wgt_color(L, 3, 255);
    } else if (!strcmp(k, "bg")) {
      ud->progress.bg = wgt_color(L, 3, 255);
    } else if (!strcmp(k, "padding")) {
      ud->progress.padding = (int)luaL_checkinteger(L, 3);
    }
    break;
  case WGT_SEPARATOR:
    if (!strcmp(k, "color")) {
      ud->sep.color = wgt_color(L, 3, 255);
    } else if (!strcmp(k, "thickness")) {
      ud->sep.thickness = (int)luaL_checkinteger(L, 3);
    } else if (!strcmp(k, "padding")) {
      ud->sep.padding = (int)luaL_checkinteger(L, 3);
    }
    break;
  case WGT_CONTAINER:
    if (!strcmp(k, "spacing")) {
      ud->container.spacing = (int)luaL_checkinteger(L, 3);
    } else if (!strcmp(k, "padding")) {
      ud->container.padding = (int)luaL_checkinteger(L, 3);
    } else if (!strcmp(k, "bg")) {
      ud->container.bg = wgt_color(L, 3, 255);
      ud->container.has_bg = true;
    }
    break;
  default:
    break;
  }
  return 0;
}

/* __gc: release any stored Lua refs */
static int lwidget_gc(lua_State *L) {
  WidgetUD *ud = widget_check(L, 1);
  if (!ud) return 0;
  if (ud->text.fn_ref     != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, ud->text.fn_ref);
  if (ud->image.fn_ref    != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, ud->image.fn_ref);
  if (ud->progress.fn_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, ud->progress.fn_ref);
  for (int i = 0; i < ud->container.child_count; i++)
    if (ud->container.child_refs[i] != LUA_NOREF)
      luaL_unref(L, LUA_REGISTRYINDEX, ud->container.child_refs[i]);
  return 0;
}

static const luaL_Reg widget_methods[] = {
  {"measure", lwidget_measure},
  {"draw",    lwidget_draw},
  {"set",     lwidget_set},
  {NULL, NULL}
};

/* ── Constructor helpers ───────────────────────────────────────────────── */

/* opt_color(L, table_idx, field, default_hex) */
static Color opt_color(lua_State *L, int t, const char *field, uint32_t def) {
  lua_getfield(L, t, field);
  Color c;
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    c.a = (def >> 24) & 0xff;
    c.r = (def >> 16) & 0xff;
    c.g = (def >>  8) & 0xff;
    c.b =  def        & 0xff;
    if (!c.a && def) c.a = 255;
  } else {
    c = wgt_color(L, -1, 255);
    lua_pop(L, 1);
  }
  return c;
}

/* trixie.widget.text { text, text_fn, fg, align, padding_x, padding_y } */
static int lwgt_text(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  WidgetUD *ud = widget_new(L);  /* pushed */
  ud->kind = WGT_TEXT;

  /* text */
  lua_getfield(L, 1, "text");
  if (lua_isstring(L, -1))
    strncpy(ud->text.text, lua_tostring(L, -1), sizeof(ud->text.text) - 1);
  lua_pop(L, 1);

  /* text_fn */
  lua_getfield(L, 1, "text_fn");
  if (lua_isfunction(L, -1))
    ud->text.fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  else
    lua_pop(L, 1);

  /* fg — default subtext1-ish white */
  ud->text.fg = opt_color(L, 1, "fg", 0xFFCDD6F4);

  /* align */
  lua_getfield(L, 1, "align");
  if (lua_isstring(L, -1)) {
    const char *a = lua_tostring(L, -1);
    ud->text.align = (!strcmp(a,"center")) ? 1 : (!strcmp(a,"right")) ? 2 : 0;
  }
  lua_pop(L, 1);

  /* padding */
  lua_getfield(L, 1, "padding_x");
  ud->text.padding_x = lua_isnumber(L, -1) ? (int)lua_tointeger(L, -1) : 8;
  lua_pop(L, 1);

  lua_getfield(L, 1, "padding_y");
  ud->text.padding_y = lua_isnumber(L, -1) ? (int)lua_tointeger(L, -1) : 0;
  lua_pop(L, 1);

  return 1; /* widget userdata */
}

/* trixie.widget.image { pixels_fn, src_w, src_h, width, height, padding }
 * pixels_fn is a function that returns a {pixels, w, h} table, OR
 * the opts table itself may have a .pixels field (string). */
static int lwgt_image(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  WidgetUD *ud = widget_new(L);
  ud->kind = WGT_IMAGE;

  /* pixels_fn or pixels table */
  lua_getfield(L, 1, "pixels_fn");
  if (lua_isfunction(L, -1)) {
    ud->image.fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  } else {
    lua_pop(L, 1);
    /* Static pixel blob: store the table as a ref directly */
    lua_getfield(L, 1, "pixels");
    if (!lua_isnil(L, -1))
      ud->image.fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    else
      lua_pop(L, 1);
  }

  lua_getfield(L, 1, "src_w");
  ud->image.src_w = lua_isnumber(L,-1) ? (int)lua_tointeger(L,-1) : 0;
  lua_pop(L, 1);

  lua_getfield(L, 1, "src_h");
  ud->image.src_h = lua_isnumber(L,-1) ? (int)lua_tointeger(L,-1) : 0;
  lua_pop(L, 1);

  lua_getfield(L, 1, "width");
  ud->image.width = lua_isnumber(L,-1)
                    ? (int)lua_tointeger(L,-1) : ud->image.src_w;
  lua_pop(L, 1);

  lua_getfield(L, 1, "height");
  ud->image.height = lua_isnumber(L,-1)
                     ? (int)lua_tointeger(L,-1) : ud->image.src_h;
  lua_pop(L, 1);

  lua_getfield(L, 1, "padding");
  ud->image.padding = lua_isnumber(L,-1) ? (int)lua_tointeger(L,-1) : 2;
  lua_pop(L, 1);

  return 1;
}

/* trixie.widget.progress { value, value_fn, fg, bg, border, padding } */
static int lwgt_progress(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  WidgetUD *ud = widget_new(L);
  ud->kind = WGT_PROGRESS;

  lua_getfield(L, 1, "value");
  ud->progress.value = lua_isnumber(L,-1) ? (float)lua_tonumber(L,-1) : 0.f;
  lua_pop(L, 1);

  lua_getfield(L, 1, "value_fn");
  if (lua_isfunction(L, -1))
    ud->progress.fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  else
    lua_pop(L, 1);

  ud->progress.fg = opt_color(L, 1, "fg", 0xFF89B4FA); /* blue */
  ud->progress.bg = opt_color(L, 1, "bg", 0xFF313244); /* surface0 */

  lua_getfield(L, 1, "border");
  if (!lua_isnil(L, -1)) {
    ud->progress.border    = wgt_color(L, -1, 255);
    ud->progress.has_border = true;
  }
  lua_pop(L, 1);

  lua_getfield(L, 1, "padding");
  ud->progress.padding = lua_isnumber(L,-1) ? (int)lua_tointeger(L,-1) : 2;
  lua_pop(L, 1);

  return 1;
}

/* trixie.widget.separator { thickness, color, padding } */
static int lwgt_separator(lua_State *L) {
  /* opts table is optional — separator has good defaults */
  WidgetUD *ud = widget_new(L);
  ud->kind = WGT_SEPARATOR;
  ud->sep.thickness = 1;
  ud->sep.color     = (Color){.a=255,.r=69,.g=71,.b=90};  /* surface1 */
  ud->sep.padding   = 4;

  if (lua_gettop(L) >= 1 && lua_istable(L, 1)) {
    lua_getfield(L, 1, "thickness");
    if (lua_isnumber(L,-1)) ud->sep.thickness = (int)lua_tointeger(L,-1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "color");
    if (!lua_isnil(L,-1)) ud->sep.color = wgt_color(L, -1, 255);
    lua_pop(L, 1);

    lua_getfield(L, 1, "padding");
    if (lua_isnumber(L,-1)) ud->sep.padding = (int)lua_tointeger(L,-1);
    lua_pop(L, 1);
  }
  return 1;
}

/* trixie.widget.spacer() — zero-desired-width spacer for flex fill */
static int lwgt_spacer(lua_State *L) {
  (void)L;
  WidgetUD *ud = widget_new(L);
  ud->kind = WGT_SEPARATOR;
  ud->sep.thickness = 0;
  ud->sep.padding   = 0;
  ud->sep.color     = (Color){0};
  return 1;
}

/* trixie.widget.container { children, layout, spacing, padding, bg,
 *                           width, height }
 * layout = "horizontal" (default) | "vertical" | "fixed"
 *
 * Children can be widget userdata OR plain Lua functions that take
 * (canvas, x, y, w, h) — an escape hatch for custom drawing. */
static int lwgt_container(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  WidgetUD *ud = widget_new(L);   /* pushed as stack top */
  ud->kind = WGT_CONTAINER;
  int self_idx = lua_gettop(L);   /* index of the new userdata */

  /* layout */
  lua_getfield(L, 1, "layout");
  if (lua_isstring(L, -1)) {
    const char *lo = lua_tostring(L, -1);
    if      (!strcmp(lo, "vertical")) ud->container.layout = CLAYOUT_VERTICAL;
    else if (!strcmp(lo, "fixed"))    ud->container.layout = CLAYOUT_FIXED;
    else                              ud->container.layout = CLAYOUT_HORIZONTAL;
  }
  lua_pop(L, 1);

  /* spacing, padding */
  lua_getfield(L, 1, "spacing");
  ud->container.spacing = lua_isnumber(L,-1) ? (int)lua_tointeger(L,-1) : 0;
  lua_pop(L, 1);

  lua_getfield(L, 1, "padding");
  ud->container.padding = lua_isnumber(L,-1) ? (int)lua_tointeger(L,-1) : 0;
  lua_pop(L, 1);

  /* fixed size */
  lua_getfield(L, 1, "width");
  ud->container.fixed_w = lua_isnumber(L,-1) ? (int)lua_tointeger(L,-1) : 0;
  lua_pop(L, 1);

  lua_getfield(L, 1, "height");
  ud->container.fixed_h = lua_isnumber(L,-1) ? (int)lua_tointeger(L,-1) : 0;
  lua_pop(L, 1);

  /* optional background */
  lua_getfield(L, 1, "bg");
  if (!lua_isnil(L, -1)) {
    ud->container.bg     = wgt_color(L, -1, 255);
    ud->container.has_bg = true;
  }
  lua_pop(L, 1);

  /* children */
  lua_getfield(L, 1, "children");
  if (lua_istable(L, -1)) {
    int n = (int)lua_objlen(L, -1);
    for (int i = 1; i <= n && ud->container.child_count < WGT_MAX_CHILDREN; i++) {
      lua_rawgeti(L, -1, i);
      /* Accept widget userdata or a callable (function / table with __call) */
      if (widget_check(L, -1) || lua_isfunction(L, -1) || lua_istable(L, -1)) {
        ud->container.child_refs[ud->container.child_count++] =
          luaL_ref(L, LUA_REGISTRYINDEX);
      } else {
        lua_pop(L, 1); /* discard unknown types silently */
      }
    }
  }
  lua_pop(L, 1); /* pop children table */

  lua_pushvalue(L, self_idx); /* return the userdata */
  return 1;
}

/* ── trixie.widget sub-table ───────────────────────────────────────────── */

/* Called from luaopen_trixie to push the trixie.widget table. */
static void push_widget_table(lua_State *L) {
  lua_createtable(L, 0, 6);
  lua_pushcfunction(L, lwgt_text);      lua_setfield(L, -2, "text");
  lua_pushcfunction(L, lwgt_image);     lua_setfield(L, -2, "image");
  lua_pushcfunction(L, lwgt_progress);  lua_setfield(L, -2, "progress");
  lua_pushcfunction(L, lwgt_separator); lua_setfield(L, -2, "separator");
  lua_pushcfunction(L, lwgt_spacer);    lua_setfield(L, -2, "spacer");
  lua_pushcfunction(L, lwgt_container); lua_setfield(L, -2, "container");
}

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
/* ── trixie.layout.register(name, fn) ──────────────────────────────────────
 * Register a custom Lua layout.  fn(clients, geometry) is called at reflow.
 * We store it in the registry under "trixie_layouts" table. */
#define REG_LAYOUTS "trixie_layouts"

static int l_layout_register(lua_State *L) {
  const char *name = luaL_checkstring(L, 1);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  /* Ensure the layout registry table exists */
  lua_getfield(L, LUA_REGISTRYINDEX, REG_LAYOUTS);
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, REG_LAYOUTS);
  }
  lua_pushvalue(L, 2);
  lua_setfield(L, -2, name);
  lua_pop(L, 1);
  wlr_log(WLR_INFO, "[lua] registered layout '%s'", name);
  return 0;
}

/* Called from twm/layout C code when layout name matches a Lua layout.
 * Returns true if a Lua layout handled it. */
bool lua_call_layout(TrixieServer *s, const char *name, PaneId *panes,
                     int npanes, Rect area, int gap) {
  if (!s->L || !name)
    return false;
  lua_getfield(s->L, LUA_REGISTRYINDEX, REG_LAYOUTS);
  if (!lua_istable(s->L, -1)) {
    lua_pop(s->L, 1);
    return false;
  }
  lua_getfield(s->L, -1, name);
  if (!lua_isfunction(s->L, -1)) {
    lua_pop(s->L, 2);
    return false;
  }
  /* Push clients array */
  lua_createtable(s->L, npanes, 0);
  for (int i = 0; i < npanes; i++) {
    push_client(s->L, panes[i]);
    lua_rawseti(s->L, -2, i + 1);
  }
  /* Push geometry */
  lua_createtable(s->L, 0, 5);
  lua_pushinteger(s->L, area.x);
  lua_setfield(s->L, -2, "x");
  lua_pushinteger(s->L, area.y);
  lua_setfield(s->L, -2, "y");
  lua_pushinteger(s->L, area.w);
  lua_setfield(s->L, -2, "width");
  lua_pushinteger(s->L, area.h);
  lua_setfield(s->L, -2, "height");
  lua_pushinteger(s->L, gap);
  lua_setfield(s->L, -2, "gap");
  if (lua_pcall(s->L, 2, 0, 0) != LUA_OK) {
    wlr_log(WLR_ERROR, "[lua] layout '%s': %s", name, lua_tostring(s->L, -1));
    lua_pop(s->L, 1);
  }
  lua_pop(s->L, 1); /* layouts table */
  return true;
}

/* ── trixie.button({mods}, btn, fn [, fn_release]) ─────────────────────────
 * Global mouse button binding.  btn = 1/2/3 (left/middle/right).
 * Stored in registry REG_BUTTONS table alongside key binds. */
#define REG_BUTTONS "trixie_buttons"
#define MAX_BUTTONS 128

static int l_button(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  int btn = (int)luaL_checkinteger(L, 2);
  luaL_checktype(L, 3, LUA_TFUNCTION);
  TrixieServer *s = get_server(L);
  uint32_t mods = mods_from_table(L, 1);
  /* Store in server lua_binds-style array via registry table */
  lua_getfield(L, LUA_REGISTRYINDEX, REG_BUTTONS);
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, REG_BUTTONS);
  }
  int n = (int)lua_objlen(L, -1);
  lua_createtable(L, 0, 4);
  lua_pushinteger(L, (lua_Integer)mods);
  lua_setfield(L, -2, "mods");
  lua_pushinteger(L, btn);
  lua_setfield(L, -2, "btn");
  lua_pushvalue(L, 3);
  lua_setfield(L, -2, "fn");
  if (lua_isfunction(L, 4)) {
    lua_pushvalue(L, 4);
    lua_setfield(L, -2, "fn_release");
  }
  lua_rawseti(L, -2, n + 1);
  lua_pop(L, 1);
  (void)s;
  return 0;
}

/* Dispatch a button press from main.c cursor_button handler.
 * Returns true if consumed. */
bool lua_dispatch_button(TrixieServer *s, uint32_t mods, uint32_t btn,
                         bool pressed) {
  if (!s->L)
    return false;
  lua_getfield(s->L, LUA_REGISTRYINDEX, REG_BUTTONS);
  if (!lua_istable(s->L, -1)) {
    lua_pop(s->L, 1);
    return false;
  }
  int n = (int)lua_objlen(s->L, -1);
  bool handled = false;
  for (int i = 1; i <= n; i++) {
    lua_rawgeti(s->L, -1, i);
    lua_getfield(s->L, -1, "mods");
    uint32_t bmods = (uint32_t)lua_tointeger(s->L, -1);
    lua_pop(s->L, 1);
    lua_getfield(s->L, -1, "btn");
    uint32_t bbtn = (uint32_t)lua_tointeger(s->L, -1);
    lua_pop(s->L, 1);
    if (bmods == mods && bbtn == btn) {
      const char *fnkey = pressed ? "fn" : "fn_release";
      lua_getfield(s->L, -1, fnkey);
      if (lua_isfunction(s->L, -1)) {
        if (lua_pcall(s->L, 0, 0, 0) != LUA_OK) {
          wlr_log(WLR_ERROR, "[lua] button: %s", lua_tostring(s->L, -1));
          lua_pop(s->L, 1);
        }
        handled = true;
      } else {
        lua_pop(s->L, 1);
      }
    }
    lua_pop(s->L, 1);
    if (handled)
      break;
  }
  lua_pop(s->L, 1);
  return handled;
}

/* ── trixie.gesture(spec, fn) ───────────────────────────────────────────────
 * spec = "swipe:3:left" | "swipe:3:right" | "swipe:3:up" | "swipe:3:down"
 *        "pinch:in" | "pinch:out"
 * Stored in registry; dispatched from gesture.c callbacks via
 * lua_dispatch_gesture. */
#define REG_GESTURES "trixie_gestures"

static int l_gesture(lua_State *L) {
  const char *spec = luaL_checkstring(L, 1);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  lua_getfield(L, LUA_REGISTRYINDEX, REG_GESTURES);
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, REG_GESTURES);
  }
  lua_pushvalue(L, 2);
  lua_setfield(L, -2, spec);
  lua_pop(L, 1);
  return 0;
}

bool lua_dispatch_gesture(TrixieServer *s, const char *spec) {
  if (!s->L || !spec)
    return false;
  lua_getfield(s->L, LUA_REGISTRYINDEX, REG_GESTURES);
  if (!lua_istable(s->L, -1)) {
    lua_pop(s->L, 1);
    return false;
  }
  lua_getfield(s->L, -1, spec);
  bool handled = false;
  if (lua_isfunction(s->L, -1)) {
    if (lua_pcall(s->L, 0, 0, 0) != LUA_OK) {
      wlr_log(WLR_ERROR, "[lua] gesture '%s': %s", spec,
              lua_tostring(s->L, -1));
      lua_pop(s->L, 1);
    } else {
      handled = true;
    }
  } else {
    lua_pop(s->L, 1);
  }
  lua_pop(s->L, 1);
  return handled;
}

/* ── trixie.exec_once(cmd) ──────────────────────────────────────────────────
 * Spawn cmd once per compositor session — deduplicates across reloads.
 * Uses a registry set so reloads don't re-run autostart commands. */
#define REG_EXEC_ONCE "trixie_exec_once"

static int l_exec_once(lua_State *L) {
  const char *cmd = luaL_checkstring(L, 1);
  TrixieServer *s = get_server(L);
  lua_getfield(L, LUA_REGISTRYINDEX, REG_EXEC_ONCE);
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, REG_EXEC_ONCE);
  }
  /* Check if already run */
  lua_getfield(L, -1, cmd);
  bool already = lua_toboolean(L, -1);
  lua_pop(L, 1);
  if (!already) {
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, cmd);
    server_spawn(s, cmd);
  }
  lua_pop(L, 1);
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
    lua_pushstring(L, c->keyboard.kb_layout);
    return 1;
  }
  if (!strcmp(key, "repeat_rate")) {
    lua_pushinteger(L, c->keyboard.repeat_rate);
    return 1;
  }
  if (!strcmp(key, "repeat_delay")) {
    lua_pushinteger(L, c->keyboard.repeat_delay);
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
  /* Called as trixie.workspace(n) via __call metamethod.
   * __call passes the table as arg 1, so n is at arg 2.
   * Also handle direct call style where n is at arg 1 (e.g. from ws_index). */
  int arg = lua_isnumber(L, 1) ? 1 : (lua_isnumber(L, 2) ? 2 : 0);
  if (arg) {
    int n = (int)lua_tointeger(L, arg);
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
    const char *name = luaL_checkstring(L, 2);
    /* Store the query name — canvas_font_reload resolves via fontconfig */
    strncpy(c->font_path, name, sizeof(c->font_path) - 1);
    bool ok = canvas_font_reload(c->font_path, c->font_path_bold,
                                 c->font_path_italic, c->font_size);
    if (!ok)
      wlr_log(WLR_ERROR, "[lua] trixie.set font: could not load '%s'", name);
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
    strncpy(c->keyboard.kb_layout, luaL_checkstring(L, 2),
            sizeof(c->keyboard.kb_layout) - 1);
  } else if (!strcmp(key, "kb_variant")) {
    strncpy(c->keyboard.kb_variant, luaL_checkstring(L, 2),
            sizeof(c->keyboard.kb_variant) - 1);
  } else if (!strcmp(key, "kb_options")) {
    strncpy(c->keyboard.kb_options, luaL_checkstring(L, 2),
            sizeof(c->keyboard.kb_options) - 1);
  } else if (!strcmp(key, "repeat_rate")) {
    c->keyboard.repeat_rate = (int)luaL_checkinteger(L, 2);
  } else if (!strcmp(key, "repeat_delay")) {
    c->keyboard.repeat_delay = (int)luaL_checkinteger(L, 2);
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
  } else if (!strcmp(key, "saturation")) {
    c->saturation = (float)luaL_checknumber(L, 2);
  } else if (!strcmp(key, "idle_timeout")) {
    c->idle_timeout = (int)luaL_checkinteger(L, 2);
    if (s->idle_timer) {
      s->idle_timeout_ms = c->idle_timeout * 1000;
      wl_event_source_timer_update(
          s->idle_timer, s->idle_timeout_ms > 0 ? s->idle_timeout_ms : 0);
    }
  } else if (!strcmp(key, "monitor")) {
    /* trixie.set("monitor", {name="DP-1", width=2560, height=1440,
     *                         refresh=144, scale=1.0, x=0, y=0}) */
    luaL_checktype(L, 2, LUA_TTABLE);
    char mon_name[64] = {0};
    int mw = 0, mh = 0, mr = 0, mx = 0, my = 0;
    float mscale = 1.0f;
    lua_getfield(L, 2, "name");
    if (lua_isstring(L, -1))
      strncpy(mon_name, lua_tostring(L, -1), sizeof(mon_name) - 1);
    lua_pop(L, 1);
    lua_getfield(L, 2, "width");
    if (lua_isnumber(L, -1))
      mw = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 2, "height");
    if (lua_isnumber(L, -1))
      mh = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 2, "refresh");
    if (lua_isnumber(L, -1))
      mr = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 2, "scale");
    if (lua_isnumber(L, -1))
      mscale = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 2, "x");
    if (lua_isnumber(L, -1))
      mx = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 2, "y");
    if (lua_isnumber(L, -1))
      my = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);

    /* Try to apply immediately if outputs already exist. */
    bool applied = false;
    TrixieOutput *o;
    wl_list_for_each(o, &s->outputs, link) {
      if (!mon_name[0] || !strcmp(o->wlr_output->name, mon_name)) {
        struct wlr_output_state st;
        wlr_output_state_init(&st);
        if (mw > 0 && mh > 0) {
          struct wlr_output_mode *m;
          wl_list_for_each(m, &o->wlr_output->modes, link) {
            int r_mhz = mr > 0 ? mr * 1000 : 0;
            if (m->width == mw && m->height == mh &&
                (r_mhz == 0 || abs(m->refresh - r_mhz) < 500)) {
              wlr_output_state_set_mode(&st, m);
              break;
            }
          }
        }
        if (mscale > 0.1f)
          wlr_output_state_set_scale(&st, mscale);
        wlr_output_commit_state(o->wlr_output, &st);
        wlr_output_state_finish(&st);
        if (mx != 0 || my != 0)
          wlr_output_layout_add(s->output_layout, o->wlr_output, mx, my);
        wlr_log(WLR_INFO, "[lua] monitor '%s' configured", o->wlr_output->name);
        applied = true;
        break;
      }
    }

    if (!applied) {
      /* Outputs not enumerated yet (called before wlr_backend_start).
       * Store the config so lua_apply_pending_monitor() can apply it
       * when handle_new_output fires. */
      wlr_log(WLR_INFO,
              "[lua] monitor '%s': no outputs yet — queuing for later",
              mon_name[0] ? mon_name : "(any)");
      lua_getfield(L, LUA_REGISTRYINDEX, REG_PENDING_MONITORS);
      if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, REG_PENDING_MONITORS);
      }
      /* Store as a table keyed by monitor name (or "" for wildcard).
       * If the same name is set twice the second write wins. */
      lua_newtable(L);
      lua_pushstring(L,  mon_name); lua_setfield(L, -2, "name");
      lua_pushinteger(L, mw);       lua_setfield(L, -2, "width");
      lua_pushinteger(L, mh);       lua_setfield(L, -2, "height");
      lua_pushinteger(L, mr);       lua_setfield(L, -2, "refresh");
      lua_pushnumber(L,  mscale);   lua_setfield(L, -2, "scale");
      lua_pushinteger(L, mx);       lua_setfield(L, -2, "x");
      lua_pushinteger(L, my);       lua_setfield(L, -2, "y");
      lua_setfield(L, -2, mon_name[0] ? mon_name : "");
      lua_pop(L, 1); /* pop pending table */
    }
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
    {"exec_once", l_exec_once},
    /* Scratchpad */
    {"register_scratchpad", l_register_scratchpad},
    /* Input bindings */
    {"key", l_key},
    {"button", l_button},
    {"gesture", l_gesture},
    /* Config */
    {"set", l_set},
    {"get", l_get},
    /* Queries */
    {"focused", l_focused},
    {"clients", l_clients},
    {"clients_all", l_clients_all},
    /* Constructors */
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
  lua_pushcfunction(L, l_layout_register);
  lua_setfield(L, -2, "register");
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

  /* trixie.widget — composable widget primitives */
  push_widget_table(L);
  lua_setfield(L, -2, "widget");

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

  /* Widget */
  luaL_newmetatable(L, MT_WIDGET);
  lua_createtable(L, 0, 3);
  luaL_setfuncs(L, widget_methods, 0);
  lua_setfield(L, -2, "__index");
  lua_pushcfunction(L, lwidget_gc);
  lua_setfield(L, -2, "__gc");
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
    bool match = false;
    bool has_criterion = false;
    if (lua_istable(L, -1)) {
      /* class: substring match against app_id (Hyprland/sway convention) */
      lua_getfield(L, -1, "class");
      if (lua_isstring(L, -1)) {
        has_criterion = true;
        if (strstr(app_id, lua_tostring(L, -1))) match = true;
      }
      lua_pop(L, 1);
      /* name: substring match against title */
      lua_getfield(L, -1, "name");
      if (lua_isstring(L, -1)) {
        has_criterion = true;
        if (strstr(title, lua_tostring(L, -1))) match = true;
      }
      lua_pop(L, 1);
      /* app_id: substring match against app_id */
      lua_getfield(L, -1, "app_id");
      if (lua_isstring(L, -1)) {
        has_criterion = true;
        if (strstr(app_id, lua_tostring(L, -1))) match = true;
      }
      lua_pop(L, 1);
      /* title: substring match against title (explicit) */
      lua_getfield(L, -1, "title");
      if (lua_isstring(L, -1)) {
        has_criterion = true;
        if (strstr(title, lua_tostring(L, -1))) match = true;
      }
      lua_pop(L, 1);
      /* No criteria → catch-all, matches every window */
      if (!has_criterion) match = true;
    } else {
      match = true; /* no rule table → catch-all */
    }
    lua_pop(L, 1); /* rule */

    if (match) {
      wlr_log(WLR_INFO,
              "window_rule[%d]: MATCH app_id='%s' title='%s' — applying properties",
              i, app_id, title);
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
        if (lua_isnumber(L, -1)) {
          int target_ws = (int)lua_tointeger(L, -1) - 1;
          /* Move this specific pane to the target workspace.
           * We cannot use twm_move_to_ws here because that always moves
           * the *focused* pane — at rule-apply time the new pane may not
           * be focused yet. Directly transplant the pane. */
          if (target_ws >= 0 && target_ws < s->twm.ws_count &&
              target_ws != s->twm.active_ws) {
            TwmState *t = &s->twm;
            Workspace *src = &t->workspaces[t->active_ws];
            Workspace *dst = &t->workspaces[target_ws];
            PaneId pid = p->id;
            /* Remove from src */
            for (int _i = 0; _i < src->pane_count; _i++) {
              if (src->panes[_i] == pid) {
                src->panes[_i] = src->panes[--src->pane_count];
                break;
              }
            }
            if (src->has_focused && src->focused == pid) {
              src->has_focused = src->pane_count > 0;
              if (src->has_focused)
                src->focused = src->panes[src->pane_count - 1];
            }
            /* Insert into dst (only if room) */
            if (dst->pane_count < MAX_PANES) {
              dst->panes[dst->pane_count++] = pid;
              p->ws_idx = target_ws;
              wlr_log(WLR_INFO,
                      "rule: moved pane %u ('%s') to workspace %d",
                      pid, p->app_id, target_ws + 1);
            } else {
              wlr_log(WLR_ERROR,
                      "rule: workspace %d full, cannot place pane %u",
                      target_ws, pid);
              /* Put it back in src so it isn't lost */
              if (src->pane_count < MAX_PANES)
                src->panes[src->pane_count++] = pid;
            }
          }
        }
        lua_pop(L, 1);
        lua_getfield(L, -1, "opacity");
        if (lua_isnumber(L, -1))
          p->rule_opacity = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
        /* border_width: store per-pane so it doesn't clobber all windows.
         * Reads back via p->border_w_override in view_apply_geom / deco.
         * A value of -1 means "use global default" (set in Pane memset). */
        lua_getfield(L, -1, "border_width");
        if (lua_isnumber(L, -1))
          p->border_w_override = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "noborder");
        if (lua_isboolean(L, -1) && lua_toboolean(L, -1))
          p->border_w_override = 0;
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

  /* ── Step 4b: clear require() cache for user config modules ─────────────
   * LuaJIT caches required modules in package.loaded. Without clearing it,
   * reload re-runs init.lua but all require() calls return the stale first-
   * load version, so changes to modules/binds.lua etc. never take effect.
   * We clear any key that doesn't start with a known stdlib prefix so
   * built-in modules (string, table, io, …) are preserved. */
  {
    lua_getglobal(L, "package");
    if (lua_istable(L, -1)) {
      lua_getfield(L, -1, "loaded");
      if (lua_istable(L, -1)) {
        /* Collect keys to remove — can't remove while iterating */
        lua_newtable(L); /* to_remove */
        lua_pushnil(L);
        while (lua_next(L, -3) != 0) {
          /* stack: package.loaded, to_remove, key, value */
          if (lua_type(L, -2) == LUA_TSTRING) {
            const char *k = lua_tostring(L, -2);
            /* Keep stdlib and LuaJIT built-ins; remove everything else */
            if (strncmp(k, "string", 6) != 0 &&
                strncmp(k, "table",  5) != 0 &&
                strncmp(k, "io",     2) != 0 &&
                strncmp(k, "os",     2) != 0 &&
                strncmp(k, "math",   4) != 0 &&
                strncmp(k, "bit",    3) != 0 &&
                strncmp(k, "ffi",    3) != 0 &&
                strncmp(k, "jit",    3) != 0 &&
                strncmp(k, "package",7) != 0 &&
                strncmp(k, "coroutine", 9) != 0 &&
                strncmp(k, "debug",  5) != 0) {
              lua_pushvalue(L, -2); /* key copy onto to_remove */
              lua_rawseti(L, -4, (int)lua_objlen(L, -4) + 1);
            }
          }
          lua_pop(L, 1); /* pop value, keep key */
        }
        /* Now remove them */
        int nrem = (int)lua_objlen(L, -1);
        for (int i = 1; i <= nrem; i++) {
          lua_rawgeti(L, -1, i);
          lua_pushnil(L);
          lua_settable(L, -4); /* package.loaded[key] = nil */
        }
        lua_pop(L, 1); /* pop to_remove */
      }
      lua_pop(L, 1); /* pop package.loaded */
    }
    lua_pop(L, 1); /* pop package */
  }

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
  /* Use snprintf instead of strncpy to avoid -Wstringop-truncation:
   * snprintf always NUL-terminates and the compiler knows the bound. */
  snprintf(config_dir, sizeof(config_dir), "%s", path);
  char *last_slash = strrchr(config_dir, '/');
  if (last_slash)
    *last_slash = '\0';

  /* new_path must hold: config_dir/?.lua ; config_dir/?/init.lua ; old_path
   * Worst case: 2 * 512 + overhead + len(old_path).
   * Use 4096 so the compiler can prove no truncation occurs. */
  char new_path[4096];
  snprintf(new_path, sizeof(new_path), "%s/?.lua;%s/?/init.lua;%s", config_dir,
           config_dir, old_path ? old_path : "");

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

/* lua_apply_pending_monitor — called from handle_new_output in main.c.
 * Checks REG_PENDING_MONITORS for a config matching the new output's name
 * (or a wildcard "" entry) and applies it.  Removes the entry afterwards so
 * a second output with the same name doesn't re-apply it. */
void lua_apply_pending_monitor(TrixieServer *s, TrixieOutput *o) {
  lua_State *L = s->L;
  if (!L || !o || !o->wlr_output) return;

  lua_getfield(L, LUA_REGISTRYINDEX, REG_PENDING_MONITORS);
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return;
  }

  /* Try exact name match first, then wildcard "". */
  const char *keys[2] = { o->wlr_output->name, "" };
  for (int ki = 0; ki < 2; ki++) {
    lua_getfield(L, -1, keys[ki]);
    if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      continue;
    }

    lua_getfield(L, -1, "width");   int mw     = (int)lua_tointeger(L, -1); lua_pop(L, 1);
    lua_getfield(L, -1, "height");  int mh     = (int)lua_tointeger(L, -1); lua_pop(L, 1);
    lua_getfield(L, -1, "refresh"); int mr     = (int)lua_tointeger(L, -1); lua_pop(L, 1);
    lua_getfield(L, -1, "scale");   float ms   = (float)lua_tonumber(L, -1); lua_pop(L, 1);
    lua_getfield(L, -1, "x");       int mx     = (int)lua_tointeger(L, -1); lua_pop(L, 1);
    lua_getfield(L, -1, "y");       int my     = (int)lua_tointeger(L, -1); lua_pop(L, 1);
    lua_pop(L, 1); /* pop config table */

    struct wlr_output_state st;
    wlr_output_state_init(&st);
    if (mw > 0 && mh > 0) {
      struct wlr_output_mode *m;
      wl_list_for_each(m, &o->wlr_output->modes, link) {
        int r_mhz = mr > 0 ? mr * 1000 : 0;
        if (m->width == mw && m->height == mh &&
            (r_mhz == 0 || abs(m->refresh - r_mhz) < 500)) {
          wlr_output_state_set_mode(&st, m);
          break;
        }
      }
    }
    if (ms > 0.1f)
      wlr_output_state_set_scale(&st, ms);
    wlr_output_commit_state(o->wlr_output, &st);
    wlr_output_state_finish(&st);
    if (mx != 0 || my != 0)
      wlr_output_layout_add(s->output_layout, o->wlr_output, mx, my);

    wlr_log(WLR_INFO, "[lua] pending monitor config applied to '%s'",
            o->wlr_output->name);

    /* Remove so it doesn't fire again on a second output with the same name. */
    lua_pushnil(L);
    lua_setfield(L, -2, keys[ki]);
    lua_pop(L, 1); /* pop pending table */
    return;
  }

  lua_pop(L, 1); /* pop pending table (no match found) */
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
    /* Null the Lua userdata's pointer BEFORE freeing so lua_close __gc
     * runs ltimer_cancel → sees *ud == NULL → returns immediately.
     * Without this, lua_close GC double-frees and calls wl_event_source_remove
     * on a dangling pointer → SIGSEGV. */
    if (t->ud_ptr) {
      *t->ud_ptr = NULL;
      t->ud_ptr = NULL;
    }
    if (t->src) {
      wl_event_source_remove(t->src);
      t->src = NULL;
    }
    luaL_unref(L, LUA_REGISTRYINDEX, t->fn_ref);
    free(t);
    t = nx;
  }
  g_timers = NULL;
  lua_close(L);
  s->L = NULL;
}
