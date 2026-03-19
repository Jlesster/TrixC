/* twm.c — Tiling window manager state: panes, workspaces, scratchpads.
 *
 *   twm_init / twm_resize / twm_reflow     — lifecycle
 *   twm_open / twm_close                   — pane management
 *   twm_focused_id / twm_focused           — focus queries
 *   twm_set_focused / twm_focus_dir        — focus mutation
 *   twm_toggle_float / twm_float_move / … — float helpers
 *   twm_switch_ws / twm_move_to_ws        — workspace routing
 *   twm_swap / twm_swap_main               — pane reordering
 *   twm_register_scratch / twm_try_assign_scratch / twm_toggle_scratch
 *   twm_scratch_notify_title               — re-check title-matched scratchpads
 */
#include "trixie.h"
#include <ctype.h>
#include <fnmatch.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* ── ID generator ─────────────────────────────────────────────────────────────
 */

static uint32_t g_next_id = 1;
PaneId new_pane_id(void) { return g_next_id++; }

/* ── Helpers ──────────────────────────────────────────────────────────────────
 */

/* ── O(1) pane lookup — flat open-addressing hash (power-of-2 slots) ─────── */
/* Slot count must be >= MAX_PANES * 2 and a power of two.                    */
#define PANE_HT_BITS 8 /* 256 slots, MAX_PANES <= 64    */
#define PANE_HT_SIZE (1 << PANE_HT_BITS)
#define PANE_HT_MASK (PANE_HT_SIZE - 1)

static Pane *s_ht_pane[PANE_HT_SIZE]; /* pointer into t->panes[]           */
static PaneId s_ht_key[PANE_HT_SIZE]; /* stored id (0 = empty slot)        */
static bool s_ht_valid = false;

static inline void pane_ht_clear(void) {
  memset(s_ht_pane, 0, sizeof(s_ht_pane));
  memset(s_ht_key, 0, sizeof(s_ht_key));
  s_ht_valid = true;
}

static inline void pane_ht_insert(PaneId id, Pane *p) {
  if (!s_ht_valid)
    return;
  uint32_t slot = id & PANE_HT_MASK;
  while (s_ht_key[slot] && s_ht_key[slot] != id)
    slot = (slot + 1) & PANE_HT_MASK;
  s_ht_key[slot] = id;
  s_ht_pane[slot] = p;
}

static inline void pane_ht_remove(PaneId id) {
  if (!s_ht_valid)
    return;
  uint32_t slot = id & PANE_HT_MASK;
  while (s_ht_key[slot] && s_ht_key[slot] != id)
    slot = (slot + 1) & PANE_HT_MASK;
  if (!s_ht_key[slot])
    return;
  /* Tombstone removal: rehash the run. */
  s_ht_key[slot] = 0;
  s_ht_pane[slot] = NULL;
  slot = (slot + 1) & PANE_HT_MASK;
  while (s_ht_key[slot]) {
    PaneId k = s_ht_key[slot];
    Pane *v = s_ht_pane[slot];
    s_ht_key[slot] = 0;
    s_ht_pane[slot] = NULL;
    pane_ht_insert(k, v);
    slot = (slot + 1) & PANE_HT_MASK;
  }
}

/* Rebuild the hash table from the current pane array (called after compaction).
 * Kept for future use — suppress unused-function warning with __attribute__.
 */
static void pane_ht_rebuild(TwmState *t) __attribute__((unused));
static void pane_ht_rebuild(TwmState *t) {
  pane_ht_clear();
  for (int i = 0; i < t->pane_count; i++)
    pane_ht_insert(t->panes[i].id, &t->panes[i]);
}

Pane *twm_pane_by_id(TwmState *t, PaneId id) {
  if (!id)
    return NULL;
  if (s_ht_valid) {
    uint32_t slot = id & PANE_HT_MASK;
    while (s_ht_key[slot]) {
      if (s_ht_key[slot] == id)
        return s_ht_pane[slot];
      slot = (slot + 1) & PANE_HT_MASK;
    }
    return NULL;
  }
  /* Fallback linear scan (hash not yet built). */
  for (int i = 0; i < t->pane_count; i++)
    if (t->panes[i].id == id)
      return &t->panes[i];
  return NULL;
}

PaneId twm_focused_id(TwmState *t) {
  Workspace *ws = &t->workspaces[t->active_ws];
  return ws->has_focused ? ws->focused : 0;
}

