#include "trixie.h"

/* Lua binding: reload() global
 * Routes through server_schedule_reload() so the call is deferred via the
 * event loop idle callback — same path as trixie.reload() and IPC "reload".
 * The actual work is done by reload_config() which server_schedule_reload
 * ultimately calls via server_apply_config_reload, giving us the full
 * pipeline: config reset → lua_reload → deco_update → fonts → layouts → UI. */
static int l_trixie_reload(lua_State *L) {
  TrixieServer *s = lua_touserdata(L, lua_upvalueindex(1));
  server_schedule_reload(s);
  return 0;
}

void lua_register_reload(TrixieServer *s) {
  lua_pushlightuserdata(s->L, s);
  lua_pushcclosure(s->L, l_trixie_reload, 1);
  lua_setglobal(s->L, "reload");
}
