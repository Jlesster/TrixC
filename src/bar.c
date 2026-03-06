/* bar.c — Status bar renderer using wlr_scene + freetype2/harfbuzz */
#include "trixie.h"
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

/* ── Font state ───────────────────────────────────────────────────────────── */

typedef struct {
  FT_Library ft_lib;
  FT_Face    ft_face;        /* regular  */
  FT_Face    ft_face_bold;   /* bold     */
  FT_Face    ft_face_italic; /* italic   */
  hb_font_t *hb_font;        /* regular  */
  hb_font_t *hb_font_bold;
  hb_font_t *hb_font_italic;
  int        ascender;  /* px, positive */
  int        descender; /* px, negative */
  int        height;    /* ascender - descender */
} BarFont;

static BarFont g_font = { 0 };

/* Load a single FT_Face + hb_font pair. Returns true on success. */
static bool load_face(FT_Library  lib,
                      const char *path,
                      float       size_pt,
                      FT_Face    *face_out,
                      hb_font_t **hb_out) {
  if(!path || !path[0]) return false;
  if(FT_New_Face(lib, path, 0, face_out)) {
    wlr_log(WLR_ERROR, "bar: FT_New_Face failed for '%s'", path);
    return false;
  }
  FT_Set_Char_Size(*face_out, 0, (FT_F26Dot6)(size_pt * 64.0f), 96, 96);
  *hb_out = hb_ft_font_create_referenced(*face_out);
  hb_ft_font_set_funcs(*hb_out);
  return true;
}

static void font_init(const char *path,
                      const char *path_bold,
                      const char *path_italic,
                      float       size_pt) {
  if(FT_Init_FreeType(&g_font.ft_lib)) {
    wlr_log(WLR_ERROR, "bar: FT_Init_FreeType failed");
    return;
  }

  if(size_pt <= 0.0f) size_pt = 13.0f;

  /* ── Regular (required) ── */
  /* Try the config path, then a chain of fallbacks */
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
      wlr_log(WLR_INFO, "bar: font regular  → %s", candidates[i]);
      loaded = true;
      break;
    }
  }
  if(!loaded) {
    wlr_log(WLR_ERROR, "bar: could not load any regular font");
    FT_Done_FreeType(g_font.ft_lib);
    g_font.ft_lib = NULL;
    return;
  }

  /* Metrics come from the regular face */
  g_font.ascender =
      (int)ceilf((float)g_font.ft_face->size->metrics.ascender / 64.0f);
  g_font.descender =
      (int)floorf((float)g_font.ft_face->size->metrics.descender / 64.0f);
  g_font.height = g_font.ascender - g_font.descender;

  /* ── Bold (optional, falls back to regular) ── */
  if(path_bold && path_bold[0] &&
     load_face(g_font.ft_lib,
               path_bold,
               size_pt,
               &g_font.ft_face_bold,
               &g_font.hb_font_bold)) {
    wlr_log(WLR_INFO, "bar: font bold     → %s", path_bold);
  } else {
    g_font.ft_face_bold = g_font.ft_face;
    g_font.hb_font_bold = g_font.hb_font;
    wlr_log(WLR_INFO, "bar: font bold     → (same as regular)");
  }

  /* ── Italic (optional, falls back to regular) ── */
  if(path_italic && path_italic[0] &&
     load_face(g_font.ft_lib,
               path_italic,
               size_pt,
               &g_font.ft_face_italic,
               &g_font.hb_font_italic)) {
    wlr_log(WLR_INFO, "bar: font italic   → %s", path_italic);
  } else {
    g_font.ft_face_italic = g_font.ft_face;
    g_font.hb_font_italic = g_font.hb_font;
    wlr_log(WLR_INFO, "bar: font italic   → (same as regular)");
  }

  wlr_log(WLR_INFO,
          "bar: font size=%.1fpt asc=%d desc=%d h=%d",
          size_pt,
          g_font.ascender,
          g_font.descender,
          g_font.height);
}

static void font_reload(const char *path,
                        const char *path_bold,
                        const char *path_italic,
                        float       size_pt) {
  /* Tear down bold/italic only if they are separate from regular */
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

/* ── TrixieBar ────────────────────────────────────────────────────────────── */

struct TrixieBar {
  struct wlr_scene_buffer *scene_buf;
  int                      width, height;
  uint32_t                *pixels;
  int                      bar_h;
  int                      bar_y;
  /* track last font settings to detect reload */
  char                     last_font[256];
  float                    last_size;
};

/* ── wlr_buffer wrapper ───────────────────────────────────────────────────── */

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
  *stride              = rb->stride;
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
  rb->data             = calloc(w * h, 4);
  wlr_buffer_init(&rb->base, &raw_buffer_impl, w, h);
  return rb;
}

