/* deco.c — four plain wlr_scene_rect per pane + a tight text label.
 *
 * The label buffer is exactly as wide as the rendered text and as tall as
 * the font, centered vertically on the top border line.  pane_bg fill so
 * it punches through the border colour cleanly.  No full-width overlay.
 */
#include "trixie.h"
#include <drm_fourcc.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>

/* ── bar font bridge ────────────────────────────────────────────────────── */
int bar_measure_text(const char *text);
int bar_draw_text_pub(uint32_t *px, int stride, int x, int y, const char *text,
                      Color fg, int clip_w, int clip_h);
int bar_font_ascender(void);
int bar_font_height(void);

/* ── minimal wlr_buffer ─────────────────────────────────────────────────── */
struct TBuf {
  struct wlr_buffer base;
  uint32_t *data;
  int w, h;
};
static void tbuf_destroy(struct wlr_buffer *b) {
  struct TBuf *t = wl_container_of(b, t, base);
  free(t->data);
  free(t);
}
static bool tbuf_begin(struct wlr_buffer *b, uint32_t flags, void **data,
                       uint32_t *fmt, size_t *stride) {
  (void)flags;
  struct TBuf *t = wl_container_of(b, t, base);
  *data = t->data;
  *fmt = DRM_FORMAT_ARGB8888;
  *stride = (size_t)(t->w * 4);
  return true;
}
static void tbuf_end(struct wlr_buffer *b) { (void)b; }
static const struct wlr_buffer_impl tbuf_impl = {
    .destroy = tbuf_destroy,
    .begin_data_ptr_access = tbuf_begin,
    .end_data_ptr_access = tbuf_end,
};
static struct TBuf *tbuf_create(int w, int h) {
  struct TBuf *t = calloc(1, sizeof(*t));
  t->data = calloc((size_t)(w * h), 4);
  t->w = w;
  t->h = h;
  wlr_buffer_init(&t->base, &tbuf_impl, w, h);
  return t;
}

/* ── DecoEntry ──────────────────────────────────────────────────────────── */
#define MAX_DECO_PANES 256
#define R_TOP 0
#define R_LEFT 1
#define R_RIGHT 2
#define R_BOT 3
#define LABEL_PAD_H 6 /* px gap between label and window left edge */

typedef struct {
  PaneId id;
  bool active;

  struct wlr_scene_rect *borders[4];
  struct wlr_scene_tree *layer;
  bool has_rects;

  struct wlr_scene_buffer *label;

  Rect last_rect;
  float last_top[4];
  float last_side[4];
  bool last_focused;
  bool last_enabled;
  char last_text[320]; /* cached label string for dirty check */
  int last_label_x;    /* cached label node position for animation tracking */
  int last_label_y;
} DecoEntry;

struct TrixieDeco {
  struct wlr_scene_tree *layer_tiled;
  struct wlr_scene_tree *layer_float;
  DecoEntry entries[MAX_DECO_PANES];
  int count;
  bool dirty;
  PaneId last_focus;
  int last_ws;
  int last_ws_dx;
};

/* ── helpers ────────────────────────────────────────────────────────────── */
static void color_f(Color c, float out[4]) {
  out[0] = c.r / 255.f;
  out[1] = c.g / 255.f;
  out[2] = c.b / 255.f;
  out[3] = c.a / 255.f;
}
static bool color_eq(const float a[4], const float b[4]) {
  return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}
static DecoEntry *deco_find(TrixieDeco *d, PaneId id) {
  for (int i = 0; i < d->count; i++)
    if (d->entries[i].id == id)
      return &d->entries[i];
  return NULL;
}
static DecoEntry *deco_get_or_create(TrixieDeco *d, PaneId id) {
  DecoEntry *e = deco_find(d, id);
  if (e)
    return e;
  if (d->count >= MAX_DECO_PANES)
    return NULL;
  e = &d->entries[d->count++];
  memset(e, 0, sizeof(*e));
  e->id = id;
  return e;
}

/* ── rect lifecycle ─────────────────────────────────────────────────────── */
static void destroy_rects(DecoEntry *e) {
  if (!e->has_rects)
    return;
  for (int k = 0; k < 4; k++)
    if (e->borders[k]) {
      wlr_scene_node_destroy(&e->borders[k]->node);
      e->borders[k] = NULL;
    }
  e->has_rects = false;
  e->layer = NULL;
}
static void ensure_rects(DecoEntry *e, struct wlr_scene_tree *layer) {
  if (e->has_rects && e->layer == layer)
    return;
  destroy_rects(e);
  if (e->label) {
    wlr_scene_node_destroy(&e->label->node);
    e->label = NULL;
  }
  static const float zero[4] = {0};
  for (int k = 0; k < 4; k++)
    e->borders[k] = wlr_scene_rect_create(layer, 1, 1, zero);
  e->has_rects = true;
  e->layer = layer;
}
static void hide_entry(DecoEntry *e) {
  if (e->has_rects)
    for (int k = 0; k < 4; k++)
      wlr_scene_node_set_enabled(&e->borders[k]->node, false);
  if (e->label)
    wlr_scene_node_set_enabled(&e->label->node, false);
  e->last_enabled = false;
}

