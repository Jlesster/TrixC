/* bar.c — Status bar renderer using wlr_scene + pixman.
 *
 * The bar is a single wlr_scene_buffer node at the top/bottom of the screen.
 * We render into a pixman image_t each frame (CPU-side) and update the buffer.
 * For text we use a minimal bitmap font embedded as a fallback, or pango-cairo
 * if you link it in (detected at compile time via TRIXIE_PANGO).
 *
 * Layout zones:  [left ... | ... center ... | ... right]
 * Modules:       workspaces | clock | layout | battery | network | volume | custom
 */
#include "trixie.h"
#include <drm_fourcc.h>
#include <math.h>
#include <pixman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wlr/interfaces/wlr_buffer.h>

/* wlroots scene buffer surface */
#include <wlr/types/wlr_scene.h>

/* ── TrixieBar ────────────────────────────────────────────────────────────── */

struct TrixieBar {
  struct wlr_scene_buffer *scene_buf;
  struct wlr_buffer       *wlr_buf;
  int                      width, height;

  /* pixman backing */
  uint32_t       *pixels;
  pixman_image_t *image;

  /* cached bar height */
  int bar_h;
  int bar_y; /* y offset: 0 for top, screen_h - bar_h for bottom */
};

/* ── wlr_buffer wrapper for a raw pixel array ─────────────────────────────── */

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
  struct RawBuffer *rb = wl_container_of(buf, rb, base);
  *data                = rb->data;
  *format              = DRM_FORMAT_ARGB8888;
  *stride              = rb->stride;
  return true;
}

static void raw_buffer_end_data_ptr_access(struct wlr_buffer *buf) {}

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

/* ── Minimal 5×7 bitmap font (ASCII 32-126) ───────────────────────────────── */
/* Each character is 5 columns × 7 rows, packed as 5 bytes (one per column,
 * bit 0 = top row). This covers digits, uppercase, common symbols. */

