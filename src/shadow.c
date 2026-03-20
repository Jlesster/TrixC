/* shadow.c — 9-slice drop shadow for Trixie using wlr_scene_buffer.
 *
 * Architecture
 * ────────────
 * Each shadow is a wlr_scene_tree containing 8 wlr_scene_buffer nodes
 * (the 9th slice is the transparent window interior — we don't fill it).
 * Slices: TL, T, TR, L, R, BL, B, BR.
 *
 * The pixel data for each slice is a CPU-rendered ARGB gradient that
 * blends from shadow_color (at the window edge) to transparent (at the
 * outer edge), using a Gaussian-like falloff over `radius` pixels.
 *
 * The scene tree is positioned at (-radius + offset_x, -radius + offset_y)
 * relative to the window's scene_tree so the shadow appears behind it.
 *
 * All buffers use the same wlr_buffer implementation as wibox/deco (RawBuf
 * from bar.c / deco.c). We duplicate it here to keep shadow.c self-contained.
 */
#include "trixie.h"
#include <drm_fourcc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>

/* ── Minimal wlr_buffer (same pattern as deco.c TBuf) ─────────────────── */

typedef struct {
  struct wlr_buffer base;
  uint32_t *data;
  int w, h;
} ShadowBuf;

static void sbuf_destroy(struct wlr_buffer *b) {
  ShadowBuf *sb = wl_container_of(b, sb, base);
  free(sb->data);
  free(sb);
}
static bool sbuf_begin(struct wlr_buffer *b, uint32_t flags, void **data,
                       uint32_t *fmt, size_t *stride) {
  (void)flags;
  ShadowBuf *sb = wl_container_of(b, sb, base);
  *data = sb->data;
  *fmt = DRM_FORMAT_ARGB8888;
  *stride = (size_t)(sb->w * 4);
  return true;
}
static void sbuf_end(struct wlr_buffer *b) { (void)b; }
static const struct wlr_buffer_impl sbuf_impl = {
    .destroy = sbuf_destroy,
    .begin_data_ptr_access = sbuf_begin,
    .end_data_ptr_access = sbuf_end,
};
static ShadowBuf *sbuf_create(int w, int h) {
  if (w <= 0 || h <= 0)
    return NULL;
  ShadowBuf *sb = calloc(1, sizeof(*sb));
  if (!sb)
    return NULL;
  sb->w = w;
  sb->h = h;
  sb->data = calloc((size_t)(w * h), 4);
  if (!sb->data) {
    free(sb);
    return NULL;
  }
  wlr_buffer_init(&sb->base, &sbuf_impl, w, h);
  return sb;
}

/* ── Gaussian falloff helper ───────────────────────────────────────────── */
/* Returns alpha in [0, 255] for distance `d` pixels from edge, given
 * blur radius `r`.  Uses a simple exponential approximation. */
static inline uint8_t shadow_alpha(int d, int r, float base_opacity) {
  if (r <= 0 || d >= r)
    return 0;
  float t = 1.0f - (float)d / (float)r;
  float a = t * t * base_opacity * 255.0f;
  if (a > 255.0f)
    a = 255.0f;
  return (uint8_t)a;
}

/* ── Slice indices ─────────────────────────────────────────────────────── */
typedef enum { S_TL, S_T, S_TR, S_L, S_R, S_BL, S_B, S_BR, S_COUNT } SliceIdx;

/* TrixieShadow struct is defined in shadow.h */

/* ── Render a single slice into a new ShadowBuf ────────────────────────── */
static ShadowBuf *render_slice(SliceIdx idx, int r, int w, int h, uint8_t cr,
                               uint8_t cg, uint8_t cb, float opacity) {
  ShadowBuf *sb = sbuf_create(w, h);
  if (!sb)
    return NULL;
  uint32_t *px = sb->data;

  for (int row = 0; row < h; row++) {
    for (int col = 0; col < w; col++) {
      /* Distance from the nearest window edge for this slice */
      int d = 0;
      switch (idx) {
      case S_TL:
        d = r - 1 - (col < row ? col : row);
        break;
      case S_T:
        d = r - 1 - row;
        break;
      case S_TR:
        d = r - 1 - ((w - 1 - col) < row ? (w - 1 - col) : row);
        break;
      case S_L:
        d = r - 1 - col;
        break;
      case S_R:
        d = r - 1 - (w - 1 - col);
        break;
      case S_BL:
        d = r - 1 - (col < (h - 1 - row) ? col : (h - 1 - row));
        break;
      case S_B:
        d = r - 1 - (h - 1 - row);
        break;
      case S_BR:
        d = r - 1 -
            ((w - 1 - col) < (h - 1 - row) ? (w - 1 - col) : (h - 1 - row));
        break;
      default:
        d = r;
        break;
      }
      uint8_t a = shadow_alpha(d < 0 ? 0 : d, r, opacity);
      px[row * w + col] = ((uint32_t)a << 24) | ((uint32_t)cr << 16) |
                          ((uint32_t)cg << 8) | (uint32_t)cb;
    }
  }
  return sb;
}