Pane *twm_focused(TwmState *t) {
  PaneId id = twm_focused_id(t);
  return id ? twm_pane_by_id(t, id) : NULL;
}

/* Compute content and bar rects from screen dimensions + bar config. */
static Rect compute_content_rect(int sw, int sh, int bar_h, bool bar_bottom,
                                 int pad, Rect *bar_out) {
  Rect content, bar;
  if (bar_h <= 0) {
    content = (Rect){0, 0, sw, sh};
    bar = (Rect){0, 0, 0, 0};
  } else {
    if (bar_h > sh)
      bar_h = sh;
    int ch = sh - bar_h;
    if (bar_bottom) {
      content = (Rect){0, 0, sw, ch};
      bar = (Rect){0, ch, sw, bar_h};
    } else {
      content = (Rect){0, bar_h, sw, ch};
      bar = (Rect){0, 0, sw, bar_h};
    }
  }
  if (bar_out)
    *bar_out = bar;
  if (pad > 0)
    content = rect_inset(content, pad);
  return content;
}

/* ── Init ─────────────────────────────────────────────────────────────────────
 */

/* ── Init ─────────────────────────────────────────────────────────────────────
 */

void twm_init(TwmState *t, int w, int h, int bar_h, bool bar_bottom, int gap,
              int border_w, int pad, int ws_count, bool smart_gaps) {
  memset(t, 0, sizeof(*t));
  pane_ht_clear();
  t->screen_w = w;
  t->screen_h = h;
  t->gap = gap;
  t->border_w = border_w;
  t->padding = pad;
  t->smart_gaps = smart_gaps;

  if (ws_count < 1)
    ws_count = 1;
  if (ws_count > MAX_WORKSPACES)
    ws_count = MAX_WORKSPACES;
  t->ws_count = ws_count;

  for (int i = 0; i < ws_count; i++) {
    t->workspaces[i].index = i;
    t->workspaces[i].layout = LAYOUT_DWINDLE;
    t->workspaces[i].main_ratio = 0.55f;
    t->workspaces[i].gap = gap;
    /* DwindleTree.root must be DWINDLE_NULL (-1), not 0 from memset. */
    dwindle_clear(&t->workspaces[i].dwindle);
  }

  t->content_rect =
      compute_content_rect(w, h, bar_h, bar_bottom, pad, &t->bar_rect);
  t->bar_visible = true;
}

void twm_resize(TwmState *t, int w, int h) {
  t->screen_w = w;
  t->screen_h = h;
  int bar_h = t->bar_rect.h;
  bool bar_bottom = t->bar_rect.y > 0;
  t->content_rect =
      compute_content_rect(w, h, bar_h, bar_bottom, t->padding, &t->bar_rect);
  twm_reflow(t);
}

void twm_set_bar_height(TwmState *t, int h, bool at_bottom) {
  t->content_rect = compute_content_rect(t->screen_w, t->screen_h, h, at_bottom,
                                         t->padding, &t->bar_rect);
  twm_reflow(t);
}

/* ── Reflow ───────────────────────────────────────────────────────────────────
 */

static void reflow_workspace(TwmState *t, int ws_idx) {
  Workspace *ws = &t->workspaces[ws_idx];
  if (ws->pane_count == 0)
    return;

  /* Build ordered list of tiled panes (floating / fullscreen excluded). */
  PaneId tiled[MAX_PANES];
  int tiled_n = 0;
  for (int i = 0; i < ws->pane_count; i++) {
    PaneId pid = ws->panes[i];
    Pane *p = twm_pane_by_id(t, pid);
    if (p && !p->floating && !p->fullscreen)
      tiled[tiled_n++] = pid;
  }

  int eff_gap = (t->smart_gaps && tiled_n <= 1) ? 0 : t->gap;
  Rect rects[MAX_PANES];

  if (tiled_n > 0) {
    if (ws->layout == LAYOUT_DWINDLE) {
      /* Synchronise the BSP tree to exactly match the live tiled[] list:
       * prune stale leaves (floated / moved / closed panes) and insert any
       * newly tiled panes.  This is the single authoritative reconciliation
       * point — no other code needs to manually add or remove tree nodes.  */
      dwindle_sync(&ws->dwindle, tiled, tiled_n,
                   ws->has_focused ? ws->focused : 0);
      dwindle_recompute(&ws->dwindle, t->content_rect, eff_gap);
      for (int i = 0; i < tiled_n; i++) {
        Pane *p = twm_pane_by_id(t, tiled[i]);
        if (!p)
          continue;
        if (!dwindle_get_rect(&ws->dwindle, tiled[i], &p->rect))
          p->rect = t->content_rect; /* should never fire after sync */
      }
    } else {
      layout_compute(ws->layout, t->content_rect, tiled_n, ws->main_ratio,
                     eff_gap, rects);
      for (int i = 0; i < tiled_n; i++) {
        Pane *p = twm_pane_by_id(t, tiled[i]);
        if (p)
          p->rect = rects[i];
      }
    }
  }

  /* Fullscreen and floating panes override whatever layout computed. */
  for (int i = 0; i < ws->pane_count; i++) {
    Pane *p = twm_pane_by_id(t, ws->panes[i]);
    if (!p)
      continue;
    if (p->fullscreen)
      p->rect = (Rect){0, 0, t->screen_w, t->screen_h};
    else if (p->floating)
      p->rect = p->float_rect;
  }
}