static const uint8_t g_font5x7[][5] = {
  /* ' ' */ { 0x00, 0x00, 0x00, 0x00, 0x00 },
  /* '!' */
   { 0x00, 0x00, 0x5f, 0x00, 0x00 },
  /* '"' */
   { 0x00, 0x07, 0x00, 0x07, 0x00 },
  /* '#' */
   { 0x14, 0x7f, 0x14, 0x7f, 0x14 },
  /* '$' */
   { 0x24, 0x2a, 0x7f, 0x2a, 0x12 },
  /* '%' */
   { 0x23, 0x13, 0x08, 0x64, 0x62 },
  /* '&' */
   { 0x36, 0x49, 0x55, 0x22, 0x50 },
  /* '\'' */
   { 0x00, 0x05, 0x03, 0x00, 0x00 },
  /* '(' */
   { 0x00, 0x1c, 0x22, 0x41, 0x00 },
  /* ')' */
   { 0x00, 0x41, 0x22, 0x1c, 0x00 },
  /* '*' */
   { 0x14, 0x08, 0x3e, 0x08, 0x14 },
  /* '+' */
   { 0x08, 0x08, 0x3e, 0x08, 0x08 },
  /* ',' */
   { 0x00, 0x50, 0x30, 0x00, 0x00 },
  /* '-' */
   { 0x08, 0x08, 0x08, 0x08, 0x08 },
  /* '.' */
   { 0x00, 0x60, 0x60, 0x00, 0x00 },
  /* '/' */
   { 0x20, 0x10, 0x08, 0x04, 0x02 },
  /* '0' */
   { 0x3e, 0x51, 0x49, 0x45, 0x3e },
  /* '1' */
   { 0x00, 0x42, 0x7f, 0x40, 0x00 },
  /* '2' */
   { 0x42, 0x61, 0x51, 0x49, 0x46 },
  /* '3' */
   { 0x21, 0x41, 0x45, 0x4b, 0x31 },
  /* '4' */
   { 0x18, 0x14, 0x12, 0x7f, 0x10 },
  /* '5' */
   { 0x27, 0x45, 0x45, 0x45, 0x39 },
  /* '6' */
   { 0x3c, 0x4a, 0x49, 0x49, 0x30 },
  /* '7' */
   { 0x01, 0x71, 0x09, 0x05, 0x03 },
  /* '8' */
   { 0x36, 0x49, 0x49, 0x49, 0x36 },
  /* '9' */
   { 0x06, 0x49, 0x49, 0x29, 0x1e },
  /* ':' */
   { 0x00, 0x36, 0x36, 0x00, 0x00 },
  /* ';' */
   { 0x00, 0x56, 0x36, 0x00, 0x00 },
  /* '<' */
   { 0x08, 0x14, 0x22, 0x41, 0x00 },
  /* '=' */
   { 0x14, 0x14, 0x14, 0x14, 0x14 },
  /* '>' */
   { 0x00, 0x41, 0x22, 0x14, 0x08 },
  /* '?' */
   { 0x02, 0x01, 0x51, 0x09, 0x06 },
  /* '@' */
   { 0x32, 0x49, 0x79, 0x41, 0x3e },
  /* 'A' */
   { 0x7e, 0x11, 0x11, 0x11, 0x7e },
  /* 'B' */
   { 0x7f, 0x49, 0x49, 0x49, 0x36 },
  /* 'C' */
   { 0x3e, 0x41, 0x41, 0x41, 0x22 },
  /* 'D' */
   { 0x7f, 0x41, 0x41, 0x22, 0x1c },
  /* 'E' */
   { 0x7f, 0x49, 0x49, 0x49, 0x41 },
  /* 'F' */
   { 0x7f, 0x09, 0x09, 0x09, 0x01 },
  /* 'G' */
   { 0x3e, 0x41, 0x49, 0x49, 0x7a },
  /* 'H' */
   { 0x7f, 0x08, 0x08, 0x08, 0x7f },
  /* 'I' */
   { 0x00, 0x41, 0x7f, 0x41, 0x00 },
  /* 'J' */
   { 0x20, 0x40, 0x41, 0x3f, 0x01 },
  /* 'K' */
   { 0x7f, 0x08, 0x14, 0x22, 0x41 },
  /* 'L' */
   { 0x7f, 0x40, 0x40, 0x40, 0x40 },
  /* 'M' */
   { 0x7f, 0x02, 0x0c, 0x02, 0x7f },
  /* 'N' */
   { 0x7f, 0x04, 0x08, 0x10, 0x7f },
  /* 'O' */
   { 0x3e, 0x41, 0x41, 0x41, 0x3e },
  /* 'P' */
   { 0x7f, 0x09, 0x09, 0x09, 0x06 },
  /* 'Q' */
   { 0x3e, 0x41, 0x51, 0x21, 0x5e },
  /* 'R' */
   { 0x7f, 0x09, 0x19, 0x29, 0x46 },
  /* 'S' */
   { 0x46, 0x49, 0x49, 0x49, 0x31 },
  /* 'T' */
   { 0x01, 0x01, 0x7f, 0x01, 0x01 },
  /* 'U' */
   { 0x3f, 0x40, 0x40, 0x40, 0x3f },
  /* 'V' */
   { 0x1f, 0x20, 0x40, 0x20, 0x1f },
  /* 'W' */
   { 0x3f, 0x40, 0x38, 0x40, 0x3f },
  /* 'X' */
   { 0x63, 0x14, 0x08, 0x14, 0x63 },
  /* 'Y' */
   { 0x07, 0x08, 0x70, 0x08, 0x07 },
  /* 'Z' */
   { 0x61, 0x51, 0x49, 0x45, 0x43 },
  /* '[' */
   { 0x00, 0x7f, 0x41, 0x41, 0x00 },
  /* '\\'*/
   { 0x02, 0x04, 0x08, 0x10, 0x20 },
  /* ']' */
   { 0x00, 0x41, 0x41, 0x7f, 0x00 },
  /* '^' */
   { 0x04, 0x02, 0x01, 0x02, 0x04 },
  /* '_' */
   { 0x40, 0x40, 0x40, 0x40, 0x40 },
  /* '`' */
   { 0x00, 0x01, 0x02, 0x04, 0x00 },
  /* 'a' */
   { 0x20, 0x54, 0x54, 0x54, 0x78 },
  /* 'b' */
   { 0x7f, 0x48, 0x44, 0x44, 0x38 },
  /* 'c' */
   { 0x38, 0x44, 0x44, 0x44, 0x20 },
  /* 'd' */
   { 0x38, 0x44, 0x44, 0x48, 0x7f },
  /* 'e' */
   { 0x38, 0x54, 0x54, 0x54, 0x18 },
  /* 'f' */
   { 0x08, 0x7e, 0x09, 0x01, 0x02 },
  /* 'g' */
   { 0x0c, 0x52, 0x52, 0x52, 0x3e },
  /* 'h' */
   { 0x7f, 0x08, 0x04, 0x04, 0x78 },
  /* 'i' */
   { 0x00, 0x44, 0x7d, 0x40, 0x00 },
  /* 'j' */
   { 0x20, 0x40, 0x44, 0x3d, 0x00 },
  /* 'k' */
   { 0x7f, 0x10, 0x28, 0x44, 0x00 },
  /* 'l' */
   { 0x00, 0x41, 0x7f, 0x40, 0x00 },
  /* 'm' */
   { 0x7c, 0x04, 0x18, 0x04, 0x78 },
  /* 'n' */
   { 0x7c, 0x08, 0x04, 0x04, 0x78 },
  /* 'o' */
   { 0x38, 0x44, 0x44, 0x44, 0x38 },
  /* 'p' */
   { 0x7c, 0x14, 0x14, 0x14, 0x08 },
  /* 'q' */
   { 0x08, 0x14, 0x14, 0x18, 0x7c },
  /* 'r' */
   { 0x7c, 0x08, 0x04, 0x04, 0x08 },
  /* 's' */
   { 0x48, 0x54, 0x54, 0x54, 0x20 },
  /* 't' */
   { 0x04, 0x3f, 0x44, 0x40, 0x20 },
  /* 'u' */
   { 0x3c, 0x40, 0x40, 0x20, 0x7c },
  /* 'v' */
   { 0x1c, 0x20, 0x40, 0x20, 0x1c },
  /* 'w' */
   { 0x3c, 0x40, 0x30, 0x40, 0x3c },
  /* 'x' */
   { 0x44, 0x28, 0x10, 0x28, 0x44 },
  /* 'y' */
   { 0x0c, 0x50, 0x50, 0x50, 0x3c },
  /* 'z' */
   { 0x44, 0x64, 0x54, 0x4c, 0x44 },
  /* '{' */
   { 0x00, 0x08, 0x36, 0x41, 0x00 },
  /* '|' */
   { 0x00, 0x00, 0x7f, 0x00, 0x00 },
  /* '}' */
   { 0x00, 0x41, 0x36, 0x08, 0x00 },
  /* '~' */
   { 0x10, 0x08, 0x08, 0x10, 0x08 },
};

