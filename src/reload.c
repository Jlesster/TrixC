/* reload.c — live reload of config, fonts, and UI in Trixie */
#include "trixie.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void reload_fonts(TrixieServer *s) {
  canvas_font_reload(s->cfg.font_path, s->cfg.font_path_bold,
                     s->cfg.font_path_italic, s->cfg.font_size);
  /* wibox reset handled by lua_reload step 0 before init.lua runs */
}

static void recompute_layouts(TrixieServer *s) {
  for (int i = 0; i < s->twm.ws_count; i++) {
    Workspace *ws = &s->twm.workspaces[i];
    dwindle_recompute(&ws->dwindle, s->twm.content_rect, s->twm.gap);
  }
  twm_reflow(&s->twm);
}

static void refresh_ui(TrixieServer *s) {
  TrixieOutput *o;
  wl_list_for_each(o, &s->outputs, link) {
    if (o->bar)
      bar_mark_dirty(o->bar);
    for (int i = 0; i < o->wibox_count; i++) {
      if (o->wiboxes[i])
        wibox_mark_dirty(o->wiboxes[i]);
    }
  }
}

void reload_config(TrixieServer *s) {
  /* 1. Reset cfg defaults, run lua_reload (init.lua writes new values into
   *    s->cfg via trixie.set), reflow windows. */
  server_apply_config_reload(s);

  /* 2. Bust all deco cache fields (last_rect, last_text, last_top/side) so
   *    every entry unconditionally redraws on the next frame. */
  TrixieOutput *o;
  wl_list_for_each(o, &s->outputs, link) {
    if (o->deco)
      deco_complete_update(o->deco, &s->twm, &s->anim, &s->cfg);
  }

  /* 3. Mark deco dirty on all outputs so the render loop re-runs deco_update
   *    on the next frame with the live s->cfg — this is what actually pushes
   *    the new border colours/widths into the scene graph. */
  server_mark_deco_dirty(s);

  reload_fonts(s);
  recompute_layouts(s);
  refresh_ui(s);
  server_request_redraw(s);
}
