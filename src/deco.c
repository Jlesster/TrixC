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
  struct wlr_scene_rect *borders[4];
  bool                   has_rects;
  struct wlr_scene_tree *rects_layer; /* which layer borders live on */
  /* Cached state — skip wlr_scene calls when nothing changed. */
  Rect                   last_rect;
  float                  last_color[4];
  bool                   last_enabled;
} DecoEntry;

/* ── TrixieDeco ─────────────────────────────────────────────────────────────── */

struct TrixieDeco {
  struct wlr_scene_tree *layer;
  struct wlr_scene_tree *layer_float;
  DecoEntry              entries[MAX_DECO_PANES];
  int                    count;
  /* Dirty tracking — skip full update when nothing relevant has changed. */
  bool                   dirty;
  uint64_t               last_gen; /* compared against a caller-supplied gen */
  PaneId                 last_focus;
  int                    last_ws;
  int                    last_ws_dx;
};

/* ── Lifecycle ──────────────────────────────────────────────────────────────── */

TrixieDeco *deco_create(struct wlr_scene_tree *layer_tiled,
                        struct wlr_scene_tree *layer_floating) {
  TrixieDeco *d  = calloc(1, sizeof(*d));
  d->layer       = layer_tiled;
  d->layer_float = layer_floating;
  d->dirty       = true; /* force update on first frame */
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
  if(!d) return;
  d->dirty = true;
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

static void ensure_rects(TrixieDeco *d, DecoEntry *e, struct wlr_scene_tree *layer) {
  (void)d;
  if(e->has_rects && e->rects_layer == layer) return; /* correct layer, done */

  /* Wrong layer or not created yet — destroy existing and recreate */
  if(e->has_rects) {
    for(int k = 0; k < 4; k++) {
      if(e->borders[k]) {
        wlr_scene_node_destroy(&e->borders[k]->node);
        e->borders[k] = NULL;
      }
    }
    e->has_rects = false;
  }

  float zero[4] = { 0, 0, 0, 1 };
  for(int k = 0; k < 4; k++) {
    e->borders[k] = wlr_scene_rect_create(layer, 1, 1, zero);
    wlr_scene_node_set_enabled(&e->borders[k]->node, false);
  }
  e->has_rects   = true;
  e->rects_layer = layer;
}

static void hide_entry(DecoEntry *e) {
  if(!e->has_rects) return;
  for(int k = 0; k < 4; k++)
    wlr_scene_node_set_enabled(&e->borders[k]->node, false);
}

static void position_entry(
    TrixieDeco *d, DecoEntry *e, Rect r, int bw, bool focused, const Config *cfg) {
  (void)d;
  Color col;
  if(focused) {
    col = cfg->colors.active_border;
  } else {
    Color ib = cfg->colors.inactive_border;
    Color pb = cfg->colors.pane_bg;
    col.r    = (uint8_t)(((int)ib.r + (int)pb.r) / 2);
    col.g    = (uint8_t)(((int)ib.g + (int)pb.g) / 2);
    col.b    = (uint8_t)(((int)ib.b + (int)pb.b) / 2);
    col.a    = 0xff;
  }
  float fc[4];
  color_f(col, fc);

  /* Skip all wlr_scene calls when geometry and colour are unchanged. */
  bool geom_same  = (r.x == e->last_rect.x && r.y == e->last_rect.y &&
                    r.w == e->last_rect.w && r.h == e->last_rect.h);
  bool color_same = (fc[0] == e->last_color[0] && fc[1] == e->last_color[1] &&
                     fc[2] == e->last_color[2] && fc[3] == e->last_color[3]);
  if(geom_same && color_same && e->last_enabled) return;

  e->last_rect     = r;
  e->last_color[0] = fc[0];
  e->last_color[1] = fc[1];
  e->last_color[2] = fc[2];
  e->last_color[3] = fc[3];
  e->last_enabled  = true;

  wlr_scene_rect_set_size(e->borders[0], r.w, bw);
  wlr_scene_node_set_position(&e->borders[0]->node, r.x, r.y);
  wlr_scene_rect_set_color(e->borders[0], fc);
  wlr_scene_node_set_enabled(&e->borders[0]->node, true);

  wlr_scene_rect_set_size(e->borders[1], r.w, bw);
  wlr_scene_node_set_position(&e->borders[1]->node, r.x, r.y + r.h - bw);
  wlr_scene_rect_set_color(e->borders[1], fc);
  wlr_scene_node_set_enabled(&e->borders[1]->node, true);

  wlr_scene_rect_set_size(e->borders[2], bw, r.h);
  wlr_scene_node_set_position(&e->borders[2]->node, r.x, r.y);
  wlr_scene_rect_set_color(e->borders[2], fc);
  wlr_scene_node_set_enabled(&e->borders[2]->node, true);

  wlr_scene_rect_set_size(e->borders[3], bw, r.h);
  wlr_scene_node_set_position(&e->borders[3]->node, r.x + r.w - bw, r.y);
  wlr_scene_rect_set_color(e->borders[3], fc);
  wlr_scene_node_set_enabled(&e->borders[3]->node, true);
}

void deco_update(TrixieDeco *d, TwmState *twm, AnimSet *anim, const Config *cfg) {
  if(!d || !twm) return;

  int    bw         = cfg->border_width;
  PaneId focused_id = twm_focused_id(twm);
  int    ws_dx      = anim_ws_incoming_x(anim);
  bool   animating  = anim_any(anim);

  /* Skip the full update when nothing has changed.
   * deco_mark_dirty() is called on focus change, layout change, ws switch,
   * config reload, and open/close.  During animations we must update every
   * frame (positions change).  When still, we only re-run if dirty. */
  if(!animating && !d->dirty && focused_id == d->last_focus &&
     twm->active_ws == d->last_ws && ws_dx == d->last_ws_dx)
    return;

  d->dirty      = false;
  d->last_focus = focused_id;
  d->last_ws    = twm->active_ws;
  d->last_ws_dx = ws_dx;

  Workspace *ws = &twm->workspaces[twm->active_ws];

  for(int i = 0; i < d->count; i++)
    d->entries[i].active = false;

  /* Precompute noborder flags once for all win-rules (O(n+m) instead of O(n*m)). */
  bool noborder_map[MAX_DECO_PANES] = { false };
  for(int i = 0; i < ws->pane_count && i < MAX_DECO_PANES; i++) {
    PaneId pid = ws->panes[i];
    Pane  *p   = twm_pane_by_id(twm, pid);
    if(!p) continue;
    for(int ri = 0; ri < cfg->win_rule_count; ri++) {
      const WinRule *wr = &cfg->win_rules[ri];
      if(!wr->app_id[0] || !wr->noborder) continue;
      bool match = false;
      if(!strncmp(wr->app_id, "title:", 6))
        match = strstr(p->title, wr->app_id + 6) != NULL;
      else
        match = strstr(p->app_id, wr->app_id) != NULL;
      if(match) {
        noborder_map[i] = true;
        break;
      }
    }
  }

  for(int i = 0; i < ws->pane_count; i++) {
    PaneId pid = ws->panes[i];
    Pane  *p   = twm_pane_by_id(twm, pid);
    if(!p) continue;

    bool noborder = noborder_map[i];

    if(p->fullscreen) {
      DecoEntry *e = deco_find(d, pid);
      if(e) {
        hide_entry(e);
        e->active = true;
      }
      continue;
    }

    if(p->floating) {
      int        fbw     = bw < 2 ? 2 : bw;
      Rect       r       = anim_get_rect(anim, pid, p->rect);
      Rect       shifted = { r.x + ws_dx, r.y, r.w, r.h };
      bool       focused = (pid == focused_id);
      DecoEntry *e       = deco_get_or_create(d, pid);
      if(!e) continue;
      e->active = true;
      /* Floating borders go on layer_float — above floating window content */
      ensure_rects(d, e, d->layer_float);

      Color col = focused ? cfg->colors.active_border : cfg->colors.inactive_border;
      float fc[4];
      color_f(col, fc);
      float opacity = anim_get_opacity(anim, pid, 1.0f);
      fc[3] *= opacity;

      wlr_scene_rect_set_size(e->borders[0], shifted.w, fbw);
      wlr_scene_node_set_position(&e->borders[0]->node, shifted.x, shifted.y);
      wlr_scene_rect_set_color(e->borders[0], fc);
      wlr_scene_node_set_enabled(&e->borders[0]->node, true);

      wlr_scene_rect_set_size(e->borders[1], shifted.w, fbw);
      wlr_scene_node_set_position(
          &e->borders[1]->node, shifted.x, shifted.y + shifted.h - fbw);
      wlr_scene_rect_set_color(e->borders[1], fc);
      wlr_scene_node_set_enabled(&e->borders[1]->node, true);

      wlr_scene_rect_set_size(e->borders[2], fbw, shifted.h);
      wlr_scene_node_set_position(&e->borders[2]->node, shifted.x, shifted.y);
      wlr_scene_rect_set_color(e->borders[2], fc);
      wlr_scene_node_set_enabled(&e->borders[2]->node, true);

      wlr_scene_rect_set_size(e->borders[3], fbw, shifted.h);
      wlr_scene_node_set_position(
          &e->borders[3]->node, shifted.x + shifted.w - fbw, shifted.y);
      wlr_scene_rect_set_color(e->borders[3], fc);
      wlr_scene_node_set_enabled(&e->borders[3]->node, true);
      continue;
    }

    if(bw <= 0 || noborder) {
      DecoEntry *e = deco_find(d, pid);
      if(e) {
        hide_entry(e);
        e->active = true;
      }
      continue;
    }

    Rect r       = anim_get_rect(anim, pid, p->rect);
    Rect shifted = { r.x + ws_dx, r.y, r.w, r.h };
    bool focused = (pid == focused_id);

    DecoEntry *e = deco_get_or_create(d, pid);
    if(!e) continue;
    e->active = true;
    /* Tiled borders go on layer_chrome — above tiled windows, below floating */
    ensure_rects(d, e, d->layer);
    position_entry(d, e, shifted, bw, focused, cfg);
  }

  for(int i = 0; i < d->count; i++)
    if(!d->entries[i].active) hide_entry(&d->entries[i]);
}
