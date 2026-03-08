/* layout.c — Tiling layout algorithms.
 *
 * Mirrors the Rust LayoutKind enum and the bsp_split / column_split /
 * row_split / spiral_split free functions in twm.rs.  Every algorithm
 * takes a content Rect, a pane count, a main_ratio, and a gap, and
 * writes exactly `n` non-overlapping Rects into `out`.
 *
 * Which layout do I use?
 *
 *   Layout       Description
 *   BSP          Binary-space partition, alternating axis per level
 *   Spiral       Fibonacci / golden-ratio spiral subdivision
 *   Columns      One wide main column, rest stacked on the right
 *   Rows         One tall main row, rest arranged below
 *   ThreeCol     Main pane centred, side stacks left and right
 *   Monocle      Every pane occupies the full area (stacked)
 */
#include "trixie.h"

/* ── Label / cycle ──────────────────────────────────────────────────────────── */

const char *layout_label(Layout l) {
  switch(l) {
    case LAYOUT_BSP: return "BSP";
    case LAYOUT_SPIRAL: return "Spiral";
    case LAYOUT_COLUMNS: return "Columns";
    case LAYOUT_ROWS: return "Rows";
    case LAYOUT_THREECOL: return "ThreeCol";
    case LAYOUT_MONOCLE: return "Monocle";
    default: return "?";
  }
}

Layout layout_next(Layout l) {
  return (l + 1) % LAYOUT_COUNT;
}
Layout layout_prev(Layout l) {
  return (l + LAYOUT_COUNT - 1) % LAYOUT_COUNT;
}

/* ── BSP ────────────────────────────────────────────────────────────────────── */

static void
bsp_inner(Rect area, int n, int gap, bool split_vert, Rect *out, int *idx) {
  if(n == 0) return;
  if(n == 1) {
    out[(*idx)++] = area;
    return;
  }

  if(split_vert) {
    int  lw       = (area.w - gap) / 2;
    int  rw       = area.w - lw - gap;
    Rect left     = { area.x, area.y, lw, area.h };
    Rect right    = { area.x + lw + gap, area.y, rw, area.h };
    out[(*idx)++] = left;
    bsp_inner(right, n - 1, gap, false, out, idx);
  } else {
    int  th       = (area.h - gap) / 2;
    int  bh       = area.h - th - gap;
    Rect top      = { area.x, area.y, area.w, th };
    Rect bot      = { area.x, area.y + th + gap, area.w, bh };
    out[(*idx)++] = top;
    bsp_inner(bot, n - 1, gap, true, out, idx);
  }
}

static void layout_bsp(Rect area, int n, int gap, Rect *out) {
  int idx = 0;
  bsp_inner(area, n, gap, area.w >= area.h, out, &idx);
}

/* ── Spiral ─────────────────────────────────────────────────────────────────── */

static void layout_spiral(Rect area, int n, int gap, Rect *out) {
  Rect rem = area;
  for(int i = 0; i < n; i++) {
    if(i == n - 1) {
      out[i] = rem;
      break;
    }
    switch(i % 4) {
      case 0: {
        int lw = (rem.w - gap) / 2;
        out[i] = (Rect){ rem.x, rem.y, lw, rem.h };
        rem    = (Rect){ rem.x + lw + gap, rem.y, rem.w - lw - gap, rem.h };
        break;
      }
      case 1: {
        int th = (rem.h - gap) / 2;
        out[i] = (Rect){ rem.x, rem.y, rem.w, th };
        rem    = (Rect){ rem.x, rem.y + th + gap, rem.w, rem.h - th - gap };
        break;
      }
      case 2: {
        int lw = (rem.w - gap) / 2;
        int rw = rem.w - lw - gap;
        out[i] = (Rect){ rem.x + lw + gap, rem.y, rw, rem.h };
        rem    = (Rect){ rem.x, rem.y, lw, rem.h };
        break;
      }
      default: {
        int th = (rem.h - gap) / 2;
        int bh = rem.h - th - gap;
        out[i] = (Rect){ rem.x, rem.y + th + gap, rem.w, bh };
        rem    = (Rect){ rem.x, rem.y, rem.w, th };
        break;
      }
    }
  }
}

