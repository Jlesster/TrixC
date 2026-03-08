/* overlay_internal.h — Shared primitives between overlay.c and panel files.
 *
 * Include this in overlay.c and any panel *.c that needs to draw.
 * Never include from outside the overlay subsystem.
 */
#pragma once

#define _POSIX_C_SOURCE 200809L
#include "trixie.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <math.h>
#include <stdint.h>
#include <string.h>

/* ── Layout constants ───────────────────────────────────────────────────────
 *
 * ROW_H  — height of one text row including inter-row breathing room.
 *           Previously g_ov_th+6 which packed rows too tightly at typical
 *           font sizes.  Now g_ov_th*2 so each row has a full line-height of
 *           padding around the glyph cap-height — doubles the vertical
 *           density and makes the panel feel spacious rather than crammed.
 *
 * PAD    — horizontal/vertical gutter inside the panel border.
 *           Raised from 12 → 16 so text never feels glued to edges.
 *
 * HEADER_H — tab-bar + rule height.  One full ROW_H plus a 2 px rule gap.
 *
 * COL_GAP  — minimum pixel gap between adjacent columns of text.
 *
 * SECTION_GAP — vertical space between logical sections (e.g. between the
 *               config list and the output pane).
 */
extern int g_ov_th;  /* total glyph height  (ascender − descender) */
extern int g_ov_asc; /* ascender in pixels                          */

#define ROW_H       (g_ov_th * 2)
#define PAD         16
#define HEADER_H    (ROW_H + 6)
#define COL_GAP     20
#define SECTION_GAP 10

/* ── Font state (owned by overlay.c) ────────────────────────────────────── */
extern FT_Library g_ov_ft;
extern FT_Face    g_ov_face;

/* ── Log ring (owned by overlay.c) ──────────────────────────────────────── */
#define LOG_RING_SIZE 512
#define LOG_LINE_MAX  256

typedef struct {
  char lines[LOG_RING_SIZE][LOG_LINE_MAX];
  int  head;
  int  count;
} LogRing;

extern LogRing g_log_ring;

void        log_ring_push(const char *line);
const char *log_ring_get(int idx);

/* ── Time helper (defined in overlay.c) ─────────────────────────────────── */
int64_t ov_now_ms(void);

/* ── Drawing primitives (defined in overlay.c) ───────────────────────────── */
int ov_measure(const char *text);

void ov_draw_text(uint32_t   *px,
                  int         stride,
                  int         x,
                  int         y,
                  int         clip_w,
                  int         clip_h,
                  const char *text,
                  uint8_t     r,
                  uint8_t     g,
                  uint8_t     b,
                  uint8_t     a);

void ov_fill_rect(uint32_t *px,
                  int       stride,
                  int       x,
                  int       y,
                  int       w,
                  int       h,
                  uint8_t   r,
                  uint8_t   g,
                  uint8_t   b,
                  uint8_t   a,
                  int       cw,
                  int       ch);

void ov_fill_border(uint32_t *px,
                    int       stride,
                    int       x,
                    int       y,
                    int       w,
                    int       h,
                    uint8_t   r,
                    uint8_t   g,
                    uint8_t   b,
                    uint8_t   a,
                    int       cw,
                    int       ch);

void draw_cursor_line(uint32_t *px,
                      int       stride,
                      int       px0,
                      int       ry,
                      int       pw,
                      Color     ac,
                      Color     bg,
                      int       cw,
                      int       ch);

void overlay_open_file(const char *path, int line, const OverlayCfg *ov_cfg);
