/* lsp_panel.c — Aggregated LSP diagnostics panel for the Trixie overlay.
 *
 * This panel ([L] tab) supplements the per-file nvim panel with a project-wide
 * view of all LSP diagnostics across every language server active in nvim.
 *
 * Layout (three-pane: severity filter left | diagnostics centre | detail right):
 *
 *   ┌─ filter ──┬─ diagnostics ──────────────────────────┬─ detail ───────┐
 *   │ [E] Error │  main.c:42   undefined reference to … │  main.c        │
 *   │ [W] Warn  │  lib.rs:18   cannot borrow `x` as …   │  line 42       │
 *   │ [I] Info  │  App.java:7  method not found: foo()  │  clangd        │
 *   │ [H] Hint  │                                        │                │
 *   └───────────┴────────────────────────────────────────┴────────────────┘
 *
 * Features
 * ────────
 *   • Severity filter toggles (E/W/I/H keys)
 *   • Sort by severity (default) or by file (f key)
 *   • Grouping by file: press g to toggle
 *   • Enter → jump in nvim to diagnostic location
 *   • Diagnostic count badge shown in overlay tab bar
 *
 * Keys
 * ────
 *   j / k          navigate
 *   Enter          jump to location in nvim
 *   e              toggle error filter
 *   w              toggle warning filter
 *   i              toggle info filter
 *   h              toggle hint filter (h also navigates; use H to toggle hint)
 *   f              toggle sort: severity vs file
 *   g              toggle group-by-file
 *   r              force refresh from nvim
 *   Esc            reset filters / deselect
 */

#include "nvim_panel.h"
#define _POSIX_C_SOURCE 200809L
#include "nvim_panel.h"
#include "overlay_internal.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>

/* Forward-declare the nvim side types we depend on */
void nvim_panel_poll(const OverlayCfg *);
bool nvim_panel_key(int *, xkb_keysym_t, const OverlayCfg *);
void nvim_open_file(const char *, int);

/* Filled by lsp_snapshot_copy() which locks g_nv.lock briefly */
#define LSP_SNAP_MAX 256
static LspDiagSnapshot g_snap[LSP_SNAP_MAX];
static int             g_snap_count = 0;
static pthread_mutex_t g_snap_lock  = PTHREAD_MUTEX_INITIALIZER;

/* ── Pull a snapshot from nvim_panel's live state ──────────────────────── */
/* Declared extern — nvim_panel.c fills this in via its locking accessor.   */
extern void nvim_get_diag_snapshot(LspDiagSnapshot *out, int max, int *count);

/* ── Filter state ────────────────────────────────────────────────────────── */
typedef struct {
  bool show_error;
  bool show_warn;
  bool show_info;
  bool show_hint;
  bool sort_by_file; /* false = sort by severity */
  bool group_by_file;
} LspFilter;

static LspFilter g_lsp_filter = {
  .show_error    = true,
  .show_warn     = true,
  .show_info     = true,
  .show_hint     = false,
  .sort_by_file  = false,
  .group_by_file = false,
};

/* Filtered + sorted view (indices into g_snap) */
static int g_view[LSP_SNAP_MAX];
static int g_view_count = 0;

static void lsp_rebuild_view(void) {
  g_view_count = 0;
  pthread_mutex_lock(&g_snap_lock);
  for(int i = 0; i < g_snap_count && g_view_count < LSP_SNAP_MAX; i++) {
    int sev = g_snap[i].sev;
    if(sev == 1 && !g_lsp_filter.show_error) continue;
    if(sev == 2 && !g_lsp_filter.show_warn) continue;
    if(sev == 3 && !g_lsp_filter.show_info) continue;
    if(sev == 4 && !g_lsp_filter.show_hint) continue;
    g_view[g_view_count++] = i;
  }
  /* Sort */
  for(int i = 1; i < g_view_count; i++) {
    int key = g_view[i];
    int j   = i - 1;
    while(j >= 0) {
      bool before;
      if(g_lsp_filter.sort_by_file) {
        int cmp = strcmp(g_snap[g_view[j]].file, g_snap[key].file);
        before  = cmp > 0 || (cmp == 0 && g_snap[g_view[j]].line > g_snap[key].line);
      } else {
        before = g_snap[g_view[j]].sev > g_snap[key].sev;
      }
      if(!before) break;
      g_view[j + 1] = g_view[j];
      j--;
    }
    g_view[j + 1] = key;
  }
  pthread_mutex_unlock(&g_snap_lock);
}