/* ── Columns ────────────────────────────────────────────────────────────────── */

static void layout_columns(Rect area, int n, float ratio, int gap, Rect *out) {
  if(n == 1) {
    out[0] = area;
    return;
  }
  int main_w = (int)(area.w * ratio);
  if(main_w < 4) main_w = 4;
  int side_w = area.w - main_w - gap;
  int rest   = n - 1;
  int gaps   = gap * (rest - 1);
  int each_h = (area.h - gaps) / rest;

  out[0] = (Rect){ area.x, area.y, main_w, area.h };
  for(int i = 0; i < rest; i++) {
    int y      = area.y + i * (each_h + gap);
    int h      = (i + 1 == rest) ? (area.y + area.h - y) : each_h;
    out[i + 1] = (Rect){ area.x + main_w + gap, y, side_w, h };
  }
}

/* ── Rows ───────────────────────────────────────────────────────────────────── */

static void layout_rows(Rect area, int n, float ratio, int gap, Rect *out) {
  if(n == 1) {
    out[0] = area;
    return;
  }
  int main_h = (int)(area.h * ratio);
  if(main_h < 3) main_h = 3;
  int side_h = area.h - main_h - gap;
  int rest   = n - 1;
  int gaps   = gap * (rest - 1);
  int each_w = (area.w - gaps) / rest;

  out[0] = (Rect){ area.x, area.y, area.w, main_h };
  for(int i = 0; i < rest; i++) {
    int x      = area.x + i * (each_w + gap);
    int w      = (i + 1 == rest) ? (area.x + area.w - x) : each_w;
    out[i + 1] = (Rect){ x, area.y + main_h + gap, w, side_h };
  }
}

/* ── ThreeCol helper ────────────────────────────────────────────────────────── */

static void stack_col(Rect area, int sx, int sw, int count, int gap, Rect *out) {
  if(count == 0) return;
  int gaps   = gap * (count - 1);
  int each_h = (area.h - gaps) / count;
  for(int i = 0; i < count; i++) {
    int y  = area.y + i * (each_h + gap);
    int h  = (i + 1 == count) ? (area.y + area.h - y) : each_h;
    out[i] = (Rect){ sx, y, sw, h };
  }
}

static void layout_threecol(Rect area, int n, float ratio, int gap, Rect *out) {
  if(n == 1) {
    out[0] = area;
    return;
  }
  if(n == 2) {
    layout_columns(area, n, ratio, gap, out);
    return;
  }

  int main_w = (int)(area.w * ratio);
  if(main_w < 4) main_w = 4;
  int side_w = (area.w - main_w - gap * 2) / 2;
  int cx     = area.x + side_w + gap;

  out[0] = (Rect){ cx, area.y, main_w, area.h }; /* centre main */

  int left_n  = (n - 1) / 2;
  int right_n = (n - 1) - left_n;

  stack_col(area, area.x, side_w, left_n, gap, out + 1);
  stack_col(area, cx + main_w + gap, side_w, right_n, gap, out + 1 + left_n);
}

/* ── Public entry ───────────────────────────────────────────────────────────── */

void layout_compute(Layout l, Rect area, int n, float ratio, int gap, Rect *out) {
  if(n == 0) return;
  switch(l) {
    case LAYOUT_BSP: layout_bsp(area, n, gap, out); break;
    case LAYOUT_SPIRAL: layout_spiral(area, n, gap, out); break;
    case LAYOUT_COLUMNS: layout_columns(area, n, ratio, gap, out); break;
    case LAYOUT_ROWS: layout_rows(area, n, ratio, gap, out); break;
    case LAYOUT_THREECOL: layout_threecol(area, n, ratio, gap, out); break;
    case LAYOUT_MONOCLE:
      for(int i = 0; i < n; i++)
        out[i] = area;
      break;
    default: layout_bsp(area, n, gap, out); break;
  }
}