#define FONT_W 6 /* glyph width + 1 px kerning */
#define FONT_H 7

static int text_width(const char *s) {
  int len = 0;
  while(*s++)
    len++;
  return len * FONT_W;
}

static void draw_char(
    uint32_t *px, int stride_px, int x, int y, char c, uint32_t color, int clip_h) {
  if(c < 32 || c > 126) return;
  const uint8_t *glyph = g_font5x7[(uint8_t)(c - 32)];
  for(int col = 0; col < 5; col++) {
    for(int row = 0; row < FONT_H; row++) {
      if(y + row >= clip_h) continue;
      if((glyph[col] >> row) & 1) px[(y + row) * stride_px + x + col] = color;
    }
  }
}

static void draw_text(uint32_t   *px,
                      int         stride_px,
                      int         x,
                      int         y,
                      const char *s,
                      uint32_t    color,
                      int         clip_w,
                      int         clip_h) {
  while(*s) {
    if(x >= clip_w) break;
    draw_char(px, stride_px, x, y, *s, color, clip_h);
    x += FONT_W;
    s++;
  }
}

static void fill_rect_px(uint32_t *px,
                         int       stride_px,
                         int       x,
                         int       y,
                         int       w,
                         int       h,
                         uint32_t  color,
                         int       clip_w,
                         int       clip_h) {
  for(int row = y; row < y + h && row < clip_h; row++)
    for(int col = x; col < x + w && col < clip_w; col++)
      px[row * stride_px + col] = color;
}

/* ── Bar create/destroy ───────────────────────────────────────────────────── */