/* ── Upload a slice buffer to its scene node ───────────────────────────── */
static void upload_slice(struct wlr_scene_buffer *node, ShadowBuf *sb) {
  if (!node || !sb)
    return;
  wlr_scene_buffer_set_buffer(node, &sb->base);
  wlr_buffer_drop(&sb->base);
}

/* ── Layout: position and size each slice around the window ───────────── */
static void layout_slices(TrixieShadow *sh) {
  int r = sh->radius;
  int ww = sh->win_w;
  int wh = sh->win_h;
  int ox = sh->offset_x;
  int oy = sh->offset_y;

  /* Corner size = r × r; edges stretch to fill window edge. */
  struct {
    int x, y, w, h;
  } geom[S_COUNT] = {
      [S_TL] = {ox - r, oy - r, r, r},  [S_T] = {ox, oy - r, ww, r},
      [S_TR] = {ox + ww, oy - r, r, r}, [S_L] = {ox - r, oy, r, wh},
      [S_R] = {ox + ww, oy, r, wh},     [S_BL] = {ox - r, oy + wh, r, r},
      [S_B] = {ox, oy + wh, ww, r},     [S_BR] = {ox + ww, oy + wh, r, r},
  };

  for (int i = 0; i < S_COUNT; i++) {
    if (!sh->bufs[i])
      continue;
    wlr_scene_node_set_position(&sh->bufs[i]->node, geom[i].x, geom[i].y);
    wlr_scene_buffer_set_dest_size(sh->bufs[i], geom[i].w, geom[i].h);
  }
}

/* ── Re-render all slices (called on create + resize) ──────────────────── */
static void rebuild_slices(TrixieShadow *sh) {
  int r = sh->radius;
  if (r <= 0)
    return;

  /* Corner slices are always r × r; edge slices vary. */
  int edge_w_h = sh->win_h; /* for L and R  */
  int edge_w_w = sh->win_w; /* for T and B  */
  if (edge_w_h < 1)
    edge_w_h = 1;
  if (edge_w_w < 1)
    edge_w_w = 1;

  int sizes[S_COUNT][2] = {
      [S_TL] = {r, r},       [S_T] = {edge_w_w, r}, [S_TR] = {r, r},
      [S_L] = {r, edge_w_h}, [S_R] = {r, edge_w_h}, [S_BL] = {r, r},
      [S_B] = {edge_w_w, r}, [S_BR] = {r, r},
  };

  for (int i = 0; i < S_COUNT; i++) {
    if (!sh->bufs[i])
      continue;
    ShadowBuf *sb = render_slice((SliceIdx)i, r, sizes[i][0], sizes[i][1],
                                 sh->cr, sh->cg, sh->cb, sh->opacity);
    upload_slice(sh->bufs[i], sb);
  }
  layout_slices(sh);
}

/* ── Public API ────────────────────────────────────────────────────────── */

TrixieShadow *shadow_create(struct wlr_scene_tree *parent, const void *cfg_ptr,
                            int w, int h) {
  const ShadowCfg *cfg = (const ShadowCfg *)cfg_ptr;
  if (!parent || !cfg || !cfg->enabled || cfg->radius <= 0)
    return NULL;

  TrixieShadow *sh = calloc(1, sizeof(*sh));
  if (!sh)
    return NULL;

  sh->radius = cfg->radius;
  sh->offset_x = cfg->offset_x;
  sh->offset_y = cfg->offset_y;
  sh->opacity = cfg->opacity > 0.0f ? cfg->opacity : 0.5f;
  sh->cr = cfg->color.r;
  sh->cg = cfg->color.g;
  sh->cb = cfg->color.b;
  sh->win_w = w;
  sh->win_h = h;

  /* The shadow tree sits behind the window. Position it so its top-left
   * (which is at offset -radius) aligns with the window's top-left. */
  sh->tree = wlr_scene_tree_create(parent);
  if (!sh->tree) {
    free(sh);
    return NULL;
  }

  /* Place the shadow tree behind window content by inserting it first —
   * scene graph renders children in insertion order (back-to-front). */
  wlr_scene_node_lower_to_bottom(&sh->tree->node);
  /* Position tree at (0,0) relative to parent; individual slice nodes
   * carry the negative offsets for the blur overhang. */
  wlr_scene_node_set_position(&sh->tree->node, 0, 0);

  static const float zero[4] = {0};
  for (int i = 0; i < S_COUNT; i++) {
    sh->bufs[i] = wlr_scene_buffer_create(sh->tree, NULL);
    if (!sh->bufs[i]) {
      shadow_destroy(sh);
      return NULL;
    }
    (void)zero;
  }

  rebuild_slices(sh);
  return sh;
}

void shadow_resize(TrixieShadow *sh, int w, int h) {
  if (!sh)
    return;
  if (sh->win_w == w && sh->win_h == h)
    return;
  sh->win_w = w;
  sh->win_h = h;
  rebuild_slices(sh);
}

void shadow_set_enabled(TrixieShadow *sh, bool enabled) {
  if (!sh || !sh->tree)
    return;
  wlr_scene_node_set_enabled(&sh->tree->node, enabled);
}

void shadow_destroy(TrixieShadow *sh) {
  if (!sh)
    return;
  if (sh->tree)
    wlr_scene_node_destroy(&sh->tree->node);
  free(sh);
}
