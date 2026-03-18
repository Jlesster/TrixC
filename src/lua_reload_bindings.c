#include "trixie.h"

/* Lua binding: trixie.reload() */
static int l_trixie_reload(lua_State *L) {
  TrixieServer *s = lua_touserdata(L, lua_upvalueindex(1));
  reload_config(s);
  return 0;
}

/* Register reload() in Lua */
void lua_register_reload(TrixieServer *s) {
  lua_pushlightuserdata(s->L, s);
  lua_pushcclosure(s->L, l_trixie_reload, 1);
  lua_setglobal(s->L, "reload");
}