TrixieBar *bar_create(struct wlr_scene *scene, int w, int h, const Config *cfg) {
  TrixieBar *b = calloc(1, sizeof(*b));
  b->width     = w;
  b->height    = h;
  b->bar_h     = cfg->bar.height;
  b->bar_y     = cfg->bar.position == BAR_BOTTOM ? h - cfg->bar.height : 0;

  int bw = w, bh = b->bar_h;
  b->pixels = calloc(bw * bh, 4);
  b->image  = pixman_image_create_bits(PIXMAN_a8r8g8b8, bw, bh, b->pixels, bw * 4);

  /* create scene buffer at bar position */
  b->scene_buf = wlr_scene_buffer_create(&scene->tree, NULL);
  wlr_scene_node_set_position(&b->scene_buf->node, 0, b->bar_y);

  return b;
}

void bar_resize(TrixieBar *b, int w, int h) {
  b->width  = w;
  b->height = h;
}

void bar_destroy(TrixieBar *b) {
  if(!b) return;
  if(b->image) pixman_image_unref(b->image);
  free(b->pixels);
  wlr_scene_node_destroy(&b->scene_buf->node);
  free(b);
}

/* ── Bar draw helpers ─────────────────────────────────────────────────────── */

typedef struct {
  const char *text;
  Color       fg;
  Color       bg;
  bool        has_bg;
  int         pad; /* horizontal padding in px */
} BarItem;

static int bar_item_width(const BarItem *it) {
  return text_width(it->text) + it->pad * 2;
}

static int draw_bar_item(uint32_t      *px,
                         int            stride,
                         const BarItem *it,
                         int            x,
                         int            y,
                         int            bar_h,
                         Color          bar_bg,
                         int            pill_r) {
  int w      = bar_item_width(it);
  int item_h = bar_h - 4;
  int iy     = (bar_h - item_h) / 2;

  if(it->has_bg) {
    /* pill background */
    uint32_t bg_c = to_argb(it->bg);
    fill_rect_px(px, stride, x, y + iy, w, item_h, bg_c, stride, bar_h);
  }

  /* text, vertically centered */
  int ty = y + (bar_h - FONT_H) / 2;
  draw_text(px, stride, x + it->pad, ty, it->text, to_argb(it->fg), stride, bar_h);

  return w;
}

/* ── System info helpers ──────────────────────────────────────────────────── */

static int read_battery_pct(bool *charging) {
  FILE *f;
  int   pct = -1;
  *charging = false;

  f = fopen("/sys/class/power_supply/BAT0/capacity", "r");
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
  /* find default route interface */
  FILE *f = fopen("/proc/net/route", "r");
  if(!f) {
    strncpy(out, "eth0", max);
    return;
  }
  char iface[32], line[256];
  fgets(line, sizeof(line), f); /* header */
  while(fgets(line, sizeof(line), f)) {
    unsigned flags;
    if(sscanf(line, "%31s %*s %*s %x", iface, &flags) == 2) {
      if(flags & 0x2) {
        strncpy(out, iface, max);
        fclose(f);
        return;
      }
    }
  }
  fclose(f);
  strncpy(out, "eth0", max);
}

/* ── Main update ──────────────────────────────────────────────────────────── */

