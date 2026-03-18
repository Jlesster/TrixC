/* reload.c — live reload of config, fonts, and UI in Trixie */
#include "trixie.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ── Helpers ───────────────────────────────────────────────────────────── */

/* Reload canvas fonts and reset wiboxes for all outputs */
static void reload_fonts(TrixieServer *s) {
  canvas_font_reload(s->cfg.font_path, s->cfg.font_path_bold,
                     s->cfg.font_path_italic, s->cfg.font_size);

  TrixieOutput *o;
  wl_list_for_each(o, &s->outputs, link) {
    for (int i = 0; i < o->wibox_count; i++) {
      if (!o->wiboxes[i])
        continue;
      wibox_clear_output(o);
      wibox_reset_output(o, s->L);
    }
  }
}

/* Recompute dwindle trees and layouts for all workspaces */
static void recompute_layouts(TrixieServer *s) {
  for (int i = 0; i < s->twm.ws_count; i++) {
    Workspace *ws = &s->twm.workspaces[i];
    dwindle_recompute(&ws->dwindle, s->twm.content_rect, s->twm.gap);
  }
  twm_reflow(&s->twm);
}

/* Refresh bars and wiboxes */
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

/* ── Public reload function ────────────────────────────────────────────── */
void reload_config(TrixieServer *s) {
  lua_reload(s);

  TrixieOutput *o;
  wl_list_for_each(o, &s->outputs, link) {
    if (o->deco) {
      deco_complete_update(o->deco, &s->twm, &s->anim, &s->cfg);
    }
  }

  reload_fonts(s);
  recompute_layouts(s);
  refresh_ui(s);
  server_request_redraw(s);

  fprintf(
      stderr,
      "[reload] configuration reloaded — UI, decorations, and bars updated\n");
}