/* ── border colours ─────────────────────────────────────────────────────── */
static void top_color(bool focused, const Config *cfg, float out[4]) {
  if (focused) {
    color_f(cfg->colors.active_border, out);
    return;
  }
  Color ib = cfg->colors.inactive_border, pb = cfg->colors.pane_bg;
  Color c = {.r = (uint8_t)(((int)ib.r + (int)pb.r) / 2),
             .g = (uint8_t)(((int)ib.g + (int)pb.g) / 2),
             .b = (uint8_t)(((int)ib.b + (int)pb.b) / 2),
             .a = 0xff};
  color_f(c, out);
}

/* ── rect positioning ───────────────────────────────────────────────────── */
static void position_rects(DecoEntry *e, Rect r, int bw, float tc[4],
                           float sc[4]) {
  bool gs = r.x == e->last_rect.x && r.y == e->last_rect.y &&
            r.w == e->last_rect.w && r.h == e->last_rect.h;
  bool cs = color_eq(tc, e->last_top) && color_eq(sc, e->last_side);
  if (gs && cs && e->last_enabled)
    return;
  e->last_rect = r;
  memcpy(e->last_top, tc, sizeof(e->last_top));
  memcpy(e->last_side, sc, sizeof(e->last_side));
  e->last_enabled = true;
  int ix = r.x + bw, iw = r.w - bw * 2, iy = r.y + bw, ih = r.h - bw;
  wlr_scene_rect_set_size(e->borders[R_TOP], r.w, bw);
  wlr_scene_node_set_position(&e->borders[R_TOP]->node, r.x, r.y);
  wlr_scene_rect_set_color(e->borders[R_TOP], tc);
  wlr_scene_node_set_enabled(&e->borders[R_TOP]->node, true);
  wlr_scene_rect_set_size(e->borders[R_LEFT], bw, ih);
  wlr_scene_node_set_position(&e->borders[R_LEFT]->node, r.x, iy);
  wlr_scene_rect_set_color(e->borders[R_LEFT], sc);
  wlr_scene_node_set_enabled(&e->borders[R_LEFT]->node, true);
  wlr_scene_rect_set_size(e->borders[R_RIGHT], bw, ih);
  wlr_scene_node_set_position(&e->borders[R_RIGHT]->node, r.x + r.w - bw, iy);
  wlr_scene_rect_set_color(e->borders[R_RIGHT], sc);
  wlr_scene_node_set_enabled(&e->borders[R_RIGHT]->node, true);
  wlr_scene_rect_set_size(e->borders[R_BOT], iw, bw);
  wlr_scene_node_set_position(&e->borders[R_BOT]->node, ix, r.y + r.h - bw);
  wlr_scene_rect_set_color(e->borders[R_BOT], sc);
  wlr_scene_node_set_enabled(&e->borders[R_BOT]->node, true);
}

/* ── label upload ───────────────────────────────────────────────────────── */
static void update_label(DecoEntry *e, struct wlr_scene_tree *layer, Rect r,
                         int bw, int ws_idx, bool focused, bool floating,
                         const Config *cfg, const Pane *p)
    __attribute__((unused));
