/* bar.c — Status bar renderer using wlr_scene + FreeType/HarfBuzz.
 *
 * All system-info polling (battery, network, volume, cpu, memory, exec
 * modules) has been moved to bar_worker.c.  bar_update() only reads from
 * the pre-populated BarWorkerPool slots — zero popen(), zero blocking.
 *
 */
#include "trixie.h"
#include <dirent.h>
#include <drm_fourcc.h>
#include <math.h>
#include <pixman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H

#include <hb-ft.h>
#include <hb.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Font state
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
  FT_Library ft_lib;
  FT_Face    ft_face;
  FT_Face    ft_face_bold;
  FT_Face    ft_face_italic;
  hb_font_t *hb_font;
  hb_font_t *hb_font_bold;
  hb_font_t *hb_font_italic;
  int        ascender;
  int        descender;
  int        height;
} BarFont;

static BarFont g_font = { 0 };

/* ── Glyph bitmap cache ─────────────────────────────────────────────────────
 * FT_Load_Glyph with FT_LOAD_RENDER is expensive — it rasterises the glyph
 * every call.  We cache the bitmap for each glyph index so each glyph is
 * rendered exactly once per font/size configuration.
 *
 * Key: FT glyph index (not codepoint — avoids double lookup).
 * Capacity: 1024 slots, open-addressing with linear probe.
 * Cleared automatically on font_reload().
 */
#define BAR_GCACHE_SIZE 1024
#define BAR_GCACHE_MASK (BAR_GCACHE_SIZE - 1)

typedef struct {
  uint32_t glyph_index; /* 0 = empty slot */
  int      bearing_x;
  int      bearing_y;
  int      advance_x; /* pixels */
  int      bm_w, bm_h, bm_pitch;
  uint8_t *bm; /* heap-allocated bitmap, freed on cache_clear */
} BarGlyphCache;

static BarGlyphCache g_bar_gcache[BAR_GCACHE_SIZE];

static void bar_gcache_clear(void) {
  for(int i = 0; i < BAR_GCACHE_SIZE; i++) {
    free(g_bar_gcache[i].bm);
    g_bar_gcache[i] = (BarGlyphCache){ 0 };
  }
}

/* Returns a pointer to the cached glyph, loading it if necessary.
 * Returns NULL if the glyph cannot be loaded. */
static const BarGlyphCache *bar_gcache_get(uint32_t glyph_index) {
  if(!glyph_index) return NULL;
  uint32_t slot = glyph_index & BAR_GCACHE_MASK;
  for(int i = 0; i < BAR_GCACHE_SIZE; i++) {
    uint32_t s = (slot + (uint32_t)i) & BAR_GCACHE_MASK;
    if(g_bar_gcache[s].glyph_index == glyph_index) return &g_bar_gcache[s];
    if(g_bar_gcache[s].glyph_index == 0) {
      /* Empty slot — load and store. */
      if(FT_Load_Glyph(
             g_font.ft_face, glyph_index, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL))
        return NULL;
      FT_GlyphSlot   sl = g_font.ft_face->glyph;
      BarGlyphCache *cg = &g_bar_gcache[s];
      cg->glyph_index   = glyph_index;
      cg->bearing_x     = sl->bitmap_left;
      cg->bearing_y     = sl->bitmap_top;
      cg->advance_x     = (int)(sl->advance.x >> 6);
      cg->bm_w          = (int)sl->bitmap.width;
      cg->bm_h          = (int)sl->bitmap.rows;
      cg->bm_pitch      = sl->bitmap.pitch;
      int sz            = cg->bm_h * cg->bm_pitch;
      cg->bm            = sz > 0 ? malloc(sz) : NULL;
      if(cg->bm) memcpy(cg->bm, sl->bitmap.buffer, sz);
      return cg;
    }
  }
  /* Cache full — fall back to uncached render (shouldn't happen in practice). */
  if(FT_Load_Glyph(
         g_font.ft_face, glyph_index, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL))
    return NULL;
  static BarGlyphCache s_tmp;
  FT_GlyphSlot         sl = g_font.ft_face->glyph;
  s_tmp.glyph_index       = glyph_index;
  s_tmp.bearing_x         = sl->bitmap_left;
  s_tmp.bearing_y         = sl->bitmap_top;
  s_tmp.advance_x         = (int)(sl->advance.x >> 6);
  s_tmp.bm_w              = (int)sl->bitmap.width;
  s_tmp.bm_h              = (int)sl->bitmap.rows;
  s_tmp.bm_pitch          = sl->bitmap.pitch;
  s_tmp.bm = sl->bitmap.buffer; /* NOT owned — valid only during this call */
  return &s_tmp;
}