void twm_reflow(TwmState *t) { reflow_workspace(t, t->active_ws); }

/* ── Pane management ──────────────────────────────────────────────────────────
 */

PaneId twm_open(TwmState *t, const char *app_id) {
  return twm_open_ex(t, app_id, false, false);
}

PaneId twm_open_ex(TwmState *t, const char *app_id, bool floating,
                   bool fullscreen) {
  if (t->pane_count >= MAX_PANES)
    return 0;
  Pane *p = &t->panes[t->pane_count++];
  memset(p, 0, sizeof(*p));
  p->id = new_pane_id();
  p->floating = floating;
  p->fullscreen = fullscreen;
  strncpy(p->app_id, app_id, sizeof(p->app_id) - 1);
  strncpy(p->title, app_id, sizeof(p->title) - 1);

  /* Pre-initialise float_rect so floating windows don't start at 0,0.
   * view_do_map will override this with any forced sizes / rule positions. */
  if (floating) {
    int fw = t->screen_w / 2;
    int fh = t->screen_h / 2;
    p->float_rect =
        (Rect){(t->screen_w - fw) / 2, (t->screen_h - fh) / 2, fw, fh};
    p->rect = p->float_rect;
  }

  pane_ht_insert(p->id, p);

  Workspace *ws = &t->workspaces[t->active_ws];

  PaneId prev_focused = ws->has_focused ? ws->focused : 0;

  ws->panes[ws->pane_count++] = p->id;
  p->ws_idx = t->active_ws;
  ws->focused = p->id;
  ws->has_focused = true;

  if (!floating && !fullscreen && ws->layout == LAYOUT_DWINDLE)
    dwindle_insert(&ws->dwindle, p->id, prev_focused, t->content_rect, t->gap);

  twm_reflow(t);
  return p->id;
}

void twm_close(TwmState *t, PaneId id) {
  pane_ht_remove(id);
  for (int i = 0; i < t->pane_count; i++) {
    if (t->panes[i].id == id) {
      t->panes[i] = t->panes[--t->pane_count];
      if (i < t->pane_count)
        pane_ht_insert(t->panes[i].id, &t->panes[i]);
      break;
    }
  }
  /* Track which workspaces actually contained the pane so we only reflow
   * those — skips reflowing unaffected workspaces entirely. */
  bool ws_dirty[MAX_WORKSPACES] = {false};
  for (int wi = 0; wi < t->ws_count; wi++) {
    Workspace *ws = &t->workspaces[wi];
    for (int i = 0; i < ws->pane_count; i++) {
      if (ws->panes[i] == id) {
        ws->panes[i] = ws->panes[--ws->pane_count];
        ws_dirty[wi] = true;
        break;
      }
    }
    if (ws->has_focused && ws->focused == id) {
      if (ws->pane_count > 0)
        ws->focused = ws->panes[ws->pane_count - 1];
      else
        ws->has_focused = false;
    }
  }
  for (int i = 0; i < t->scratch_count; i++) {
    if (t->scratchpads[i].has_pane && t->scratchpads[i].pane_id == id) {
      t->scratchpads[i].has_pane = false;
      t->scratchpads[i].visible = false;
    }
  }
  for (int wi = 0; wi < t->ws_count; wi++)
    if (ws_dirty[wi] && t->workspaces[wi].pane_count > 0)
      reflow_workspace(t, wi);
}

/* ───────────────────────────────────────────────────────────────────────────
 * twm_set_title — updates title and re-checks title-matched scratchpads.
 * ───────────────────────────────────────────────────────────────────────────
 */