/* ── Color helpers ────────────────────────────────────────────────────────── */

static uint32_t to_argb(Color c) {
  return ((uint32_t)c.a << 24) | ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) |
         (uint32_t)c.b;
}

/* Alpha-blend src (premul) onto dst ARGB pixel */
static void
blend_pixel(uint32_t *dst, uint8_t alpha, uint8_t r, uint8_t g, uint8_t b) {
  if(alpha == 0) return;
  if(alpha == 255) {
    *dst = (0xffu << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    return;
  }
  uint32_t d   = *dst;
  uint8_t  da  = (d >> 24) & 0xff;
  uint8_t  dr  = (d >> 16) & 0xff;
  uint8_t  dg  = (d >> 8) & 0xff;
  uint8_t  db  = d & 0xff;
  uint32_t inv = 255 - alpha;
  uint8_t  oa  = (uint8_t)(alpha + (da * inv) / 255);
  uint8_t  or_ = (uint8_t)((r * alpha + dr * inv) / 255);
  uint8_t  og  = (uint8_t)((g * alpha + dg * inv) / 255);
  uint8_t  ob  = (uint8_t)((b * alpha + db * inv) / 255);
  *dst         = ((uint32_t)oa << 24) | ((uint32_t)or_ << 16) | ((uint32_t)og << 8) |
         (uint32_t)ob;
}

/* ── Pixel fill helper ────────────────────────────────────────────────────── */

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

/* Rounded-rectangle fill (pill shape). Falls back to plain rect if radius=0. */
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
      bool in_tl = lx < r && ly < r;
      bool in_tr = lx >= w - r && ly < r;
      bool in_bl = lx < r && ly >= h - r;
      bool in_br = lx >= w - r && ly >= h - r;
      if(in_tl || in_tr || in_bl || in_br) {
        int cx = (lx < r) ? r - 1 : w - r;
        int cy = (ly < r) ? r - 1 : h - r;
        int dx = lx - cx, dy = ly - cy;
        if(dx * dx + dy * dy > r * r) continue;
      }
      px[row * stride + col] = color;
    }
  }
}

/* ── Freetype text drawing ────────────────────────────────────────────────── */