static bool load_face(FT_Library  lib,
                      const char *path,
                      float       size_pt,
                      FT_Face    *face_out,
                      hb_font_t **hb_out) {
  if(!path || !path[0]) return false;
  if(FT_New_Face(lib, path, 0, face_out)) return false;
  FT_Set_Char_Size(*face_out, 0, (FT_F26Dot6)(size_pt * 64.f), 96, 96);
  *hb_out = hb_ft_font_create_referenced(*face_out);
  hb_ft_font_set_funcs(*hb_out);
  return true;
}

static void font_init(const char *path,
                      const char *path_bold,
                      const char *path_italic,
                      float       size_pt) {
  if(FT_Init_FreeType(&g_font.ft_lib)) return;
  if(size_pt <= 0.f) size_pt = 13.f;

  const char *candidates[] = {
    (path && path[0]) ? path : NULL,
    "/usr/share/fonts/TTF/JetBrainsMono-Regular.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/liberation/LiberationMono-Regular.ttf",
    NULL,
  };
  bool loaded = false;
  for(int i = 0; candidates[i]; i++) {
    if(!candidates[i]) continue;
    if(load_face(g_font.ft_lib,
                 candidates[i],
                 size_pt,
                 &g_font.ft_face,
                 &g_font.hb_font)) {
      loaded = true;
      break;
    }
  }
  if(!loaded) {
    FT_Done_FreeType(g_font.ft_lib);
    g_font.ft_lib = NULL;
    return;
  }

  g_font.ascender = (int)ceilf((float)g_font.ft_face->size->metrics.ascender / 64.f);
  g_font.descender =
      (int)floorf((float)g_font.ft_face->size->metrics.descender / 64.f);
  g_font.height = g_font.ascender - g_font.descender;

  if(path_bold && path_bold[0] &&
     load_face(g_font.ft_lib,
               path_bold,
               size_pt,
               &g_font.ft_face_bold,
               &g_font.hb_font_bold)) {
  } else {
    g_font.ft_face_bold = g_font.ft_face;
    g_font.hb_font_bold = g_font.hb_font;
  }

  if(path_italic && path_italic[0] &&
     load_face(g_font.ft_lib,
               path_italic,
               size_pt,
               &g_font.ft_face_italic,
               &g_font.hb_font_italic)) {
  } else {
    g_font.ft_face_italic = g_font.ft_face;
    g_font.hb_font_italic = g_font.hb_font;
  }
}

