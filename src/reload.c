/* reload.c — live reload of config, fonts, and UI in Trixie */
#include "trixie.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void recompute_layouts(TrixieServer *s) {
  /* Rebuild dwindle BSP trees for all workspaces with the new content_rect.
   * Do NOT call twm_reflow here — server_apply_config_reload already did. */
  for (int i = 0; i < s->twm.ws_count; i++) {
    Workspace *ws = &s->twm.workspaces[i];
    dwindle_recompute(&ws->dwindle, s->twm.content_rect, s->twm.gap);
  }
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
  /* 1. Reset cfg defaults and run init.lua.
   *    canvas_font_reload is called once here with defaults so any wibox draw
   *    callbacks invoked during lua_reload (init.lua) use a valid font face.
   *    server_apply_config_reload → lua_reload may update s->cfg.font_path via
   *    trixie.set("font", ...), so we call canvas_font_reload again afterwards
   *    to pick up the final font choice. */
  canvas_font_reload(s->cfg.font_path, s->cfg.font_path_bold,
                     s->cfg.font_path_italic, s->cfg.font_size);
  server_apply_config_reload(s);

  /* 2. Re-apply fonts with whatever init.lua settled on (no-op if unchanged).
   */
  canvas_font_reload(s->cfg.font_path, s->cfg.font_path_bold,
                     s->cfg.font_path_italic, s->cfg.font_size);

  /* 3. Bust all deco cache fields (last_rect, last_text, last_top/side) so
   *    every entry unconditionally redraws on the next frame. */
  TrixieOutput *o;
  wl_list_for_each(o, &s->outputs, link) {
    if (o->deco)
      deco_complete_update(o->deco, &s->twm, &s->anim, &s->cfg);
  }

  /* 4. Mark deco dirty on all outputs so the render loop re-runs deco_update
   *    on the next frame with the live s->cfg — pushes new border
   * colours/widths into the scene graph. */
  server_mark_deco_dirty(s);

  recompute_layouts(s);
  refresh_ui(s);
  server_request_redraw(s);
}