/* Returns advance width in pixels */
static int draw_text_ft(uint32_t   *px,
                        int         stride,
                        int         x,
                        int         y, /* baseline origin */
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

      int gx = pen_x + slot->bitmap_left + (gpos[i].x_offset >> 6);
      int gy = y - slot->bitmap_top + (gpos[i].y_offset >> 6);

      /* grayscale: one byte per pixel */
      int bw = (int)bitmap->width;
      int bh = (int)bitmap->rows;
      for(int row = 0; row < bh; row++) {
        int py = gy + row;
        if(py < 0 || py >= clip_h) continue;
        for(int col = 0; col < bw; col++) {
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

/* Measure text width in pixels without rendering */
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

  int w = 0;
  for(unsigned int i = 0; i < glyph_count; i++)
    w += gpos[i].x_advance >> 6;

  hb_buffer_destroy(buf);
  return w;
}

/* ── Bar item ─────────────────────────────────────────────────────────────── */

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

static int draw_bar_item(uint32_t      *px,
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

  /* baseline = ascender from top of bar, vertically centred */
  int baseline = (bar_h - g_font.height) / 2 + g_font.ascender;
  draw_text_ft(px, stride, x + it->pad, baseline, it->text, it->fg, bw, bh);
  return w;
}

/* ── System info helpers ──────────────────────────────────────────────────── */

static int read_battery_pct(bool *charging) {
  FILE *f;
  int   pct = -1;
  *charging = false;
  f         = fopen("/sys/class/power_supply/BAT0/capacity", "r");
  if(!f) f = fopen("/sys/class/power_supply/BAT1/capacity", "r");
  if(f) {
    fscanf(f, "%d", &pct);
    fclose(f);
  }
  f = fopen("/sys/class/power_supply/BAT0/status", "r");
  if(!f) f = fopen("/sys/class/power_supply/BAT1/status", "r");
  if(f) {
    char status[32];
    fscanf(f, "%31s", status);
    fclose(f);
    *charging = !strcmp(status, "Charging") || !strcmp(status, "Full");
  }
  return pct;
}

static void read_net_label(char *out, int max) {
  FILE *f = fopen("/proc/net/route", "r");
  if(!f) {
    strncpy(out, "eth0", max);
    return;
  }
  char iface[32], line[256];
  fgets(line, sizeof(line), f);
  while(fgets(line, sizeof(line), f)) {
    unsigned flags;
    if(sscanf(line, "%31s %*s %*s %x", iface, &flags) == 2 && (flags & 0x2)) {
      strncpy(out, iface, max);
      fclose(f);
      return;
    }
  }
  fclose(f);
  strncpy(out, "eth0", max);
}

static int read_volume(bool *muted) {
  *muted         = false;
  char  buf[128] = { 0 };
  FILE *p        = popen("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null", "r");
  if(p) {
    bool got = fgets(buf, sizeof(buf), p) != NULL;
    pclose(p);
    if(got) {
      float v = 0.0f;
      if(sscanf(buf, "Volume: %f", &v) == 1) {
        *muted = strstr(buf, "[MUTED]") != NULL;
        return (int)(v * 100.0f + 0.5f);
      }
    }
  }
  /* fallback: amixer */
  p = popen("amixer sget Master 2>/dev/null", "r");
  if(!p) return -1;
  while(fgets(buf, sizeof(buf), p)) {
    int pct;
    if(sscanf(buf, " Front Left: %*d [%d%%]", &pct) == 1 ||
       sscanf(buf, " Mono: %*d [%d%%]", &pct) == 1) {
      *muted = strstr(buf, "[off]") != NULL;
      pclose(p);
      return pct;
    }
  }
  pclose(p);
  return -1;
}

/* ── Bar create / destroy ─────────────────────────────────────────────────── */

TrixieBar *
bar_create(struct wlr_scene_tree *layer, int w, int h, const Config *cfg) {
  font_reload(cfg->font_path,
              cfg->font_path_bold,
              cfg->font_path_italic,
              cfg->bar.font_size > 0.0f ? cfg->bar.font_size : cfg->font_size);

  TrixieBar *b = calloc(1, sizeof(*b));
  b->width     = w;
  b->height    = h;
  b->bar_h     = cfg->bar.height;
  b->bar_y     = cfg->bar.position == BAR_BOTTOM ? h - cfg->bar.height : 0;
  strncpy(b->last_font, cfg->font_path, sizeof(b->last_font) - 1);
  b->last_size = cfg->bar.font_size > 0.0f ? cfg->bar.font_size : cfg->font_size;

  b->pixels = calloc(w * b->bar_h, 4);

  b->scene_buf = wlr_scene_buffer_create(layer, NULL);
  wlr_scene_node_set_position(&b->scene_buf->node, 0, b->bar_y);
  return b;
}

void bar_resize(TrixieBar *b, int w, int h) {
  b->width  = w;
  b->height = h;
  free(b->pixels);
  b->pixels = calloc(w * b->bar_h, 4);
}

void bar_destroy(TrixieBar *b) {
  if(!b) return;
  free(b->pixels);
  wlr_scene_node_destroy(&b->scene_buf->node);
  free(b);
}

/* ── Main update ──────────────────────────────────────────────────────────── */

void bar_update(TrixieBar *b, TwmState *twm, const Config *cfg) {
  if(!b || !twm) return;

  const BarCfg *bar = &cfg->bar;
  int           bw = b->width, bh = b->bar_h;
  if(bw <= 0 || bh <= 0) return;

  /* Hot-reload font if config changed */
  float want_size = bar->font_size > 0.0f ? bar->font_size : cfg->font_size;
  if(strcmp(b->last_font, cfg->font_path) != 0 ||
     fabsf(b->last_size - want_size) > 0.01f) {
    font_reload(
        cfg->font_path, cfg->font_path_bold, cfg->font_path_italic, want_size);
    strncpy(b->last_font, cfg->font_path, sizeof(b->last_font) - 1);
    b->last_size = want_size;
  }

  uint32_t *px     = b->pixels;
  int       stride = bw;

  /* Background */
  uint32_t bg_c = to_argb(bar->bg);
  for(int i = 0; i < bw * bh; i++)
    px[i] = bg_c;

  /* Separator line */
  if(bar->separator) {
    uint32_t sep_c = to_argb(bar->separator_color);
    int      sep_y = (bar->position == BAR_BOTTOM) ? 0 : bh - 1;
    for(int x = 0; x < bw; x++)
      px[sep_y * stride + x] = sep_c;
  }

#define MAX_ITEMS 32
  BarItem left[MAX_ITEMS], center[MAX_ITEMS], right[MAX_ITEMS];
  int     ln = 0, cn = 0, rn = 0;

  char ws_labels[MAX_WORKSPACES][8];
  char clock_buf[32], layout_buf[32], bat_buf[32], net_buf[32], vol_buf[32];

  int radius = bar->pill_radius > 0 ? bar->pill_radius : 4;

  /* Workspaces */
  for(int i = 0; i < twm->ws_count; i++) {
    Workspace *ws = &twm->workspaces[i];
    snprintf(ws_labels[i], sizeof(ws_labels[i]), " %d ", i + 1);
    BarItem it = { .text = ws_labels[i], .pad = 4 };
    if(i == twm->active_ws) {
      it.fg     = bar->active_ws_fg;
      it.bg     = bar->active_ws_bg;
      it.has_bg = true;
    } else if(ws->pane_count > 0) {
      it.fg = bar->occupied_ws_fg;
    } else {
      it.fg = bar->inactive_ws_fg;
    }
    if(ln < MAX_ITEMS) left[ln++] = it;
  }

  /* Clock */
  {
    time_t     t  = time(NULL);
    struct tm *tm = localtime(&t);
    snprintf(clock_buf, sizeof(clock_buf), " %02d:%02d ", tm->tm_hour, tm->tm_min);
    if(cn < MAX_ITEMS)
      center[cn++] = (BarItem){ .text   = clock_buf,
                                .fg     = bar->active_ws_fg,
                                .bg     = bar->active_ws_bg,
                                .has_bg = true,
                                .pad    = 4 };
  }

  /* Layout */
  {
    snprintf(layout_buf,
             sizeof(layout_buf),
             " %s ",
             layout_label(twm->workspaces[twm->active_ws].layout));
    if(rn < MAX_ITEMS)
      right[rn++] = (BarItem){ .text = layout_buf, .fg = bar->accent, .pad = 4 };
  }

  /* Battery */
  {
    bool charging;
    int  pct = read_battery_pct(&charging);
    if(pct >= 0) {
      const char *icon = charging ? "⚡" : (pct > 50 ? "🔋" : "🪫");
      snprintf(bat_buf, sizeof(bat_buf), "%s %d%%", icon, pct);
      Color fg =
          (pct <= 20 && !charging) ? (Color){ 0xf3, 0x8b, 0xa8, 0xff } : bar->fg;
      if(rn < MAX_ITEMS)
        right[rn++] = (BarItem){ .text = bat_buf, .fg = fg, .pad = 6 };
    }
  }

  /* Network */
  {
    char iface[32];
    read_net_label(iface, sizeof(iface));
    char state_path[128];
    snprintf(state_path, sizeof(state_path), "/sys/class/net/%s/operstate", iface);
    FILE *f = fopen(state_path, "r");
    if(f) {
      char state[16];
      fscanf(f, "%15s", state);
      fclose(f);
      bool up = !strcmp(state, "up");
      snprintf(net_buf, sizeof(net_buf), "%s %s", up ? "NET" : "---", iface);
      Color fg = up ? bar->fg : bar->dim;
      if(rn < MAX_ITEMS)
        right[rn++] = (BarItem){ .text = net_buf, .fg = fg, .pad = 6 };
    }
  }

  /* Volume */
  {
    bool muted;
    int  vol = read_volume(&muted);
    if(vol >= 0) {
      const char *icon = muted ? "[M]"
                               : (vol >= 70   ? "[+]"
                                  : vol >= 30 ? "[~]"
                                              : "[-]");
      snprintf(vol_buf, sizeof(vol_buf), "%s %d%%", icon, vol);
      Color fg = muted ? bar->dim : bar->fg;
      if(rn < MAX_ITEMS)
        right[rn++] = (BarItem){ .text = vol_buf, .fg = fg, .pad = 6 };
    }
  }

  int sp = bar->item_spacing > 0 ? bar->item_spacing : 6;

  /* Left */
  int x = sp;
  for(int i = 0; i < ln; i++)
    x += draw_bar_item(px, stride, &left[i], x, bh, bw, bh, radius) + sp;

  /* Right (right-aligned) */
  int rw = sp;
  for(int i = 0; i < rn; i++)
    rw += bar_item_width(&right[i]) + sp;
  int rx = bw - rw;
  for(int i = 0; i < rn; i++) {
    rx += draw_bar_item(px, stride, &right[i], rx, bh, bw, bh, radius) + sp;
  }

  /* Center */
  int ct = 0;
  for(int i = 0; i < cn; i++)
    ct += bar_item_width(&center[i]);
  int cx = (bw - ct) / 2;
  for(int i = 0; i < cn; i++)
    cx += draw_bar_item(px, stride, &center[i], cx, bh, bw, bh, radius);

  /* Upload to scene */
  struct RawBuffer *rb = raw_buffer_create(bw, bh);
  memcpy(rb->data, b->pixels, bw * bh * 4);
  wlr_scene_buffer_set_buffer(b->scene_buf, &rb->base);
  wlr_buffer_drop(&rb->base);
}