static void update_label(DecoEntry *e, struct wlr_scene_tree *layer, Rect r,
                         int bw, int ws_idx, bool focused, bool floating,
                         const Config *cfg, const Pane *p) {
  int fh = bar_font_height();
  if (fh <= 0)
    return; /* font not ready */

  /* Build label string */
  char text[320];
  const char *name = (p->app_id[0])  ? p->app_id
                     : (p->title[0]) ? p->title
                                     : "?";
  static const char *ws_shapes[] = {"●", "✚", "▲", "■", "⬟",
                                    "⬡", "✦", "★", "⬛"};
  int ws_i = (ws_idx >= 0 && ws_idx < 9) ? ws_idx : 0;
  const char *shape = floating ? "~" : ws_shapes[ws_i];
  snprintf(text, sizeof(text), "[%s %s]", name, shape);

  /* Measure — buffer is exactly this wide */
  int tw = bar_measure_text(text);
  if (tw <= 0)
    return;

  bool changed = strcmp(text, e->last_text) != 0 || focused != e->last_focused;

  /* Compute label position first so we can check it independently */
  int lx = r.x + LABEL_PAD_H;
  int ly = r.y - (fh - bw) / 2;
  bool moved = (lx != e->last_label_x || ly != e->last_label_y);

  if (!changed && !moved && e->last_enabled && e->label)
    return;

  if (changed) {
    strncpy(e->last_text, text, sizeof(e->last_text) - 1);
    e->last_focused = focused;
  }

  /* Create label buffer node after rects (paints on top) */
  if (!e->label)
    e->label = wlr_scene_buffer_create(layer, NULL);

  if (changed) {
    Color pb = cfg->colors.background;
    Color fg =
        focused ? cfg->colors.active_border : cfg->colors.inactive_border;

    struct TBuf *tb = tbuf_create(tw, fh);
    /* Fill with pane_bg — punches cleanly through border colour */
    uint32_t bg32 =
        (0xffu << 24) | ((uint32_t)pb.r << 16) | ((uint32_t)pb.g << 8) | pb.b;
    for (int i = 0; i < tw * fh; i++)
      tb->data[i] = bg32;

    int asc = bar_font_ascender();
    bar_draw_text_pub(tb->data, tw, 0, asc, text, fg, tw, fh);

    wlr_scene_buffer_set_buffer(e->label, &tb->base);
    wlr_buffer_drop(&tb->base);
  }

  /* Center vertically on the top border line — always reposition to follow
   * anims */
  e->last_label_x = lx;
  e->last_label_y = ly;
  wlr_scene_node_set_position(&e->label->node, lx, ly);
  wlr_scene_node_set_enabled(&e->label->node, true);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Public API
 * ══════════════════════════════════════════════════════════════════════════ */
TrixieDeco *deco_create(struct wlr_scene_tree *layer_tiled,
                        struct wlr_scene_tree *layer_floating) {
  TrixieDeco *d = calloc(1, sizeof(*d));
  d->layer_tiled = layer_tiled;
  d->layer_float = layer_floating;
  d->dirty = true;
  return d;
}
void deco_destroy(TrixieDeco *d) {
  if (!d)
    return;
  for (int i = 0; i < d->count; i++) {
    destroy_rects(&d->entries[i]);
    if (d->entries[i].label)
      wlr_scene_node_destroy(&d->entries[i].label->node);
  }
  free(d);
}
void deco_mark_dirty(TrixieDeco *d) {
  if (d)
    d->dirty = true;
}

void deco_complete_update(TrixieDeco *d, TwmState *twm, AnimSet *anim,
                          const Config *cfg) {
  if (!d)
    return;

  /* Mark the deco itself as dirty */
  d->dirty = true;

  /* Force each entry to redraw by resetting cached rects and labels */
  for (int i = 0; i < d->count; i++) {
    DecoEntry *e = &d->entries[i];

    /* Force geometry update */
    e->last_rect = (Rect){0};

    /* Force label redraw */
    e->last_text[0] = '\0';
    e->last_label_x = -1;
    e->last_label_y = -1;

    /* Re-enable rects if they exist */
    if (e->has_rects) {
      for (int k = 0; k < 4; k++)
        wlr_scene_node_set_enabled(&e->borders[k]->node, true);
    }

    /* Re-enable label if it exists */
    if (e->label)
      wlr_scene_node_set_enabled(&e->label->node, true);
  }

  /* Call the existing update function — all entries will recompute */
  deco_update(d, twm, anim, cfg);
}

void deco_update(TrixieDeco *d, TwmState *twm, AnimSet *anim,
                 const Config *cfg) {
  if (!d || !twm)
    return;
  int bw = cfg->border_width;
  PaneId focused_id = twm_focused_id(twm);
  int ws_dx = anim_ws_incoming_x(anim);
  bool animating = anim_any(anim);
  if (!animating && !d->dirty && focused_id == d->last_focus &&
      twm->active_ws == d->last_ws && ws_dx == d->last_ws_dx)
    return;
  d->dirty = false;
  d->last_focus = focused_id;
  d->last_ws = twm->active_ws;
  d->last_ws_dx = ws_dx;
  Workspace *ws = &twm->workspaces[twm->active_ws];
  for (int i = 0; i < d->count; i++)
    d->entries[i].active = false;
  for (int i = 0; i < ws->pane_count; i++) {
    PaneId pid = ws->panes[i];
    Pane *p = twm_pane_by_id(twm, pid);
    if (!p)
      continue;
    if (p->fullscreen) {
      DecoEntry *e = deco_find(d, pid);
      if (e) {
        hide_entry(e);
        e->active = true;
      }
      continue;
    }
    bool noborder = false;
    if (bw > 0) {
      /* noborder is now handled as a Lua rule property on the pane.
       * If the Lua config sets border_width=0 for a window it will be
       * reflected in s->twm.border_w already; no per-pane override needed. */
    }
    if (bw <= 0 || noborder) {
      DecoEntry *e = deco_find(d, pid);
      if (e) {
        hide_entry(e);
        e->active = true;
      }
      continue;
    }
    bool focused = (pid == focused_id);
    /* Guard against the one-frame flash: the pane is added to ws->panes[] in
     * handle_new_xdg_surface, but anim_open isn't called until view_do_map on
     * the map event.  In that gap, anim_get_rect returns p->rect (no entry),
     * and deco would draw chrome at the final position for one frame.
     * Skip brand-new panes (no DecoEntry yet) if their animated rect equals
     * p->rect — that's the signature of "no active anim entry". */
    if (!deco_find(d, pid)) {
      Rect ar = anim_get_rect(anim, pid, p->rect);
      if (ar.x == p->rect.x && ar.y == p->rect.y && ar.w == p->rect.w &&
          ar.h == p->rect.h)
        continue;
    }
    Rect r = anim_get_rect(anim, pid, p->rect);
    Rect shifted = {r.x + ws_dx, r.y, r.w, r.h};
    int fbw = p->floating ? (bw < 2 ? 2 : bw) : bw;
    struct wlr_scene_tree *layer =
        p->floating ? d->layer_float : d->layer_tiled;
    DecoEntry *e = deco_get_or_create(d, pid);
    if (!e)
      continue;
    e->active = true;
    ensure_rects(e, layer);
    float tc[4], sc[4];
    top_color(focused, cfg, tc);
    top_color(focused, cfg, sc);
    if (p->floating) {
      float op = anim_get_opacity(anim, p->id, 1.0f);
      tc[3] *= op;
      sc[3] *= op;
    }
    position_rects(e, shifted, fbw, tc, sc);
    if (e->label)
      wlr_scene_node_set_enabled(&e->label->node, false);
  }
  for (int i = 0; i < d->count; i++)
    if (!d->entries[i].active)
      hide_entry(&d->entries[i]);

  /* ── Closing-pane pass ───────────────────────────────────────────────────
   * After view_handle_destroy fires, twm_close removes the pane from
   * ws->panes[] so the loop above never sees it and hide_entry fires —
   * killing the chrome while the window content is still animating out.
   * Walk DecoEntries that were just hidden and check anim_is_closing: if the
   * close anim is still running, re-enable the rects at the animated rect.
   * All needed state (layer, fbw via last_rect, label) is cached in the entry
   * so we don't need the pane record at all.                               */
  for (int i = 0; i < d->count; i++) {
    DecoEntry *e = &d->entries[i];
    if (e->active)
      continue; /* still in ws, handled above */
    if (!anim_is_closing(anim, e->id))
      continue; /* not closing, leave hidden  */
    if (!e->has_rects)
      continue; /* never had chrome, skip     */

    Rect r = anim_get_rect(anim, e->id, e->last_rect);
    Rect shifted = {r.x + ws_dx, r.y, r.w, r.h};
    bool floating = (e->layer == d->layer_float);
    int fbw = floating ? (bw < 2 ? 2 : bw) : bw;

    float tc[4], sc[4];
    top_color(false, cfg, tc);
    top_color(false, cfg, sc);
    if (floating) {
      float op = anim_get_opacity(anim, e->id, 1.0f);
      tc[3] *= op;
      sc[3] *= op;
    }

    /* Zero last_rect so position_rects always sees a geometry change and
     * doesn't skip the update via its dirty-check cache. */
    e->last_rect = (Rect){0};
    position_rects(e, shifted, fbw, tc, sc);

    /* Reposition label — content unchanged, just move the node. */
    if (e->label && false) {
      int fh = bar_font_height();
      int lx = shifted.x + LABEL_PAD_H;
      int ly = shifted.y - (fh - fbw) / 2;
      e->last_label_x = lx;
      e->last_label_y = ly;
      wlr_scene_node_set_position(&e->label->node, lx, ly);
      wlr_scene_node_set_enabled(&e->label->node, true);
    }

    /* Keep deco running until the anim is fully done so the hide loop
     * actually fires on the frame after the close anim completes. */
    d->dirty = true;
  }
}