void bar_update(TrixieBar *b, TwmState *twm, const Config *cfg) {
  if(!b || !twm) return;

  const BarCfg *bar = &cfg->bar;
  int           bw = b->width, bh = b->bar_h;
  if(bw <= 0 || bh <= 0) return;

  uint32_t *px     = b->pixels;
  int       stride = bw;

  /* clear to bar background */
  uint32_t bg_c = to_argb(bar->bg);
  for(int i = 0; i < bw * bh; i++)
    px[i] = bg_c;

  /* separator line */
  if(bar->separator) {
    uint32_t sep_c = to_argb(bar->separator_color);
    int      sep_y = (bar->position == BAR_BOTTOM) ? 0 : bh - 1;
    for(int x = 0; x < bw; x++)
      px[sep_y * stride + x] = sep_c;
  }

  /* ── Build module item arrays ─────────────────────────────────────────── */
#define MAX_ITEMS 32
  BarItem left_items[MAX_ITEMS], center_items[MAX_ITEMS], right_items[MAX_ITEMS];
  int     left_n = 0, center_n = 0, right_n = 0;

  /* workspace label buffers */
  char ws_labels[MAX_WORKSPACES][8];
  char clock_buf[32], layout_buf[32], bat_buf[32], net_buf[32];

  /* Workspace indicators */
  Workspace *active_ws = &twm->workspaces[twm->active_ws];
  for(int i = 0; i < twm->ws_count; i++) {
    Workspace *ws = &twm->workspaces[i];
    snprintf(ws_labels[i], sizeof(ws_labels[i]), " %d ", i + 1);
    BarItem it = {
      .text = ws_labels[i],
      .pad  = 2,
    };
    if(i == twm->active_ws) {
      it.fg     = bar->active_ws_fg;
      it.bg     = bar->active_ws_bg;
      it.has_bg = true;
    } else if(ws->pane_count > 0) {
      it.fg = bar->occupied_ws_fg;
    } else {
      it.fg = bar->inactive_ws_fg;
    }
    if(left_n < MAX_ITEMS) left_items[left_n++] = it;
  }

  /* Clock */
  {
    time_t     t  = time(NULL);
    struct tm *tm = localtime(&t);
    snprintf(clock_buf, sizeof(clock_buf), " %02d:%02d ", tm->tm_hour, tm->tm_min);
    if(center_n < MAX_ITEMS)
      center_items[center_n++] = (BarItem){
        .text   = clock_buf,
        .fg     = bar->active_ws_fg,
        .bg     = bar->active_ws_bg,
        .has_bg = true,
        .pad    = 2,
      };
  }

  /* Layout */
  {
    const char *lbl = layout_label(active_ws->layout);
    snprintf(layout_buf, sizeof(layout_buf), " %s ", lbl);
    if(right_n < MAX_ITEMS)
      right_items[right_n++] = (BarItem){
        .text = layout_buf,
        .fg   = bar->accent,
        .pad  = 2,
      };
  }

  /* Battery */
  {
    bool charging;
    int  pct = read_battery_pct(&charging);
    if(pct >= 0) {
      const char *icon = charging ? "CHG" : (pct > 50 ? "BAT" : "LOW");
      snprintf(bat_buf, sizeof(bat_buf), "%s %d%%", icon, pct);
      Color fg =
          (pct <= 20 && !charging) ? (Color){ 0xf3, 0x8b, 0xa8, 0xff } : bar->fg;
      if(right_n < MAX_ITEMS)
        right_items[right_n++] = (BarItem){ .text = bat_buf, .fg = fg, .pad = 4 };
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
      if(right_n < MAX_ITEMS)
        right_items[right_n++] = (BarItem){ .text = net_buf, .fg = fg, .pad = 4 };
    }
  }

  /* ── Draw zones ───────────────────────────────────────────────────────── */

  /* Left zone */
  int x = bar->item_spacing > 0 ? bar->item_spacing : 4;
  for(int i = 0; i < left_n; i++)
    x += draw_bar_item(
        px, stride, &left_items[i], x, 0, bh, bar->bg, bar->pill_radius);

  /* Right zone (right-aligned) */
  int right_w = bar->item_spacing > 0 ? bar->item_spacing : 4;
  for(int i = 0; i < right_n; i++)
    right_w += bar_item_width(&right_items[i]) +
               (bar->item_spacing > 0 ? bar->item_spacing : 4);
  int rx = bw - right_w;
  for(int i = 0; i < right_n; i++) {
    int sp = bar->item_spacing > 0 ? bar->item_spacing : 4;
    rx += draw_bar_item(
        px, stride, &right_items[i], rx, 0, bh, bar->bg, bar->pill_radius);
    rx += sp;
  }

  /* Center zone */
  int center_total = 0;
  for(int i = 0; i < center_n; i++)
    center_total += bar_item_width(&center_items[i]);
  int cx = (bw - center_total) / 2;
  for(int i = 0; i < center_n; i++)
    cx += draw_bar_item(
        px, stride, &center_items[i], cx, 0, bh, bar->bg, bar->pill_radius);

  /* ── Upload to scene buffer ───────────────────────────────────────────── */
  struct RawBuffer *rb = raw_buffer_create(bw, bh);
  memcpy(rb->data, b->pixels, bw * bh * 4);
  wlr_scene_buffer_set_buffer(b->scene_buf, &rb->base);
  wlr_buffer_drop(&rb->base);
}