static void font_reload(const char *path,
                        const char *path_bold,
                        const char *path_italic,
                        float       size_pt) {
  if(g_font.hb_font_italic && g_font.hb_font_italic != g_font.hb_font)
    hb_font_destroy(g_font.hb_font_italic);
  if(g_font.hb_font_bold && g_font.hb_font_bold != g_font.hb_font)
    hb_font_destroy(g_font.hb_font_bold);
  if(g_font.ft_face_italic && g_font.ft_face_italic != g_font.ft_face)
    FT_Done_Face(g_font.ft_face_italic);
  if(g_font.ft_face_bold && g_font.ft_face_bold != g_font.ft_face)
    FT_Done_Face(g_font.ft_face_bold);
  if(g_font.hb_font) hb_font_destroy(g_font.hb_font);
  if(g_font.ft_face) FT_Done_Face(g_font.ft_face);
  if(g_font.ft_lib) FT_Done_FreeType(g_font.ft_lib);
  memset(&g_font, 0, sizeof(g_font));
  bar_gcache_clear(); /* invalidate cached bitmaps — new face/size */
  font_init(path, path_bold, path_italic, size_pt);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  TrixieBar
 * ═══════════════════════════════════════════════════════════════════════════ */

struct TrixieBar {
  struct wlr_scene_buffer *scene_buf;
  int                      width, height;
  uint32_t                *pixels;
  int                      bar_h;
  int                      bar_y;
  char                     last_font[256];
  float                    last_size;
  BarWorkerPool           *pool;
  /* Persistent upload buffer — allocated once, reused every frame.
   * Eliminates the calloc+free that was happening on every dirty render. */
  struct RawBuffer        *rb_cache;
  /* dirty-flag state */
  bool                     dirty;
  int64_t                  last_render_ms;
  int                      last_active_ws;
  PaneId                   last_focused;
  int                      last_pane_count;
  char                     last_title[128];
  uint64_t                 last_generation; /* from pool->generation */
  uint32_t                 last_urgent_mask;
};

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  wlr_buffer wrapper
 * ═══════════════════════════════════════════════════════════════════════════ */

struct RawBuffer {
  struct wlr_buffer base;
  uint32_t         *data;
  int               stride;
};

static bool raw_buffer_begin_data_ptr_access(struct wlr_buffer *buf,
                                             uint32_t           flags,
                                             void             **data,
                                             uint32_t          *format,
                                             size_t            *stride) {
  (void)flags;
  struct RawBuffer *rb = wl_container_of(buf, rb, base);
  *data                = rb->data;
  *format              = DRM_FORMAT_ARGB8888;
  *stride              = (size_t)rb->stride;
  return true;
}
static void raw_buffer_end_data_ptr_access(struct wlr_buffer *buf) {
  (void)buf;
}

/* Persistent variant — destroy frees the RawBuffer header but NOT rb->data,
 * because the pixel data is owned by TrixieBar.pixels and lives for the bar's
 * full lifetime.  This is used by rb_cache to avoid per-frame heap churn. */
static void raw_buffer_destroy_persistent(struct wlr_buffer *buf) {
  struct RawBuffer *rb = wl_container_of(buf, rb, base);
  /* rb->data is owned by TrixieBar.pixels — do NOT free it here. */
  free(rb);
}
static const struct wlr_buffer_impl raw_buffer_impl_persistent = {
  .destroy               = raw_buffer_destroy_persistent,
  .begin_data_ptr_access = raw_buffer_begin_data_ptr_access,
  .end_data_ptr_access   = raw_buffer_end_data_ptr_access,
};

/* Create a persistent RawBuffer whose data pointer aliases an external slab.
 * The caller owns the pixel memory; the buffer header is heap-allocated once. */
static struct RawBuffer *
raw_buffer_create_persistent(uint32_t *pixels, int w, int h) {
  struct RawBuffer *rb = calloc(1, sizeof(*rb));
  rb->stride           = w * 4;
  rb->data             = pixels; /* alias — not owned */
  wlr_buffer_init(&rb->base, &raw_buffer_impl_persistent, w, h);
  return rb;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * §4  Pixel helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static uint32_t to_argb(Color c) {
  return ((uint32_t)c.a << 24) | ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) |
         (uint32_t)c.b;
}

static void
blend_pixel(uint32_t *dst, uint8_t alpha, uint8_t r, uint8_t g, uint8_t b) {
  if(!alpha) return;
  if(alpha == 255) {
    *dst = (0xffu << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    return;
  }
  uint32_t d   = *dst;
  uint32_t inv = 255 - alpha;
  uint8_t  oa  = (uint8_t)(alpha + (((d >> 24) & 0xff) * inv) / 255);
  uint8_t  or_ = (uint8_t)((r * alpha + ((d >> 16) & 0xff) * inv) / 255);
  uint8_t  og  = (uint8_t)((g * alpha + ((d >> 8) & 0xff) * inv) / 255);
  uint8_t  ob  = (uint8_t)((b * alpha + (d & 0xff) * inv) / 255);
  *dst         = ((uint32_t)oa << 24) | ((uint32_t)or_ << 16) | ((uint32_t)og << 8) |
         (uint32_t)ob;
}

static void fill_rect_px(uint32_t *px,
                         int       stride,
                         int       x,
                         int       y,
                         int       w,
                         int       h,
                         uint32_t  color,
                         int       cw,
                         int       ch) {
  for(int row = y; row < y + h && row < ch; row++)
    for(int col = x; col < x + w && col < cw; col++)
      px[row * stride + col] = color;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * §5  FreeType/HarfBuzz text
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── Shared HarfBuzz buffer (avoids hb_buffer_create/destroy per draw call) ─ */
static hb_buffer_t *s_hb_buf = NULL;
static hb_buffer_t *hb_buf_get(void) {
  if(!s_hb_buf) s_hb_buf = hb_buffer_create();
  hb_buffer_clear_contents(s_hb_buf);
  hb_buffer_set_direction(s_hb_buf, HB_DIRECTION_LTR);
  hb_buffer_set_script(s_hb_buf, HB_SCRIPT_LATIN);
  hb_buffer_set_language(s_hb_buf, hb_language_from_string("en", -1));
  return s_hb_buf;
}

static int draw_text_ft(uint32_t   *px,
                        int         stride,
                        int         x,
                        int         y,
                        const char *text,
                        Color       fg,
                        int         clip_w,
                        int         clip_h) {
  if(!g_font.ft_face || !g_font.hb_font || !text || !text[0]) return 0;

  hb_buffer_t *buf = hb_buf_get();
  hb_buffer_add_utf8(buf, text, -1, 0, -1);
  hb_shape(g_font.hb_font, buf, NULL, 0);

  unsigned int         glyph_count;
  hb_glyph_info_t     *ginfo = hb_buffer_get_glyph_infos(buf, &glyph_count);
  hb_glyph_position_t *gpos  = hb_buffer_get_glyph_positions(buf, &glyph_count);

  int pen_x = x;
  for(unsigned int i = 0; i < glyph_count; i++) {
    const BarGlyphCache *cg = bar_gcache_get(ginfo[i].codepoint);
    if(!cg || !cg->bm) goto next;
    {
      int gx = pen_x + cg->bearing_x + (gpos[i].x_offset >> 6);
      int gy = y - cg->bearing_y + (gpos[i].y_offset >> 6);
      for(int row = 0; row < cg->bm_h; row++) {
        int py = gy + row;
        if(py < 0 || py >= clip_h) continue;
        for(int col = 0; col < cg->bm_w; col++) {
          int px_x = gx + col;
          if(px_x < 0 || px_x >= clip_w) continue;
          uint8_t alpha = cg->bm[row * cg->bm_pitch + col];
          if(alpha) blend_pixel(&px[py * stride + px_x], alpha, fg.r, fg.g, fg.b);
        }
      }
    }
  next:
    pen_x += gpos[i].x_advance >> 6;
  }
  return pen_x - x;
}

static int measure_text_ft(const char *text) {
  if(!g_font.ft_face || !g_font.hb_font || !text || !text[0]) return 0;
  hb_buffer_t *buf = hb_buf_get();
  hb_buffer_add_utf8(buf, text, -1, 0, -1);
  hb_shape(g_font.hb_font, buf, NULL, 0);
  unsigned int         glyph_count;
  hb_glyph_position_t *gpos = hb_buffer_get_glyph_positions(buf, &glyph_count);
  int                  w    = 0;
  for(unsigned int i = 0; i < glyph_count; i++)
    w += gpos[i].x_advance >> 6;
  return w;
}

/* ── Public thin wrappers used by deco.c for title-bar rendering ────────── */

int bar_measure_text(const char *text) {
  return measure_text_ft(text);
}

int bar_draw_text_pub(uint32_t   *px,
                      int         stride,
                      int         x,
                      int         y,
                      const char *text,
                      Color       fg,
                      int         clip_w,
                      int         clip_h) {
  return draw_text_ft(px, stride, x, y, text, fg, clip_w, clip_h);
}

int bar_font_ascender(void) {
  return g_font.ascender;
}
int bar_font_height(void) {
  return g_font.height;
}

/* Parse first integer found in a slot string like "CPU 42%" → 42 */
static int parse_pct(const char *s) {
  while(*s && (*s < '0' || *s > '9'))
    s++;
  return (*s) ? atoi(s) : 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §7  Monotonic clock helper
 * ═══════════════════════════════════════════════════════════════════════════ */

static int64_t mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §9  Lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

void bar_mark_dirty(TrixieBar *b) {
  if(b) b->dirty = true;
}

void bar_sync_exec_modules(const Config *cfg) {
  (void)cfg;
}

TrixieBar *bar_create(struct wlr_scene_tree *layer,
                      int                    w,
                      int                    h,
                      const Config          *cfg,
                      BarWorkerPool         *pool) {
  font_reload(cfg->font_path,
              cfg->font_path_bold,
              cfg->font_path_italic,
              cfg->bar.font_size > 0.f ? cfg->bar.font_size : cfg->font_size);

  TrixieBar *b = calloc(1, sizeof(*b));
  b->width     = w;
  b->height    = h;
  b->bar_h     = cfg->bar.height;
  b->bar_y     = cfg->bar.position == BAR_BOTTOM ? h - cfg->bar.height : 0;
  b->pool      = pool;
  strncpy(b->last_font, cfg->font_path, sizeof(b->last_font) - 1);
  b->last_size = cfg->bar.font_size > 0.f ? cfg->bar.font_size : cfg->font_size;
  b->pixels    = calloc((size_t)(w * b->bar_h), 4);
  b->rb_cache  = raw_buffer_create_persistent(b->pixels, w, b->bar_h);
  b->scene_buf = wlr_scene_buffer_create(layer, NULL);
  wlr_scene_node_set_position(&b->scene_buf->node, 0, b->bar_y);
  b->dirty = true;
  return b;
}

void bar_resize(TrixieBar *b, int w, int h) {
  b->width  = w;
  b->height = h;
  free(b->pixels);
  /* Drop the old persistent header (destroy_persistent won't free pixels). */
  if(b->rb_cache) wlr_buffer_drop(&b->rb_cache->base);
  b->pixels   = calloc((size_t)(w * b->bar_h), 4);
  b->rb_cache = raw_buffer_create_persistent(b->pixels, w, b->bar_h);
  b->dirty    = true;
}

void bar_destroy(TrixieBar *b) {
  if(!b) return;
  if(b->rb_cache) wlr_buffer_drop(&b->rb_cache->base);
  free(b->pixels);
  wlr_scene_node_destroy(&b->scene_buf->node);
  free(b);
}

void bar_set_visible(TrixieBar *b, bool visible) {
  if(!b) return;
  wlr_scene_node_set_enabled(&b->scene_buf->node, visible);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §10  bar_update
 * ═══════════════════════════════════════════════════════════════════════════ */

bool bar_update(TrixieBar *b, TwmState *twm, const Config *cfg) {
  if(!b || !twm) return false;

  const BarCfg *bar = &cfg->bar;
  int           bw = b->width, bh = b->bar_h;
  if(bw <= 0 || bh <= 0) return false;

  /* Hot-reload font */
  float want_size = bar->font_size > 0.f ? bar->font_size : cfg->font_size;
  if(strcmp(b->last_font, cfg->font_path) != 0 ||
     fabsf(b->last_size - want_size) > 0.01f) {
    font_reload(
        cfg->font_path, cfg->font_path_bold, cfg->font_path_italic, want_size);
    strncpy(b->last_font, cfg->font_path, sizeof(b->last_font) - 1);
    b->last_size = want_size;
    b->dirty     = true;
  }

  /* ── Dirty-flag check ──────────────────────────────────────────────────── */
  int64_t     now        = mono_ms();
  PaneId      focused_id = twm_focused_id(twm);
  Pane       *fp         = twm_focused(twm);
  const char *title      = (fp && fp->title[0]) ? fp->title : "";
  int         cur_panes  = twm->workspaces[twm->active_ws].pane_count;
  uint64_t    gen        = b->pool ? atomic_load(&b->pool->generation) : 0;

  if(now - b->last_render_ms >= 1000) b->dirty = true; /* clock tick */
  if(twm->active_ws != b->last_active_ws) b->dirty = true;
  if(focused_id != b->last_focused) b->dirty = true;
  if(strcmp(title, b->last_title) != 0) b->dirty = true;
  if(cur_panes != b->last_pane_count) b->dirty = true;
  if(b->pool && gen != b->last_generation) b->dirty = true;
  if(twm->ws_urgent_mask != b->last_urgent_mask) b->dirty = true;

  if(!b->dirty) return false; /* ← nothing to do, tell caller to not reschedule */

  uint32_t *px     = b->pixels;
  int       stride = bw;

  /* ── Background — solid Catppuccin Mocha base ──────────────────────────── */
  uint32_t bg_c = to_argb(bar->bg);
  for(int i = 0; i < bw * bh; i++)
    px[i] = bg_c;

  /* Top edge rule — 1px accent line separating bar from windows */
  {
    uint32_t rule_c = to_argb(bar->separator ? bar->separator_color : bar->dim);
    int      rule_y = bar->separator_top ? 0 : bh - 1;
    for(int x = 0; x < bw; x++)
      px[rule_y * stride + x] = rule_c;
  }

  /* ── TUI layout constants ───────────────────────────────────────────────── */
  /* All text sits on a single baseline centred vertically in the bar. */
  int baseline = (bh - g_font.height) / 2 + g_font.ascender;
  int sp       = 6; /* horizontal padding around each segment's text */

  /* Dim separator colour — used for │ glyphs between segments */
  Color dim_col = bar->dim;
  Color fg_col  = bar->fg;
  Color ac_col  = bar->accent;

  /* ── Braille bar helper ─────────────────────────────────────────────────
   * Converts a 0-100 percentage into a 4-char braille progress bar.
   * Uses Unicode braille patterns U+2800–U+28FF.
   * Each braille char covers 4 vertical dots; we use 3 chars wide = 12 steps.
   * ─────────────────────────────────────────────────────────────────────── */
  /* Block bar using ▁▂▃▄▅▆▇█ (U+2581–U+2588) — one char, 8 levels */
  static const char *BLOCKS8[] = { " ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█" };
  /* Build a 5-cell braille-style bar for a 0-100 value.
   * Each cell is one of the block chars above. */
#define BRAILLE_BAR(buf_, val_)                                                \
  do {                                                                         \
    int _v  = (val_) < 0 ? 0 : (val_) > 100 ? 100 : (val_);                    \
    /* 5 cells, each representing 20% */                                       \
    buf_[0] = '\0';                                                            \
    for(int _ci = 0; _ci < 5; _ci++) {                                         \
      int _cell_pct = _v - _ci * 20;                                           \
      int _lvl      = _cell_pct >= 20  ? 8                                     \
                      : _cell_pct <= 0 ? 0                                     \
                                       : (int)(_cell_pct * 8.f / 20.f + 0.5f); \
      strncat(buf_, BLOCKS8[_lvl], sizeof(buf_) - strlen(buf_) - 1);           \
    }                                                                          \
  } while(0)

  /* ── Inline draw helpers ───────────────────────────────────────────────── */
#define DRAW(x_, text_, col_) \
  draw_text_ft(px, stride, (x_), baseline, (text_), (col_), bw, bh)
#define MEASURE(text_) measure_text_ft(text_)
#define PIPE(x_)       draw_text_ft(px, stride, (x_), baseline, " │ ", dim_col, bw, bh)
#define PIPE_W         (MEASURE(" │ "))

  /* ── Slot text extraction ──────────────────────────────────────────────── */
#define SLOT_TEXT(idx_) \
  (b->pool && b->pool->slots[(idx_)].valid ? b->pool->slots[(idx_)].text : "")

  /* ── LEFT SIDE ─────────────────────────────────────────────────────────── */
  int lx = sp;

  /* Workspaces: active=[1] occupied=2 empty=·  urgent=!1 */
  for(int mi = 0; mi < bar->modules_left_n; mi++) {
    const char *mod = bar->modules_left[mi];

    if(!strcmp(mod, "workspaces")) {
      for(int wi = 0; wi < twm->ws_count; wi++) {
        Workspace *ws     = &twm->workspaces[wi];
        bool       urgent = (twm->ws_urgent_mask & (1u << wi)) != 0;
        bool       active = (wi == twm->active_ws);
        bool       occ    = (ws->pane_count > 0);
        char       label[16];
        if(urgent)
          snprintf(label, sizeof(label), "[!%d]", wi + 1);
        else if(active)
          snprintf(label, sizeof(label), "[%d]", wi + 1);
        else if(occ)
          snprintf(label, sizeof(label), " %d ", wi + 1);
        else
          snprintf(label, sizeof(label), " · ");
        Color col = urgent   ? (Color){ 0xf3, 0x8b, 0xa8, 0xff }
                    : active ? bar->active_ws_fg
                    : occ    ? bar->occupied_ws_fg
                             : bar->inactive_ws_fg;
        /* Highlight background for active ws */
        if(active) {
          int tw = MEASURE(label) + sp * 2;
          int iy = 2, ih = bh - 4;
          fill_rect_px(
              px, stride, lx - sp, iy, tw, ih, to_argb(bar->active_ws_bg), bw, bh);
        }
        lx += DRAW(lx, label, col);
      }
      /* │ after workspaces */
      lx += DRAW(lx, " │ ", dim_col);

    } else if(!strcmp(mod, "layout")) {
      char lb[32];
      snprintf(lb,
               sizeof(lb),
               "[%s]",
               layout_label(twm->workspaces[twm->active_ws].layout));
      lx += DRAW(lx, lb, ac_col);
      lx += DRAW(lx, " │ ", dim_col);

    } else if(!strcmp(mod, "title")) {
      if(fp && fp->title[0]) {
        char tb2[96];
        snprintf(tb2, sizeof(tb2), "%.80s", fp->title);
        lx += DRAW(lx, tb2, fg_col);
        lx += DRAW(lx, " │ ", dim_col);
      }

    } else if(!strcmp(mod, "clock")) {
      const char *fmt = "%a %d %b  %H:%M";
      for(int ci = 0; ci < cfg->bar.module_cfg_count; ci++) {
        if(!strcmp(cfg->bar.module_cfgs[ci].name, "clock") &&
           cfg->bar.module_cfgs[ci].format[0]) {
          fmt = cfg->bar.module_cfgs[ci].format;
          break;
        }
      }
      char       clk[64];
      time_t     nt = time(NULL);
      struct tm *tm = localtime(&nt);
      strftime(clk, sizeof(clk), fmt, tm);
      lx += DRAW(lx, clk, fg_col);
      lx += DRAW(lx, " │ ", dim_col);

    } else {
      /* Generic slot */
      const char *txt = NULL;
      if(!strcmp(mod, "battery"))
        txt = SLOT_TEXT(BAR_SLOT_BATTERY);
      else if(!strcmp(mod, "network"))
        txt = SLOT_TEXT(BAR_SLOT_NETWORK);
      else if(!strcmp(mod, "volume"))
        txt = SLOT_TEXT(BAR_SLOT_VOLUME);
      else if(!strcmp(mod, "cpu"))
        txt = SLOT_TEXT(BAR_SLOT_CPU);
      else if(!strcmp(mod, "memory"))
        txt = SLOT_TEXT(BAR_SLOT_MEMORY);
      else {
        for(int ei = 0; ei < cfg->bar.module_cfg_count; ei++) {
          if(!strcmp(cfg->bar.module_cfgs[ei].name, mod)) {
            txt = SLOT_TEXT(BAR_SLOT_EXEC_BASE + ei);
            break;
          }
        }
      }
      if(txt && txt[0]) {
        lx += DRAW(lx, txt, fg_col);
        lx += DRAW(lx, " │ ", dim_col);
      }
    }
  }

  /* ── RIGHT SIDE — build right-to-left, then render ─────────────────────── */
  /* We pre-measure each right module and render right-to-left. */
  struct {
    char  text[128];
    Color col;
  } rsegs[MAX_BAR_MODS * 4];
  int rn = 0;

  for(int mi = bar->modules_right_n - 1; mi >= 0; mi--) {
    const char *mod = bar->modules_right[mi];

    if(!strcmp(mod, "clock")) {
      const char *fmt = "%a %d %b  %H:%M";
      for(int ci = 0; ci < cfg->bar.module_cfg_count; ci++) {
        if(!strcmp(cfg->bar.module_cfgs[ci].name, "clock") &&
           cfg->bar.module_cfgs[ci].format[0]) {
          fmt = cfg->bar.module_cfgs[ci].format;
          break;
        }
      }
      char       clk[64];
      time_t     nt = time(NULL);
      struct tm *tm = localtime(&nt);
      strftime(clk, sizeof(clk), fmt, tm);
      if(rn < (int)(sizeof(rsegs) / sizeof(rsegs[0]))) {
        snprintf(rsegs[rn].text, sizeof(rsegs[rn].text), "%s", clk);
        rsegs[rn].col = ac_col;
        rn++;
      }

    } else if(!strcmp(mod, "layout")) {
      char lb[32];
      snprintf(lb,
               sizeof(lb),
               "[%s]",
               layout_label(twm->workspaces[twm->active_ws].layout));
      if(rn < (int)(sizeof(rsegs) / sizeof(rsegs[0]))) {
        snprintf(rsegs[rn].text, sizeof(rsegs[rn].text), "%s", lb);
        rsegs[rn].col = ac_col;
        rn++;
      }

    } else if(!strcmp(mod, "cpu")) {
      const char *raw = SLOT_TEXT(BAR_SLOT_CPU);
      if(raw && raw[0]) {
        int  pct = parse_pct(raw);
        char bar_str[32];
        BRAILLE_BAR(bar_str, pct);
        char seg[64];
        snprintf(seg, sizeof(seg), "cpu %s %d%%", bar_str, pct);
        if(rn < (int)(sizeof(rsegs) / sizeof(rsegs[0]))) {
          snprintf(rsegs[rn].text, sizeof(rsegs[rn].text), "%s", seg);
          rsegs[rn].col = ac_col;
          rn++;
        }
      }

    } else if(!strcmp(mod, "memory")) {
      const char *raw = SLOT_TEXT(BAR_SLOT_MEMORY);
      if(raw && raw[0]) {
        if(rn < (int)(sizeof(rsegs) / sizeof(rsegs[0]))) {
          snprintf(rsegs[rn].text, sizeof(rsegs[rn].text), "%s", raw);
          rsegs[rn].col = fg_col;
          rn++;
        }
      }

    } else {
      const char *txt = NULL;
      if(!strcmp(mod, "battery"))
        txt = SLOT_TEXT(BAR_SLOT_BATTERY);
      else if(!strcmp(mod, "network"))
        txt = SLOT_TEXT(BAR_SLOT_NETWORK);
      else if(!strcmp(mod, "volume"))
        txt = SLOT_TEXT(BAR_SLOT_VOLUME);
      else {
        for(int ei = 0; ei < cfg->bar.module_cfg_count; ei++) {
          if(!strcmp(cfg->bar.module_cfgs[ei].name, mod)) {
            txt = SLOT_TEXT(BAR_SLOT_EXEC_BASE + ei);
            break;
          }
        }
      }
      if(txt && txt[0] && rn < (int)(sizeof(rsegs) / sizeof(rsegs[0]))) {
        snprintf(rsegs[rn].text, sizeof(rsegs[rn].text), "%s", txt);
        rsegs[rn].col = fg_col;
        rn++;
      }
    }
  }

  /* Render right segments right-to-left */
  {
    int rx = bw - sp;
    for(int i = 0; i < rn; i++) {
      int tw = MEASURE(rsegs[i].text);
      rx -= tw;
      draw_text_ft(px, stride, rx, baseline, rsegs[i].text, rsegs[i].col, bw, bh);
      if(i < rn - 1) {
        rx -= PIPE_W;
        draw_text_ft(px, stride, rx, baseline, " │ ", dim_col, bw, bh);
      }
    }
  }

  /* ── CENTER — clock or title if configured ─────────────────────────────── */
  for(int mi = 0; mi < bar->modules_center_n; mi++) {
    const char *mod      = bar->modules_center[mi];
    char        seg[128] = "";

    if(!strcmp(mod, "clock")) {
      const char *fmt = "%a %d %b  %H:%M";
      for(int ci = 0; ci < cfg->bar.module_cfg_count; ci++) {
        if(!strcmp(cfg->bar.module_cfgs[ci].name, "clock") &&
           cfg->bar.module_cfgs[ci].format[0]) {
          fmt = cfg->bar.module_cfgs[ci].format;
          break;
        }
      }
      time_t     nt = time(NULL);
      struct tm *tm = localtime(&nt);
      strftime(seg, sizeof(seg), fmt, tm);
    } else if(!strcmp(mod, "title")) {
      if(fp && fp->title[0]) snprintf(seg, sizeof(seg), "%.80s", fp->title);
    } else if(!strcmp(mod, "layout")) {
      snprintf(seg,
               sizeof(seg),
               "[%s]",
               layout_label(twm->workspaces[twm->active_ws].layout));
    }

    if(seg[0]) {
      int tw = MEASURE(seg);
      int cx = (bw - tw) / 2;
      draw_text_ft(px, stride, cx, baseline, seg, ac_col, bw, bh);
    }
  }

#undef DRAW
#undef MEASURE
#undef PIPE
#undef PIPE_W
#undef SLOT_TEXT
#undef BRAILLE_BAR

  /* Upload pixels using the persistent buffer — zero allocations on the hot path.
   * rb_cache->data already points at b->pixels, so no memcpy needed: we just
   * re-init the wlr_buffer header (which resets its reference count to 1) and
   * hand it to wlr_scene_buffer_set_buffer.  wlr_buffer_drop decrements to 0
   * and calls raw_buffer_destroy_persistent, which frees only the header — the
   * pixel slab stays alive because it is owned by b->pixels. */
  wlr_buffer_init(&b->rb_cache->base, &raw_buffer_impl_persistent, bw, bh);
  wlr_scene_buffer_set_buffer(b->scene_buf, &b->rb_cache->base);
  wlr_buffer_drop(&b->rb_cache->base);
  /* Recreate the header immediately so rb_cache is valid for next frame. */
  b->rb_cache = raw_buffer_create_persistent(b->pixels, bw, bh);

  b->dirty            = false;
  b->last_render_ms   = now;
  b->last_active_ws   = twm->active_ws;
  b->last_focused     = focused_id;
  b->last_pane_count  = cur_panes;
  b->last_generation  = gen;
  b->last_urgent_mask = twm->ws_urgent_mask;
  snprintf(b->last_title, sizeof(b->last_title), "%s", title);

  return true; /* ← we redrew, caller should schedule next frame to present it */
}