void twm_set_title(TwmState *t, PaneId id, const char *title) {
  Pane *p = twm_pane_by_id(t, id);
  if (!p)
    return;
  if (strncmp(p->title, title, sizeof(p->title) - 1) == 0)
    return;
  strncpy(p->title, title, sizeof(p->title) - 1);
  /* Re-attempt scratchpad assignment now that we have a real title. */
  twm_scratch_notify_title(t, id);
}

void twm_set_focused(TwmState *t, PaneId id) {
  Workspace *ws = &t->workspaces[t->active_ws];
  for (int i = 0; i < ws->pane_count; i++) {
    if (ws->panes[i] == id) {
      ws->focused = id;
      ws->has_focused = true;
      t->ws_urgent_mask &= ~(1u << t->active_ws);
      return;
    }
  }
}

/* ── Float ────────────────────────────────────────────────────────────────────
 */

void twm_toggle_float(TwmState *t) {
  PaneId id = twm_focused_id(t);
  if (!id)
    return;
  Pane *p = twm_pane_by_id(t, id);
  if (!p)
    return;

  p->floating = !p->floating;
  if (p->floating) {
    if (rect_empty(p->float_rect)) {
      int fw = t->screen_w / 2;
      int fh = t->screen_h / 2;
      p->float_rect =
          (Rect){(t->screen_w - fw) / 2, (t->screen_h - fh) / 2, fw, fh};
    }
    p->rect = p->float_rect;
  }
  /* dwindle_sync inside reflow_workspace will prune the now-floating pane's
   * leaf from the tree (or insert the un-floated pane) automatically.      */
  twm_reflow(t);
}

void twm_float_move(TwmState *t, PaneId id, int dx, int dy) {
  Pane *p = twm_pane_by_id(t, id);
  if (!p || !p->floating)
    return;
  p->float_rect.x += dx;
  p->float_rect.y += dy;
  int margin = 32;
  if (p->float_rect.x + p->float_rect.w < margin)
    p->float_rect.x = margin - p->float_rect.w;
  if (p->float_rect.x > t->screen_w - margin)
    p->float_rect.x = t->screen_w - margin;
  if (p->float_rect.y < 0)
    p->float_rect.y = 0;
  if (p->float_rect.y > t->screen_h - margin)
    p->float_rect.y = t->screen_h - margin;
  p->rect = p->float_rect;
}

void twm_float_resize(TwmState *t, PaneId id, int dw, int dh) {
  Pane *p = twm_pane_by_id(t, id);
  if (!p || !p->floating)
    return;
  p->float_rect.w += dw;
  p->float_rect.h += dh;
  if (p->float_rect.w < 80)
    p->float_rect.w = 80;
  if (p->float_rect.h < 60)
    p->float_rect.h = 60;
  p->rect = p->float_rect;
}

/* ── Focus direction — cosine-similarity scoring ──────────────────────────────
 */

void twm_focus_dir(TwmState *t, int dx, int dy) {
  Workspace *ws = &t->workspaces[t->active_ws];
  if (!ws->has_focused)
    return;
  Pane *cur = twm_pane_by_id(t, ws->focused);
  if (!cur)
    return;

  int cx = cur->rect.x + cur->rect.w / 2;
  int cy = cur->rect.y + cur->rect.h / 2;

  PaneId best = 0;
  float best_score = -1.0f;

  for (int i = 0; i < ws->pane_count; i++) {
    PaneId pid = ws->panes[i];
    if (pid == ws->focused)
      continue;
    Pane *p = twm_pane_by_id(t, pid);
    if (!p || p->floating)
      continue;

    int nx = p->rect.x + p->rect.w / 2;
    int ny = p->rect.y + p->rect.h / 2;
    int vx = nx - cx;
    int vy = ny - cy;
    int dot = vx * dx + vy * dy;
    if (dot <= 0)
      continue;

    float dist_sq = (float)(vx * vx + vy * vy);
    float score = (float)dot / sqrtf(dist_sq + 1.0f);
    if (score > best_score) {
      best_score = score;
      best = pid;
    }
  }
  if (best) {
    ws->focused = best;
    ws->has_focused = true;
  }
}

/* ── Swap ─────────────────────────────────────────────────────────────────────
 */