static void lsp_refresh(void) {
  pthread_mutex_lock(&g_snap_lock);
  nvim_get_diag_snapshot(g_snap, LSP_SNAP_MAX, &g_snap_count);
  pthread_mutex_unlock(&g_snap_lock);
  lsp_rebuild_view();
}

/* ── Severity helpers ────────────────────────────────────────────────────── */
static void sev_color(int sev, uint8_t *r, uint8_t *g, uint8_t *b) {
  switch(sev) {
    case 1:
      *r = 0xf3;
      *g = 0x8b;
      *b = 0xa8;
      break;
    case 2:
      *r = 0xf9;
      *g = 0xe2;
      *b = 0xaf;
      break;
    case 3:
      *r = 0x89;
      *g = 0xdc;
      *b = 0xeb;
      break;
    case 4:
      *r = 0xa6;
      *g = 0xe3;
      *b = 0xa1;
      break;
    default:
      *r = 0x6c;
      *g = 0x70;
      *b = 0x86;
      break;
  }
}
static const char *sev_label(int sev) {
  switch(sev) {
    case 1: return "E";
    case 2: return "W";
    case 3: return "I";
    case 4: return "H";
    default: return "?";
  }
}
static const char *sev_name(int sev) {
  switch(sev) {
    case 1: return "error";
    case 2: return "warning";
    case 3: return "info";
    case 4: return "hint";
    default: return "?";
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Draw
 * ═══════════════════════════════════════════════════════════════════════════ */

void draw_panel_lsp(uint32_t         *px,
                    int               stride,
                    int               px0,
                    int               py0,
                    int               pw,
                    int               ph,
                    int              *cursor,
                    const Config     *cfg,
                    const OverlayCfg *ov_cfg) {
  (void)ov_cfg;
  Color ac  = cfg->colors.active_border;
  Color bg  = cfg->colors.pane_bg;
  int   y   = py0 + HEADER_H + PAD;
  int   bot = py0 + ph - PAD;

  /* ── Toolbar ── */
  {
    char toolbar[256];
    int  ec = 0, wc = 0;
    pthread_mutex_lock(&g_snap_lock);
    for(int i = 0; i < g_snap_count; i++) {
      if(g_snap[i].sev == 1)
        ec++;
      else if(g_snap[i].sev == 2)
        wc++;
    }
    pthread_mutex_unlock(&g_snap_lock);

    snprintf(
        toolbar,
        sizeof(toolbar),
        " %d   %d  — e/w/i/H=filter  f=sort:%s  g=group:%s  r=refresh  Enter=jump",
        ec,
        wc,
        g_lsp_filter.sort_by_file ? "file" : "sev",
        g_lsp_filter.group_by_file ? "on" : "off");

    uint8_t tr = ec > 0 ? 0xf3 : ac.r;
    uint8_t tg = ec > 0 ? 0x8b : ac.g;
    uint8_t tb = ec > 0 ? 0xa8 : ac.b;
    ov_draw_text(
        px, stride, px0 + PAD, y + g_ov_asc, stride, bot, toolbar, tr, tg, tb, 0xff);
    y += ROW_H;
  }

  /* ── Filter badges ── */
  {
    struct {
      const char *label;
      bool       *on;
      int         sev;
    } filters[] = {
      { "[E]error", &g_lsp_filter.show_error, 1 },
      { "[W]warn",  &g_lsp_filter.show_warn,  2 },
      { "[I]info",  &g_lsp_filter.show_info,  3 },
      { "[H]hint",  &g_lsp_filter.show_hint,  4 },
    };
    int bx = px0 + PAD;
    for(int i = 0; i < 4; i++) {
      uint8_t r, g, b;
      sev_color(filters[i].sev, &r, &g, &b);
      ov_draw_text(px,
                   stride,
                   bx,
                   y + g_ov_asc,
                   stride,
                   bot,
                   filters[i].label,
                   r,
                   g,
                   b,
                   *filters[i].on ? 0xff : 0x38);
      bx += ov_measure(filters[i].label) + COL_GAP;
    }
    y += ROW_H;
  }

  /* Separator */
  ov_fill_rect(px,
               stride,
               px0 + PAD,
               y,
               pw - PAD * 2,
               1,
               ac.r,
               ac.g,
               ac.b,
               0x35,
               stride,
               bot);
  y += SECTION_GAP;

  /* ── Diagnostic list ── */
  if(g_view_count == 0) {
    const char *empty = g_snap_count == 0 ? "no diagnostics — press r to refresh"
                                          : "all diagnostics filtered";
    ov_draw_text(px,
                 stride,
                 px0 + PAD,
                 y + g_ov_asc,
                 stride,
                 bot,
                 empty,
                 0x45,
                 0x47,
                 0x5a,
                 0xff);
    goto done;
  }

  {
    int visible = (bot - y) / ROW_H;
    if(visible < 1) goto done;

    if(*cursor < 0) *cursor = 0;
    if(*cursor >= g_view_count) *cursor = g_view_count - 1;

    int scroll = *cursor - visible + 1;
    if(scroll < 0) scroll = 0;

    const char *last_file = NULL;

    for(int i = 0; i < visible; i++) {
      int vi = i + scroll;
      if(vi >= g_view_count) break;
      int              si  = g_view[vi];
      LspDiagSnapshot *d   = &g_snap[si];
      bool             sel = (vi == *cursor);
      int              ry  = y + i * ROW_H;

      if(sel) draw_cursor_line(px, stride, px0, ry, pw, ac, bg, stride, bot);

      /* Group header */
      const char *base = strrchr(d->file, '/');
      base             = base ? base + 1 : d->file;
      if(g_lsp_filter.group_by_file && base != last_file) {
        last_file = base;
        if(i > 0) {
          ov_fill_rect(px,
                       stride,
                       px0 + PAD,
                       ry - 1,
                       pw - PAD * 2,
                       1,
                       ac.r,
                       ac.g,
                       ac.b,
                       0x20,
                       stride,
                       bot);
        }
        ov_draw_text(px,
                     stride,
                     px0 + PAD,
                     ry + g_ov_asc,
                     stride,
                     bot,
                     base,
                     ac.r,
                     ac.g,
                     ac.b,
                     0xa0);
        i++;
        ry = y + i * ROW_H;
        if(i >= visible) break;
      }

      /* Severity badge */
      uint8_t sr, sg, sb;
      sev_color(d->sev, &sr, &sg, &sb);
      ov_draw_text(px,
                   stride,
                   px0 + PAD,
                   ry + g_ov_asc + 2,
                   stride,
                   bot,
                   sev_label(d->sev),
                   sr,
                   sg,
                   sb,
                   0xff);

      /* file:line */
      char loc[280];
      snprintf(loc, sizeof(loc), "%s:%d", base, d->line);
      int loc_x = px0 + PAD + 18;
      ov_draw_text(px,
                   stride,
                   loc_x,
                   ry + g_ov_asc + 2,
                   stride,
                   bot,
                   loc,
                   sel ? ac.r : 0x89,
                   sel ? ac.g : 0xdc,
                   sel ? ac.b : 0xeb,
                   0xff);

      /* source */
      if(d->source[0]) {
        int  src_x = loc_x + ov_measure(loc) + COL_GAP;
        char srcbuf[72];
        snprintf(srcbuf, sizeof(srcbuf), "[%s]", d->source);
        ov_draw_text(px,
                     stride,
                     src_x,
                     ry + g_ov_asc + 2,
                     stride,
                     bot,
                     srcbuf,
                     0x45,
                     0x47,
                     0x5a,
                     0xff);
        loc_x = src_x + ov_measure(srcbuf) + COL_GAP;
      } else {
        loc_x += ov_measure(loc) + COL_GAP;
      }

      /* message */
      char trunc[256];
      strncpy(trunc, d->msg, sizeof(trunc) - 1);
      int avail = pw - PAD * 2 - (loc_x - px0);
      while(ov_measure(trunc) > avail && strlen(trunc) > 3)
        trunc[strlen(trunc) - 1] = '\0';
      ov_draw_text(px,
                   stride,
                   loc_x,
                   ry + g_ov_asc + 2,
                   stride,
                   bot,
                   trunc,
                   sr,
                   sg,
                   sb,
                   sel ? 0xff : 0xb0);
    }

    /* Count indicator */
    char cnt[32];
    snprintf(cnt, sizeof(cnt), "%d/%d", *cursor + 1, g_view_count);
    ov_draw_text(px,
                 stride,
                 px0 + pw - PAD - ov_measure(cnt),
                 y + g_ov_asc,
                 stride,
                 bot,
                 cnt,
                 0x45,
                 0x47,
                 0x5a,
                 0xff);
  }

done:
  (void)bg;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Key handler
 * ═══════════════════════════════════════════════════════════════════════════ */

bool lsp_panel_key(int *cursor, xkb_keysym_t sym, const OverlayCfg *ov_cfg) {
  if(sym == XKB_KEY_j || sym == XKB_KEY_Down) {
    (*cursor)++;
    return true;
  }
  if(sym == XKB_KEY_k || sym == XKB_KEY_Up) {
    if(*cursor > 0) (*cursor)--;
    return true;
  }

  if(sym == XKB_KEY_Return) {
    pthread_mutex_lock(&g_snap_lock);
    if(*cursor >= 0 && *cursor < g_view_count) {
      LspDiagSnapshot d = g_snap[g_view[*cursor]];
      pthread_mutex_unlock(&g_snap_lock);
      nvim_open_file(d.file, d.line);
    } else {
      pthread_mutex_unlock(&g_snap_lock);
    }
    return true;
  }

  if(sym == XKB_KEY_e) {
    g_lsp_filter.show_error = !g_lsp_filter.show_error;
    lsp_rebuild_view();
    return true;
  }
  if(sym == XKB_KEY_w) {
    g_lsp_filter.show_warn = !g_lsp_filter.show_warn;
    lsp_rebuild_view();
    return true;
  }
  if(sym == XKB_KEY_i) {
    g_lsp_filter.show_info = !g_lsp_filter.show_info;
    lsp_rebuild_view();
    return true;
  }
  if(sym == XKB_KEY_H) { /* capital H to avoid nav conflict */
    g_lsp_filter.show_hint = !g_lsp_filter.show_hint;
    lsp_rebuild_view();
    return true;
  }
  if(sym == XKB_KEY_f) {
    g_lsp_filter.sort_by_file = !g_lsp_filter.sort_by_file;
    lsp_rebuild_view();
    return true;
  }
  if(sym == XKB_KEY_g) {
    g_lsp_filter.group_by_file = !g_lsp_filter.group_by_file;
    return true;
  }
  if(sym == XKB_KEY_r) {
    lsp_refresh();
    nvim_panel_poll(ov_cfg);
    return true;
  }
  if(sym == XKB_KEY_Escape) {
    *cursor                 = 0;
    g_lsp_filter.show_error = true;
    g_lsp_filter.show_warn  = true;
    g_lsp_filter.show_info  = true;
    g_lsp_filter.show_hint  = false;
    lsp_rebuild_view();
    return true;
  }

  return true;
}

/* Called from overlay_update() each frame */
void lsp_panel_tick(const OverlayCfg *ov_cfg) {
  nvim_panel_poll(ov_cfg);
  lsp_refresh();
}

/* Badge string for the tab bar — declared in lsp_panel.h */
const char *lsp_tab_badge(void) {
  static char badge[16];
  int         ec = nvim_error_count();
  int         wc = nvim_warn_count();
  if(ec > 0) {
    snprintf(badge, sizeof(badge), " %d", ec);
    return badge;
  }
  if(wc > 0) {
    snprintf(badge, sizeof(badge), " %d", wc);
    return badge;
  }
  return "";
}
