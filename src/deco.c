/* deco.c — Window border decorations using wlr_scene rects.
 *
 * Each tiled pane gets 4 border rects (top/bottom/left/right, border_width
 * thick).  No titlebar, no text — just a clean coloured outline.
 *
 * noborder window rules are respected per-pane.
 * Active / inactive border colours come from cfg->colors.
 */
#include "trixie.h"
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_scene.h>

#define MAX_DECO_PANES 256

/* ── DecoEntry ──────────────────────────────────────────────────────────────── */

typedef struct {
  PaneId                 id;
  bool                   active;
  struct wlr_scene_rect *borders[4]; /* top, bottom, left, right */
  bool                   has_rects;
} DecoEntry;

/* ── TrixieDeco ─────────────────────────────────────────────────────────────── */

struct TrixieDeco {
  struct wlr_scene_tree *layer;
  DecoEntry              entries[MAX_DECO_PANES];
  int                    count;
};

/* ── Lifecycle ──────────────────────────────────────────────────────────────── */

TrixieDeco *deco_create(struct wlr_scene_tree *layer) {
  TrixieDeco *d = calloc(1, sizeof(*d));
  d->layer      = layer;
  return d;
}

void deco_destroy(TrixieDeco *d) {
  if(!d) return;
  for(int i = 0; i < d->count; i++) {
    DecoEntry *e = &d->entries[i];
    for(int k = 0; k < 4; k++)
      if(e->borders[k]) wlr_scene_node_destroy(&e->borders[k]->node);
  }
  free(d);
}

void deco_mark_dirty(TrixieDeco *d) {
  /* Force colour refresh on next deco_update by clearing active flags. */
  if(!d) return;
  for(int i = 0; i < d->count; i++)
    d->entries[i].active = false;
}

/* ── Internal helpers ───────────────────────────────────────────────────────── */

static DecoEntry *deco_find(TrixieDeco *d, PaneId id) {
  for(int i = 0; i < d->count; i++)
    if(d->entries[i].id == id) return &d->entries[i];
  return NULL;
}

static DecoEntry *deco_get_or_create(TrixieDeco *d, PaneId id) {
  DecoEntry *e = deco_find(d, id);
  if(e) return e;
  if(d->count >= MAX_DECO_PANES) return NULL;
  e = &d->entries[d->count++];
  memset(e, 0, sizeof(*e));
  e->id = id;
  return e;
}

static void color_f(Color c, float out[4]) {
  out[0] = c.r / 255.0f;
  out[1] = c.g / 255.0f;
  out[2] = c.b / 255.0f;
  out[3] = c.a / 255.0f;
}

static void ensure_rects(TrixieDeco *d, DecoEntry *e) {
  if(e->has_rects) return;
  float zero[4] = { 0, 0, 0, 1 };
  for(int k = 0; k < 4; k++) {
    e->borders[k] = wlr_scene_rect_create(d->layer, 1, 1, zero);
    wlr_scene_node_set_enabled(&e->borders[k]->node, false);
  }
  e->has_rects = true;
}

static void hide_entry(DecoEntry *e) {
  if(!e->has_rects) return;
  for(int k = 0; k < 4; k++)
    wlr_scene_node_set_enabled(&e->borders[k]->node, false);
}

/* Position and colour the four border rects around `r`.
 *
 * Visual intent: borders should read like tmux pane dividers — a single pixel
 * line, not a frame.  Active border uses the accent colour so the focused pane
 * is immediately obvious without any glow or shadow.  Inactive borders are
 * rendered at near-background brightness so they recede rather than compete.
 *
 * border_width is always respected from config; at 1px this looks exactly like
 * a terminal split line.  At 2px it reads as a thin frame.  No rounding ever.
 */
static void position_entry(
    TrixieDeco *d, DecoEntry *e, Rect r, int bw, bool focused, const Config *cfg) {
  Color col;
  if(focused) {
    col = cfg->colors.active_border;
  } else {
    /* Inactive: blend the configured inactive border 50% toward background
     * so unfocused panes truly recede.  This gives a tmux-like effect where
     * only the active pane has a visible outline.                           */
    Color ib = cfg->colors.inactive_border;
    Color pb = cfg->colors.pane_bg;
    col.r    = (uint8_t)(((int)ib.r + (int)pb.r) / 2);
    col.g    = (uint8_t)(((int)ib.g + (int)pb.g) / 2);
    col.b    = (uint8_t)(((int)ib.b + (int)pb.b) / 2);
    col.a    = 0xff;
  }
  float fc[4];
  color_f(col, fc);

  /* top */
  wlr_scene_rect_set_size(e->borders[0], r.w, bw);
  wlr_scene_node_set_position(&e->borders[0]->node, r.x, r.y);
  wlr_scene_rect_set_color(e->borders[0], fc);
  wlr_scene_node_set_enabled(&e->borders[0]->node, true);

  /* bottom */
  wlr_scene_rect_set_size(e->borders[1], r.w, bw);
  wlr_scene_node_set_position(&e->borders[1]->node, r.x, r.y + r.h - bw);
  wlr_scene_rect_set_color(e->borders[1], fc);
  wlr_scene_node_set_enabled(&e->borders[1]->node, true);

  /* left */
  wlr_scene_rect_set_size(e->borders[2], bw, r.h);
  wlr_scene_node_set_position(&e->borders[2]->node, r.x, r.y);
  wlr_scene_rect_set_color(e->borders[2], fc);
  wlr_scene_node_set_enabled(&e->borders[2]->node, true);

  /* right */
  wlr_scene_rect_set_size(e->borders[3], bw, r.h);
  wlr_scene_node_set_position(&e->borders[3]->node, r.x + r.w - bw, r.y);
  wlr_scene_rect_set_color(e->borders[3], fc);
  wlr_scene_node_set_enabled(&e->borders[3]->node, true);

  (void)d;
}