void twm_swap(TwmState *t, bool forward) {
  Workspace *ws = &t->workspaces[t->active_ws];
  if (ws->pane_count < 2 || !ws->has_focused)
    return;

  if (ws->layout == LAYOUT_DWINDLE) {
    dwindle_swap_cycle(&ws->dwindle, ws->focused, forward);
    twm_reflow(t);
    return;
  }

  /* Non-dwindle: cycle position in ws->panes[]. */
  int cur = -1;
  for (int i = 0; i < ws->pane_count; i++)
    if (ws->panes[i] == ws->focused) {
      cur = i;
      break;
    }
  if (cur < 0)
    return;
  int n = ws->pane_count;
  int tgt = forward ? (cur + 1) % n : (cur + n - 1) % n;
  PaneId tmp = ws->panes[cur];
  ws->panes[cur] = ws->panes[tgt];
  ws->panes[tgt] = tmp;
  twm_reflow(t);
}

void twm_swap_main(TwmState *t) {
  Workspace *ws = &t->workspaces[t->active_ws];
  if (ws->pane_count < 2 || !ws->has_focused)
    return;

  if (ws->layout == LAYOUT_DWINDLE) {
    dwindle_swap_main(&ws->dwindle, ws->focused);
    twm_reflow(t);
    return;
  }

  /* Non-dwindle: move focused to index 0 in ws->panes[]. */
  int cur = -1;
  for (int i = 0; i < ws->pane_count; i++)
    if (ws->panes[i] == ws->focused) {
      cur = i;
      break;
    }
  if (cur <= 0)
    return;
  PaneId tmp = ws->panes[0];
  ws->panes[0] = ws->panes[cur];
  ws->panes[cur] = tmp;
  twm_reflow(t);
}

/* twm_swap_dir — move the focused window toward dx,dy in the layout.
 * For dwindle: swaps with the spatially nearest neighbour in that direction.
 * For other layouts: falls back to twm_swap(forward/back) based on sign. */
void twm_swap_dir(TwmState *t, int dx, int dy) {
  Workspace *ws = &t->workspaces[t->active_ws];
  if (ws->pane_count < 2 || !ws->has_focused)
    return;

  if (ws->layout == LAYOUT_DWINDLE) {
    dwindle_swap_dir(&ws->dwindle, ws->focused, dx, dy);
    twm_reflow(t);
    return;
  }

  /* Non-dwindle fallback: treat left/up as backward, right/down as forward. */
  bool forward = (dx > 0 || dy > 0);
  twm_swap(t, forward);
}

/* ── Workspace switching ──────────────────────────────────────────────────────
 */

void twm_switch_ws(TwmState *t, int n) {
  for (int i = 0; i < t->scratch_count; i++) {
    Scratchpad *sp = &t->scratchpads[i];
    if (!sp->visible || !sp->has_pane)
      continue;
    Workspace *ws = &t->workspaces[t->active_ws];
    for (int j = 0; j < ws->pane_count; j++) {
      if (ws->panes[j] == sp->pane_id) {
        ws->panes[j] = ws->panes[--ws->pane_count];
        break;
      }
    }
    sp->visible = false;
  }
  n = n < 0 ? 0 : (n >= t->ws_count ? t->ws_count - 1 : n);
  t->active_ws = n;
  t->ws_urgent_mask &= ~(1u << n);
  twm_reflow(t);
}

