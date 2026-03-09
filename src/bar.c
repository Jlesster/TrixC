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

static void raw_buffer_destroy(struct wlr_buffer *buf) {
  struct RawBuffer *rb = wl_container_of(buf, rb, base);
  free(rb->data);
  free(rb);
}
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

static const struct wlr_buffer_impl raw_buffer_impl = {
  .destroy               = raw_buffer_destroy,
  .begin_data_ptr_access = raw_buffer_begin_data_ptr_access,
  .end_data_ptr_access   = raw_buffer_end_data_ptr_access,
};

static struct RawBuffer *raw_buffer_create(int w, int h) {
  struct RawBuffer *rb = calloc(1, sizeof(*rb));
  rb->stride           = w * 4;
  rb->data             = calloc((size_t)(w * h), 4);
  wlr_buffer_init(&rb->base, &raw_buffer_impl, w, h);
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

static void fill_pill_px(uint32_t *px,
                         int       stride,
                         int       x,
                         int       y,
                         int       w,
                         int       h,
                         uint32_t  color,
                         int       radius,
                         int       cw,
                         int       ch) {
  if(radius <= 0) {
    fill_rect_px(px, stride, x, y, w, h, color, cw, ch);
    return;
  }
  int r = radius;
  if(r > w / 2) r = w / 2;
  if(r > h / 2) r = h / 2;
  for(int row = y; row < y + h && row < ch; row++) {
    if(row < 0) continue;
    for(int col = x; col < x + w && col < cw; col++) {
      if(col < 0) continue;
      int  lx = col - x, ly = row - y;
      bool in_corner = ((lx < r && ly < r) || (lx >= w - r && ly < r) ||
                        (lx < r && ly >= h - r) || (lx >= w - r && ly >= h - r));
      if(in_corner) {
        int cx = (lx < r) ? r - 1 : w - r;
        int cy = (ly < r) ? r - 1 : h - r;
        int dx = lx - cx, dy = ly - cy;
        if(dx * dx + dy * dy > r * r) continue;
      }
      px[row * stride + col] = color;
    }
  }
}

static void
draw_sep_pipe(uint32_t *px, int stride, int x, int bh, uint32_t col, int bw) {
  int pad = bh / 5;
  for(int y = pad; y < bh - pad; y++) {
    if(x >= 0 && x < bw) px[y * stride + x] = col;
  }
}

static void
draw_sep_block(uint32_t *px, int stride, int x, int bh, uint32_t col, int bw) {
  for(int y = 0; y < bh; y++) {
    if(x >= 0 && x < bw) px[y * stride + x] = col;
    if(x + 1 >= 0 && x + 1 < bw) px[y * stride + x + 1] = col;
  }
}

static void draw_sep_arrow(uint32_t *px,
                           int       stride,
                           int       x,
                           int       bh,
                           int       w,
                           uint32_t  left_col,
                           uint32_t  right_col,
                           int       bw) {
  int half = bh / 2;
  for(int row = 0; row < bh; row++) {
    int dist = row <= half ? row : bh - 1 - row;
    int tip  = (w * dist) / (half > 0 ? half : 1);
    for(int col = x; col < x + w && col < bw; col++) {
      if(col < 0) continue;
      px[row * stride + col] = (col - x < tip) ? left_col : right_col;
    }
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §5  FreeType/HarfBuzz text
 * ═══════════════════════════════════════════════════════════════════════════ */

static int draw_text_ft(uint32_t   *px,
                        int         stride,
                        int         x,
                        int         y,
                        const char *text,
                        Color       fg,
                        int         clip_w,
                        int         clip_h) {
  if(!g_font.ft_face || !g_font.hb_font || !text || !text[0]) return 0;

  hb_buffer_t *buf = hb_buffer_create();
  hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
  hb_buffer_set_script(buf, HB_SCRIPT_LATIN);
  hb_buffer_set_language(buf, hb_language_from_string("en", -1));
  hb_buffer_add_utf8(buf, text, -1, 0, -1);
  hb_shape(g_font.hb_font, buf, NULL, 0);

  unsigned int         glyph_count;
  hb_glyph_info_t     *ginfo = hb_buffer_get_glyph_infos(buf, &glyph_count);
  hb_glyph_position_t *gpos  = hb_buffer_get_glyph_positions(buf, &glyph_count);

  int pen_x = x;
  for(unsigned int i = 0; i < glyph_count; i++) {
    if(FT_Load_Glyph(g_font.ft_face,
                     ginfo[i].codepoint,
                     FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL))
      goto next;
    {
      FT_GlyphSlot slot   = g_font.ft_face->glyph;
      FT_Bitmap   *bitmap = &slot->bitmap;
      int          gx     = pen_x + slot->bitmap_left + (gpos[i].x_offset >> 6);
      int          gy     = y - slot->bitmap_top + (gpos[i].y_offset >> 6);
      for(int row = 0; row < (int)bitmap->rows; row++) {
        int py = gy + row;
        if(py < 0 || py >= clip_h) continue;
        for(int col = 0; col < (int)bitmap->width; col++) {
          int px_x = gx + col;
          if(px_x < 0 || px_x >= clip_w) continue;
          uint8_t alpha = bitmap->buffer[row * bitmap->pitch + col];
          if(alpha) blend_pixel(&px[py * stride + px_x], alpha, fg.r, fg.g, fg.b);
        }
      }
    }
  next:
    pen_x += gpos[i].x_advance >> 6;
  }
  hb_buffer_destroy(buf);
  return pen_x - x;
}

static int measure_text_ft(const char *text) {
  if(!g_font.ft_face || !g_font.hb_font || !text || !text[0]) return 0;
  hb_buffer_t *buf = hb_buffer_create();
  hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
  hb_buffer_set_script(buf, HB_SCRIPT_LATIN);
  hb_buffer_set_language(buf, hb_language_from_string("en", -1));
  hb_buffer_add_utf8(buf, text, -1, 0, -1);
  hb_shape(g_font.hb_font, buf, NULL, 0);
  unsigned int         glyph_count;
  hb_glyph_position_t *gpos = hb_buffer_get_glyph_positions(buf, &glyph_count);
  int                  w    = 0;
  for(unsigned int i = 0; i < glyph_count; i++)
    w += gpos[i].x_advance >> 6;
  hb_buffer_destroy(buf);
  return w;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §6  BarItem
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
  const char *text;
  Color       fg;
  Color       bg;
  bool        has_bg;
  int         pad;
} BarItem;

static int bar_item_width(const BarItem *it) {
  return measure_text_ft(it->text) + it->pad * 2;
}

static int flush_item(uint32_t      *px,
                      int            stride,
                      const BarItem *it,
                      int            x,
                      int            bar_h,
                      int            bw,
                      int            bh,
                      int            radius) {
  int w = bar_item_width(it);
  if(it->has_bg) {
    int ih = bar_h - 4;
    int iy = (bar_h - ih) / 2;
    fill_pill_px(px, stride, x, iy, w, ih, to_argb(it->bg), radius, bw, bh);
  }
  int baseline = (bar_h - g_font.height) / 2 + g_font.ascender;
  draw_text_ft(px, stride, x + it->pad, baseline, it->text, it->fg, bw, bh);
  return w;
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
  b->scene_buf = wlr_scene_buffer_create(layer, NULL);
  wlr_scene_node_set_position(&b->scene_buf->node, 0, b->bar_y);
  b->dirty = true;
  return b;
}

void bar_resize(TrixieBar *b, int w, int h) {
  b->width  = w;
  b->height = h;
  free(b->pixels);
  b->pixels = calloc((size_t)(w * b->bar_h), 4);
  b->dirty  = true;
}

void bar_destroy(TrixieBar *b) {
  if(!b) return;
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

  /* Background */
  uint32_t bg_c = to_argb(bar->bg);
  for(int i = 0; i < bw * bh; i++)
    px[i] = bg_c;

  /* Separator line */
  if(bar->separator) {
    uint32_t sep_c = to_argb(bar->separator_color);
    int      sep_y = bar->separator_top ? 0 : bh - 1;
    for(int x = 0; x < bw; x++)
      px[sep_y * stride + x] = sep_c;
  }

#define MAX_ITEMS 64
  BarItem left[MAX_ITEMS], center[MAX_ITEMS], right[MAX_ITEMS];
  int     ln = 0, cn = 0, rn = 0;

  char ws_labels[MAX_WORKSPACES][8];
  char clock_buf[64];
  char title_buf[128];

  int sp = bar->item_spacing > 0 ? bar->item_spacing : 4;

  int sep_w = 0;
  switch(bar->sep_style) {
    case SEP_ARROW: sep_w = bh / 2; break;
    case SEP_BLOCK: sep_w = 4; break;
    case SEP_PIPE: sep_w = 8; break;
    default: sep_w = 0; break;
  }

#define SLOT_ITEM(slot_idx_, list_, n_, fg_color_)                           \
  do {                                                                       \
    if(b->pool) {                                                            \
      BarModuleSlot *_s = &b->pool->slots[(slot_idx_)];                      \
      if(_s->valid && (n_) < MAX_ITEMS) {                                    \
        BarItem _it     = { .text = _s->text, .fg = (fg_color_), .pad = 6 }; \
        (list_)[(n_)++] = _it;                                               \
      }                                                                      \
    }                                                                        \
  } while(0)

#define RENDER_MODULE(name_, list_, n_)                                             \
  do {                                                                              \
    const char *_mn = (name_);                                                      \
    BarItem    *_l  = (list_);                                                      \
    int        *_n  = &(n_);                                                        \
    if(!strcmp(_mn, "workspaces")) {                                                \
      for(int _i = 0; _i < twm->ws_count && *_n < MAX_ITEMS; _i++) {                \
        Workspace *_ws     = &twm->workspaces[_i];                                  \
        bool       _urgent = (twm->ws_urgent_mask & (1u << _i)) != 0;               \
        snprintf(ws_labels[_i],                                                     \
                 sizeof(ws_labels[_i]),                                             \
                 _urgent ? "%d\xE2\x80\xA2" : "%d",                                 \
                 _i + 1);                                                           \
        BarItem _it = { .text = ws_labels[_i], .pad = 5 };                          \
        if(_i == twm->active_ws) {                                                  \
          _it.fg     = bar->active_ws_fg;                                           \
          _it.bg     = bar->active_ws_bg;                                           \
          _it.has_bg = true;                                                        \
        } else if(_urgent) {                                                        \
          _it.fg = (Color){ 0xf3, 0x8b, 0xa8, 0xff };                               \
        } else if(_ws->pane_count > 0) {                                            \
          _it.fg = bar->occupied_ws_fg;                                             \
        } else {                                                                    \
          _it.fg = bar->inactive_ws_fg;                                             \
        }                                                                           \
        _l[(*_n)++] = _it;                                                          \
      }                                                                             \
    } else if(!strcmp(_mn, "title")) {                                              \
      if(fp && fp->title[0] && *_n < MAX_ITEMS) {                                   \
        strncpy(title_buf, fp->title, sizeof(title_buf) - 1);                       \
        if(strlen(fp->title) > 48) {                                                \
          title_buf[45] = '.';                                                      \
          title_buf[46] = '.';                                                      \
          title_buf[47] = '.';                                                      \
          title_buf[48] = '\0';                                                     \
        }                                                                           \
        _l[(*_n)++] = (BarItem){ .text = title_buf, .fg = bar->fg, .pad = 6 };      \
      }                                                                             \
    } else if(!strcmp(_mn, "layout")) {                                             \
      if(*_n < MAX_ITEMS) {                                                         \
        static char _lb[32];                                                        \
        snprintf(_lb,                                                               \
                 sizeof(_lb),                                                       \
                 "[%s]",                                                            \
                 layout_label(twm->workspaces[twm->active_ws].layout));             \
        _l[(*_n)++] = (BarItem){ .text = _lb, .fg = bar->accent, .pad = 8 };        \
      }                                                                             \
    } else if(!strcmp(_mn, "clock")) {                                              \
      if(*_n < MAX_ITEMS) {                                                         \
        const char *_fmt = "%H:%M";                                                 \
        for(int _ci = 0; _ci < cfg->bar.module_cfg_count; _ci++) {                  \
          if(!strcmp(cfg->bar.module_cfgs[_ci].name, "clock") &&                    \
             cfg->bar.module_cfgs[_ci].format[0]) {                                 \
            _fmt = cfg->bar.module_cfgs[_ci].format;                                \
            break;                                                                  \
          }                                                                         \
        }                                                                           \
        time_t     _nt = time(NULL);                                                \
        struct tm *_tm = localtime(&_nt);                                           \
        strftime(clock_buf, sizeof(clock_buf), _fmt, _tm);                          \
        _l[(*_n)++] = (BarItem){ .text = clock_buf, .fg = bar->accent, .pad = 8 };  \
      }                                                                             \
    } else if(!strcmp(_mn, "battery")) {                                            \
      SLOT_ITEM(BAR_SLOT_BATTERY, _l, *_n, bar->fg);                                \
    } else if(!strcmp(_mn, "network")) {                                            \
      SLOT_ITEM(BAR_SLOT_NETWORK, _l, *_n, bar->fg);                                \
    } else if(!strcmp(_mn, "volume")) {                                             \
      SLOT_ITEM(BAR_SLOT_VOLUME, _l, *_n, bar->fg);                                 \
    } else if(!strcmp(_mn, "cpu")) {                                                \
      SLOT_ITEM(BAR_SLOT_CPU, _l, *_n, bar->accent);                                \
    } else if(!strcmp(_mn, "memory")) {                                             \
      SLOT_ITEM(BAR_SLOT_MEMORY, _l, *_n, bar->accent);                             \
    } else {                                                                        \
      for(int _ei = 0; _ei < cfg->bar.module_cfg_count && *_n < MAX_ITEMS; _ei++) { \
        const BarModuleCfg *_mc = &cfg->bar.module_cfgs[_ei];                       \
        if(!strcmp(_mc->name, _mn)) {                                               \
          Color _fg = _mc->has_color ? _mc->color : bar->fg;                        \
          SLOT_ITEM(BAR_SLOT_EXEC_BASE + _ei, _l, *_n, _fg);                        \
          break;                                                                    \
        }                                                                           \
      }                                                                             \
    }                                                                               \
  } while(0)

  for(int mi = 0; mi < bar->modules_left_n && ln < MAX_ITEMS; mi++)
    RENDER_MODULE(bar->modules_left[mi], left, ln);
  for(int mi = 0; mi < bar->modules_center_n && cn < MAX_ITEMS; mi++)
    RENDER_MODULE(bar->modules_center[mi], center, cn);
  for(int mi = 0; mi < bar->modules_right_n && rn < MAX_ITEMS; mi++)
    RENDER_MODULE(bar->modules_right[mi], right, rn);

#undef RENDER_MODULE
#undef SLOT_ITEM

  int lw = 0;
  for(int i = 0; i < ln; i++)
    lw += bar_item_width(&left[i]) + sp;
  int cw_total = 0;
  for(int i = 0; i < cn; i++)
    cw_total += bar_item_width(&center[i]) + sp;
  int rw_total = 0;
  for(int i = 0; i < rn; i++)
    rw_total += bar_item_width(&right[i]) + sp;

  {
    int x = sp;
    for(int i = 0; i < ln; i++)
      x += flush_item(px, stride, &left[i], x, bh, bw, bh, 0) + sp;
    if(bar->sep_style != SEP_NONE && (cn > 0 || rn > 0)) {
      uint32_t dim_c = to_argb(bar->dim);
      uint32_t bg_c2 = to_argb(bar->bg);
      switch(bar->sep_style) {
        case SEP_PIPE: draw_sep_pipe(px, stride, x + 2, bh, dim_c, bw); break;
        case SEP_BLOCK: draw_sep_block(px, stride, x + 1, bh, dim_c, bw); break;
        case SEP_ARROW:
          draw_sep_arrow(px, stride, x, bh, sep_w, bg_c2, bg_c2, bw);
          break;
        default: break;
      }
    }
  }
  {
    int rx = bw - rw_total - sp;
    for(int i = 0; i < rn; i++)
      rx += flush_item(px, stride, &right[i], rx, bh, bw, bh, 0) + sp;
  }
  {
    int cx = (bw - cw_total + sp) / 2;
    for(int i = 0; i < cn; i++)
      cx += flush_item(px, stride, &center[i], cx, bh, bw, bh, 0) + sp;
  }

  struct RawBuffer *rb = raw_buffer_create(bw, bh);
  memcpy(rb->data, b->pixels, (size_t)(bw * bh * 4));
  wlr_scene_buffer_set_buffer(b->scene_buf, &rb->base);
  wlr_buffer_drop(&rb->base);

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