/* ── deco_update ────────────────────────────────────────────────────────────── */

void deco_update(TrixieDeco *d, TwmState *twm, AnimSet *anim, const Config *cfg) {
  if(!d || !twm) return;

  int        bw         = cfg->border_width;
  Workspace *ws         = &twm->workspaces[twm->active_ws];
  PaneId     focused_id = twm_focused_id(twm);

  /*
   * Workspace slide offset — must be added to every border x position so the
   * chrome travels with its window during workspace transitions.
   * anim_ws_incoming_x() returns 0 when no workspace animation is active, so
   * this is always safe to apply unconditionally.
   */
  int ws_dx = anim_ws_incoming_x(anim);

  for(int i = 0; i < d->count; i++)
    d->entries[i].active = false;

  for(int i = 0; i < ws->pane_count; i++) {
    PaneId pid = ws->panes[i];
    Pane  *p   = twm_pane_by_id(twm, pid);
    if(!p) continue;

    /* Check per-pane noborder window rule. */
    bool noborder = false;
    for(int ri = 0; ri < cfg->win_rule_count; ri++) {
      const WinRule *wr = &cfg->win_rules[ri];
      if(!wr->app_id[0]) continue;
      bool match = false;
      if(!strncmp(wr->app_id, "title:", 6))
        match = strstr(p->title, wr->app_id + 6) != NULL;
      else
        match = strstr(p->app_id, wr->app_id) != NULL;
      if(match && wr->noborder) {
        noborder = true;
        break;
      }
    }

    if(p->floating || p->fullscreen || bw <= 0 || noborder) {
      DecoEntry *e = deco_find(d, pid);
      if(p->fullscreen) {
        /* Fullscreen: hide all chrome. */
        if(e) {
          hide_entry(e);
          e->active = true;
        }
        continue;
      }
      if(p->floating) {
        /* Floating windows get a thin accent border so they read as elevated
         * above the tiled layer.  We use a fixed 2px frame (or border_width
         * if configured larger) with the active/inactive border colour —
         * no blend-to-bg dimming, since floats are already visually separated
         * by the scale/fade open animation. */
        int  fbw     = bw < 2 ? 2 : bw;
        Rect r       = anim_get_rect(anim, pid, p->rect);
        Rect shifted = { r.x + ws_dx, r.y, r.w, r.h };
        bool focused = (pid == focused_id);
        e            = deco_get_or_create(d, pid);
        if(!e) continue;
        e->active = true;
        ensure_rects(d, e);
        /* Use direct border colours for floats — no bg blend. */
        Color col =
            focused ? cfg->colors.active_border : cfg->colors.inactive_border;
        float fc[4];
        color_f(col, fc);
        /* top */
        wlr_scene_rect_set_size(e->borders[0], shifted.w, fbw);
        wlr_scene_node_set_position(&e->borders[0]->node, shifted.x, shifted.y);
        wlr_scene_rect_set_color(e->borders[0], fc);
        wlr_scene_node_set_enabled(&e->borders[0]->node, true);
        /* bottom */
        wlr_scene_rect_set_size(e->borders[1], shifted.w, fbw);
        wlr_scene_node_set_position(
            &e->borders[1]->node, shifted.x, shifted.y + shifted.h - fbw);
        wlr_scene_rect_set_color(e->borders[1], fc);
        wlr_scene_node_set_enabled(&e->borders[1]->node, true);
        /* left */
        wlr_scene_rect_set_size(e->borders[2], fbw, shifted.h);
        wlr_scene_node_set_position(&e->borders[2]->node, shifted.x, shifted.y);
        wlr_scene_rect_set_color(e->borders[2], fc);
        wlr_scene_node_set_enabled(&e->borders[2]->node, true);
        /* right */
        wlr_scene_rect_set_size(e->borders[3], fbw, shifted.h);
        wlr_scene_node_set_position(
            &e->borders[3]->node, shifted.x + shifted.w - fbw, shifted.y);
        wlr_scene_rect_set_color(e->borders[3], fc);
        wlr_scene_node_set_enabled(&e->borders[3]->node, true);
        continue;
      }
      /* noborder tiled — hide. */
      if(e) {
        hide_entry(e);
        e->active = true;
      }
      continue;
    }

    Rect r = anim_get_rect(anim, pid, p->rect);

    /*
     * Apply the workspace slide offset so borders travel with their windows
     * during workspace transitions.
     */
    Rect shifted = { r.x + ws_dx, r.y, r.w, r.h };

    bool focused = (pid == focused_id);

    DecoEntry *e = deco_get_or_create(d, pid);
    if(!e) continue;
    e->active = true;
    ensure_rects(d, e);
    position_entry(d, e, shifted, bw, focused, cfg);
  }

  /* Hide entries no longer on the active workspace. */
  for(int i = 0; i < d->count; i++)
    if(!d->entries[i].active) hide_entry(&d->entries[i]);
}