void twm_move_to_ws(TwmState *t, int n) {
  PaneId id = twm_focused_id(t);
  if (!id)
    return;
  int target = n < 0 ? 0 : (n >= t->ws_count ? t->ws_count - 1 : n);
  if (target == t->active_ws)
    return;

  Workspace *src = &t->workspaces[t->active_ws];

  for (int i = 0; i < src->pane_count; i++) {
    if (src->panes[i] == id) {
      src->panes[i] = src->panes[--src->pane_count];
      break;
    }
  }
  if (src->has_focused && src->focused == id) {
    src->has_focused = src->pane_count > 0;
    if (src->has_focused)
      src->focused = src->panes[src->pane_count - 1];
  }

  Workspace *dst = &t->workspaces[target];
  dst->panes[dst->pane_count++] = id;
  dst->focused = id;
  dst->has_focused = true;
  {
    Pane *_p = twm_pane_by_id(t, id);
    if (_p)
      _p->ws_idx = target;
  }

  /* Reflow only the two affected workspaces — src lost a pane, dst gained one.
   * All other workspaces are unchanged and don't need reflow. */
  reflow_workspace(t, t->active_ws); /* src — active_ws hasn't changed yet */
  reflow_workspace(t, target);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Scratchpads
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Pattern grammar (stored in Scratchpad.app_id, parsed into .pattern):
 *
 *   [field:]spec
 *
 *   field  : "class:"  | "app_id:"   → match pane->app_id   (default)
 *            "title:"                → match pane->title
 *
 *   spec   : "~<sub>"   case-insensitive substring
 *            "<glob>"   fnmatch() glob (* ? [...])
 *            plain      exact, case-insensitive
 *
 * Examples:
 *   app_id = kitty                  exact app_id match
 *   app_id = title:~ncmpcpp         title contains "ncmpcpp"
 *   app_id = class:fire*            app_id glob
 *   app_id = title:*Music Player*   title glob
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* ── Pattern helpers ──────────────────────────────────────────────────────────
 */

static ScratchPattern scratch_parse_pattern(const char *raw) {
  ScratchPattern sp = {SCRATCH_FIELD_APP_ID, SCRATCH_SPEC_EXACT, ""};
  if (!raw || !raw[0])
    return sp;

  const char *spec = raw;

  if (!strncasecmp(raw, "title:", 6)) {
    sp.field = SCRATCH_FIELD_TITLE;
    spec = raw + 6;
  } else if (!strncasecmp(raw, "app_id:", 7)) {
    sp.field = SCRATCH_FIELD_APP_ID;
    spec = raw + 7;
  } else if (!strncasecmp(raw, "class:", 6)) {
    sp.field = SCRATCH_FIELD_APP_ID;
    spec = raw + 6;
  }

  if (spec[0] == '~') {
    sp.kind = SCRATCH_SPEC_SUBSTR;
    snprintf(sp.pat, sizeof(sp.pat), "%s", spec + 1);
  } else if (strchr(spec, '*') || strchr(spec, '?') || strchr(spec, '[')) {
    sp.kind = SCRATCH_SPEC_GLOB;
    snprintf(sp.pat, sizeof(sp.pat), "%s", spec);
  } else {
    sp.kind = SCRATCH_SPEC_EXACT;
    snprintf(sp.pat, sizeof(sp.pat), "%s", spec);
  }
  return sp;
}

static bool scratch_pattern_matches(const ScratchPattern *sp, const Pane *p) {
  const char *haystack =
      (sp->field == SCRATCH_FIELD_TITLE) ? p->title : p->app_id;
  if (!haystack || !haystack[0])
    return false;

  switch (sp->kind) {
  case SCRATCH_SPEC_EXACT:
    return strcasecmp(haystack, sp->pat) == 0;

  case SCRATCH_SPEC_SUBSTR: {
    /* Case-insensitive substring via lowered copies on the stack. */
    char h[256], n[256];
    strncpy(h, haystack, 255);
    h[255] = '\0';
    strncpy(n, sp->pat, 255);
    n[255] = '\0';
    for (int i = 0; h[i]; i++)
      h[i] = (char)tolower((unsigned char)h[i]);
    for (int i = 0; n[i]; i++)
      n[i] = (char)tolower((unsigned char)n[i]);
    return strstr(h, n) != NULL;
  }

  case SCRATCH_SPEC_GLOB:
    /* FNM_CASEFOLD is a GNU extension; fall back gracefully if absent. */
#ifdef FNM_CASEFOLD
    return fnmatch(sp->pat, haystack, FNM_CASEFOLD) == 0;
#else
    return fnmatch(sp->pat, haystack, 0) == 0;
#endif
  }
  return false;
}

/* ── scratch_rect — clamped to screen ────────────────────────────────────────
 */

static Rect scratch_rect(TwmState *t, Scratchpad *sp) {
  int w = (int)(t->screen_w * sp->width_pct);
  int h = (int)(t->screen_h * sp->height_pct);
  if (w > t->screen_w)
    w = t->screen_w;
  if (h > t->screen_h)
    h = t->screen_h;
  if (w < 80)
    w = 80;
  if (h < 60)
    h = 60;
  return (Rect){(t->screen_w - w) / 2, (t->screen_h - h) / 2, w, h};
}

/* ── Internal: claim a pane for a scratchpad slot ─────────────────────────────
 */

static void scratch_claim(TwmState *t, Scratchpad *sp, PaneId id) {
  sp->pane_id = id;
  sp->has_pane = true;

  Rect r = scratch_rect(t, sp);
  Pane *p = twm_pane_by_id(t, id);
  if (p) {
    p->floating = true;
    p->rect = r;
    p->float_rect = r;
    p->ws_idx = -1; /* scratchpads live outside all workspace pane lists */
  }

  /* Reflow only workspaces that actually contained this pane — not all. */
  bool ws_dirty[MAX_WORKSPACES] = {false};
  for (int wi = 0; wi < t->ws_count; wi++) {
    Workspace *ws = &t->workspaces[wi];
    for (int j = 0; j < ws->pane_count; j++) {
      if (ws->panes[j] == id) {
        ws->panes[j] = ws->panes[--ws->pane_count];
        ws_dirty[wi] = true;
        if (ws->has_focused && ws->focused == id) {
          ws->has_focused = ws->pane_count > 0;
          if (ws->has_focused)
            ws->focused = ws->panes[ws->pane_count - 1];
        }
        break;
      }
    }
  }
  for (int wi = 0; wi < t->ws_count; wi++)
    if (ws_dirty[wi])
      reflow_workspace(t, wi);

  wlr_log(WLR_INFO, "twm: scratch '%s' claimed pane %u  rect=%dx%d+%d+%d",
          sp->name, id, r.w, r.h, r.x, r.y);
}

/* ── twm_register_scratch — upsert semantics ─────────────────────────────────
 */

void twm_register_scratch(TwmState *t, const char *name, const char *app_id,
                          const char *exec_cmd, float wpct, float hpct) {
  /* Upsert: update an existing slot so config reloads don't lose pane state. */
  Scratchpad *sp = NULL;
  for (int i = 0; i < t->scratch_count; i++) {
    if (strcmp(t->scratchpads[i].name, name) == 0) {
      sp = &t->scratchpads[i];
      break;
    }
  }
  if (!sp) {
    if (t->scratch_count >= MAX_SCRATCHPADS) {
      wlr_log(WLR_ERROR, "twm: MAX_SCRATCHPADS reached, cannot register '%s'",
              name);
      return;
    }
    sp = &t->scratchpads[t->scratch_count++];
    memset(sp, 0, sizeof(*sp));
  }

  strncpy(sp->name, name, sizeof(sp->name) - 1);
  if (app_id && app_id[0])
    strncpy(sp->app_id, app_id, sizeof(sp->app_id) - 1);
  if (exec_cmd && exec_cmd[0])
    strncpy(sp->exec, exec_cmd, sizeof(sp->exec) - 1);

  sp->width_pct = (wpct < 0.05f) ? 0.6f : (wpct > 1.0f) ? 1.0f : wpct;
  sp->height_pct = (hpct < 0.05f) ? 0.6f : (hpct > 1.0f) ? 1.0f : hpct;

  /* Pre-parse the pattern once so matching is cheap at runtime. */
  sp->pattern = scratch_parse_pattern(sp->app_id);

  wlr_log(WLR_DEBUG,
          "twm: scratch '%s' registered  app_id='%s'  exec='%s'  %.0f%%x%.0f%%"
          "  field=%s kind=%s pat='%s'",
          sp->name, sp->app_id, sp->exec, sp->width_pct * 100.f,
          sp->height_pct * 100.f,
          sp->pattern.field == SCRATCH_FIELD_TITLE ? "title" : "app_id",
          sp->pattern.kind == SCRATCH_SPEC_SUBSTR ? "substr"
          : sp->pattern.kind == SCRATCH_SPEC_GLOB ? "glob"
                                                  : "exact",
          sp->pattern.pat);
}

/* ── twm_try_assign_scratch ───────────────────────────────────────────────────
 */

bool twm_try_assign_scratch(TwmState *t, PaneId id, const char *app_id) {
  (void)app_id; /* kept for API compat; we always use the live pane fields */
  Pane *p = twm_pane_by_id(t, id);
  if (!p)
    return false;

  for (int i = 0; i < t->scratch_count; i++) {
    Scratchpad *sp = &t->scratchpads[i];
    if (sp->has_pane)
      continue;

    bool match = scratch_pattern_matches(&sp->pattern, p);

    wlr_log(WLR_DEBUG,
            "try_assign_scratch: pane=%u app_id='%s' title='%s'"
            "  sp[%d]='%s' field=%s kind=%s pat='%s'  → %s",
            id, p->app_id, p->title, i, sp->name,
            sp->pattern.field == SCRATCH_FIELD_TITLE ? "title" : "app_id",
            sp->pattern.kind == SCRATCH_SPEC_SUBSTR ? "substr"
            : sp->pattern.kind == SCRATCH_SPEC_GLOB ? "glob"
                                                    : "exact",
            sp->pattern.pat, match ? "MATCH" : "no");

    if (!match)
      continue;

    scratch_claim(t, sp, id);
    return true;
  }
  return false;
}

/* ── twm_scratch_notify_title ─────────────────────────────────────────────────
 */

void twm_scratch_notify_title(TwmState *t, PaneId id) {
  Pane *p = twm_pane_by_id(t, id);
  if (!p)
    return;

  /* Already claimed by some scratchpad — nothing to do. */
  for (int i = 0; i < t->scratch_count; i++)
    if (t->scratchpads[i].has_pane && t->scratchpads[i].pane_id == id)
      return;

  /* Only bother trying slots whose pattern targets the title field. */
  for (int i = 0; i < t->scratch_count; i++) {
    Scratchpad *sp = &t->scratchpads[i];
    if (sp->has_pane)
      continue;
    if (sp->pattern.field != SCRATCH_FIELD_TITLE)
      continue;

    bool match = scratch_pattern_matches(&sp->pattern, p);

    wlr_log(WLR_DEBUG,
            "scratch_notify_title: pane=%u title='%s'"
            "  sp[%d]='%s' pat='%s'  → %s",
            id, p->title, i, sp->name, sp->pattern.pat, match ? "MATCH" : "no");

    if (!match)
      continue;
    scratch_claim(t, sp, id);
    return;
  }
}

/* ── twm_toggle_scratch ───────────────────────────────────────────────────────
 */

void twm_toggle_scratch(TwmState *t, const char *name) {
  Scratchpad *sp = NULL;
  for (int i = 0; i < t->scratch_count; i++) {
    if (strcmp(t->scratchpads[i].name, name) == 0) {
      sp = &t->scratchpads[i];
      break;
    }
  }
  if (!sp) {
    wlr_log(WLR_DEBUG, "twm: toggle_scratch '%s': not registered", name);
    return;
  }
  if (!sp->has_pane) {
    /* Caller (server_scratch_toggle) handles spawning. */
    wlr_log(WLR_DEBUG, "twm: toggle_scratch '%s': no pane yet, spawn pending",
            name);
    return;
  }

  Workspace *ws = &t->workspaces[t->active_ws];

  if (sp->visible) {
    /* ── Hide ── */
    for (int i = 0; i < ws->pane_count; i++) {
      if (ws->panes[i] == sp->pane_id) {
        ws->panes[i] = ws->panes[--ws->pane_count];
        break;
      }
    }
    if (ws->has_focused && ws->focused == sp->pane_id) {
      ws->has_focused = ws->pane_count > 0;
      if (ws->has_focused)
        ws->focused = ws->panes[ws->pane_count - 1];
    }
    sp->visible = false;
    wlr_log(WLR_DEBUG, "twm: scratch '%s' hidden", name);
  } else {
    /* ── Show ── */
    if (ws->pane_count >= MAX_PANES) {
      wlr_log(WLR_ERROR, "twm: scratch show '%s': workspace full", name);
      return;
    }
    Rect r = scratch_rect(t, sp);
    Pane *p = twm_pane_by_id(t, sp->pane_id);
    if (p) {
      p->floating = true;
      p->rect = r;
      p->float_rect = r;
      p->ws_idx = t->active_ws;
    }
    ws->panes[ws->pane_count++] = sp->pane_id;
    ws->focused = sp->pane_id;
    ws->has_focused = true;
    sp->visible = true;
    wlr_log(WLR_DEBUG, "twm: scratch '%s' shown  rect=%dx%d+%d+%d", name, r.w,
            r.h, r.x, r.y);
  }
  twm_reflow(t);
}

/* ── ipc_scratch_json ─────────────────────────────────────────────────────────
 */

int ipc_scratch_json(TwmState *t, char *buf, size_t bufsz) {
  int len = 0;
  len += snprintf(buf + len, bufsz - len, ",\"scratchpads\":[");
  for (int i = 0; i < t->scratch_count; i++) {
    Scratchpad *sp = &t->scratchpads[i];
    if (len >= (int)bufsz - 128)
      break;
    len += snprintf(buf + len, bufsz - len,
                    "%s{\"name\":\"%s\",\"app_id\":\"%s\","
                    "\"has_pane\":%s,\"visible\":%s,\"pane_id\":%u,"
                    "\"exec\":\"%s\"}",
                    i > 0 ? "," : "", sp->name, sp->app_id,
                    sp->has_pane ? "true" : "false",
                    sp->visible ? "true" : "false", sp->pane_id, sp->exec);
  }
  len += snprintf(buf + len, bufsz - len, "]");
  return len;
}
