/* twm.c — tiling window manager state: panes, workspaces, scratchpads */
#include "trixie.h"
#include <stdlib.h>
#include <string.h>

/* ── ID generator ─────────────────────────────────────────────────────────── */

static uint32_t g_next_id = 1;
PaneId          new_pane_id(void) {
  return g_next_id++;
}

/* ── Helpers ──────────────────────────────────────────────────────────────── */

Pane *twm_pane_by_id(TwmState *t, PaneId id) {
  for(int i = 0; i < t->pane_count; i++)
    if(t->panes[i].id == id) return &t->panes[i];
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

static Rect
compute_rects(int sw, int sh, int bar_h, bool bar_bottom, int pad, Rect *bar_out) {
  Rect content, bar;
  if(bar_h <= 0) {
    content = (Rect){ 0, 0, sw, sh };
    bar     = (Rect){ 0, 0, 0, 0 };
  } else {
    if(bar_h > sh) bar_h = sh;
    int ch = sh - bar_h;
    if(bar_bottom) {
      content = (Rect){ 0, 0, sw, ch };
      bar     = (Rect){ 0, ch, sw, bar_h };
    } else {
      content = (Rect){ 0, bar_h, sw, ch };
      bar     = (Rect){ 0, 0, sw, bar_h };
    }
  }
  if(bar_out) *bar_out = bar;
  if(pad > 0) content = rect_inset(content, pad);
  return content;
}

/* ── Init ─────────────────────────────────────────────────────────────────── */

void twm_init(TwmState *t,
              int       w,
              int       h,
              int       bar_h,
              bool      bar_bottom,
              int       gap,
              int       border_w,
              int       pad,
              int       ws_count,
              bool      smart_gaps) {
  memset(t, 0, sizeof(*t));
  t->screen_w   = w;
  t->screen_h   = h;
  t->gap        = gap;
  t->border_w   = border_w;
  t->padding    = pad;
  t->smart_gaps = smart_gaps;

  if(ws_count < 1) ws_count = 1;
  if(ws_count > MAX_WORKSPACES) ws_count = MAX_WORKSPACES;
  t->ws_count = ws_count;

  for(int i = 0; i < ws_count; i++) {
    t->workspaces[i].index      = i;
    t->workspaces[i].layout     = LAYOUT_BSP;
    t->workspaces[i].main_ratio = 0.55f;
    t->workspaces[i].gap        = gap;
  }

  t->content_rect = compute_rects(w, h, bar_h, bar_bottom, pad, &t->bar_rect);
  t->bar_visible  = true;
}

void twm_resize(TwmState *t, int w, int h) {
  t->screen_w     = w;
  t->screen_h     = h;
  int  bar_h      = t->bar_rect.h;
  bool bar_bottom = t->bar_rect.y > 0;
  t->content_rect = compute_rects(w, h, bar_h, bar_bottom, t->padding, &t->bar_rect);
  twm_reflow(t);
}

void twm_set_bar_height(TwmState *t, int h, bool at_bottom) {
  t->content_rect = compute_rects(
      t->screen_w, t->screen_h, h, at_bottom, t->padding, &t->bar_rect);
  twm_reflow(t);
}

/* ── Reflow ───────────────────────────────────────────────────────────────── */

void twm_reflow(TwmState *t) {
  Workspace *ws = &t->workspaces[t->active_ws];
  if(ws->pane_count == 0) return;

  /* collect tiled panes */
  PaneId tiled[MAX_PANES];
  int    tiled_n = 0;
  for(int i = 0; i < ws->pane_count; i++) {
    PaneId pid = ws->panes[i];
    Pane  *p   = twm_pane_by_id(t, pid);
    if(p && !p->floating && !p->fullscreen) tiled[tiled_n++] = pid;
  }

  int eff_gap = (t->smart_gaps && tiled_n <= 1) ? 0 : t->gap;

  Rect rects[MAX_PANES];
  if(tiled_n > 0)
    layout_compute(
        ws->layout, t->content_rect, tiled_n, ws->main_ratio, eff_gap, rects);

  for(int i = 0; i < tiled_n; i++) {
    Pane *p = twm_pane_by_id(t, tiled[i]);
    if(p) p->rect = rects[i];
  }

  /* fullscreen / floating overrides */
  for(int i = 0; i < ws->pane_count; i++) {
    Pane *p = twm_pane_by_id(t, ws->panes[i]);
    if(!p) continue;
    if(p->fullscreen)
      p->rect = (Rect){ 0, 0, t->screen_w, t->screen_h };
    else if(p->floating && !p->fullscreen)
      p->rect = p->float_rect;
  }
}

/* ── Pane management ──────────────────────────────────────────────────────── */

PaneId twm_open(TwmState *t, const char *app_id) {
  if(t->pane_count >= MAX_PANES) return 0;
  Pane *p = &t->panes[t->pane_count++];
  memset(p, 0, sizeof(*p));
  p->id   = new_pane_id();
  p->kind = PANE_SHELL;
  strncpy(p->app_id, app_id, sizeof(p->app_id) - 1);
  strncpy(p->title, app_id, sizeof(p->title) - 1);

  Workspace *ws               = &t->workspaces[t->active_ws];
  ws->panes[ws->pane_count++] = p->id;
  ws->focused                 = p->id;
  ws->has_focused             = true;

  twm_reflow(t);
  return p->id;
}

void twm_close(TwmState *t, PaneId id) {
  /* Remove from pane pool */
  for(int i = 0; i < t->pane_count; i++) {
    if(t->panes[i].id == id) {
      t->panes[i] = t->panes[--t->pane_count];
      break;
    }
  }
  /* Remove from all workspaces */
  for(int wi = 0; wi < t->ws_count; wi++) {
    Workspace *ws = &t->workspaces[wi];
    for(int i = 0; i < ws->pane_count; i++) {
      if(ws->panes[i] == id) {
        ws->panes[i] = ws->panes[--ws->pane_count];
        break;
      }
    }
    if(ws->has_focused && ws->focused == id) {
      if(ws->pane_count > 0) {
        ws->focused = ws->panes[ws->pane_count - 1];
      } else {
        ws->has_focused = false;
      }
    }
  }
  /* Clear scratchpad refs */
  for(int i = 0; i < t->scratch_count; i++) {
    if(t->scratchpads[i].has_pane && t->scratchpads[i].pane_id == id) {
      t->scratchpads[i].has_pane = false;
      t->scratchpads[i].visible  = false;
    }
  }
  twm_reflow(t);
}

void twm_set_title(TwmState *t, PaneId id, const char *title) {
  Pane *p = twm_pane_by_id(t, id);
  if(p) strncpy(p->title, title, sizeof(p->title) - 1);
}

void twm_set_focused(TwmState *t, PaneId id) {
  Workspace *ws = &t->workspaces[t->active_ws];
  for(int i = 0; i < ws->pane_count; i++) {
    if(ws->panes[i] == id) {
      ws->focused     = id;
      ws->has_focused = true;
      return;
    }
  }
}

/* ── Float ────────────────────────────────────────────────────────────────── */

void twm_toggle_float(TwmState *t) {
  PaneId id = twm_focused_id(t);
  if(!id) return;
  Pane *p = twm_pane_by_id(t, id);
  if(!p) return;
  p->floating = !p->floating;
  if(p->floating) {
    if(rect_empty(p->float_rect)) p->float_rect = p->rect;
    p->rect = p->float_rect;
  }
  twm_reflow(t);
}

void twm_float_move(TwmState *t, PaneId id, int dx, int dy) {
  Pane *p = twm_pane_by_id(t, id);
  if(!p || !p->floating) return;
  p->rect.x = (p->rect.x + dx < 0) ? 0 : p->rect.x + dx;
  p->rect.y = (p->rect.y + dy < 0) ? 0 : p->rect.y + dy;
  if(p->rect.x + p->rect.w > t->screen_w) p->rect.x = t->screen_w - p->rect.w;
  if(p->rect.y + p->rect.h > t->screen_h) p->rect.y = t->screen_h - p->rect.h;
  p->float_rect = p->rect;
}

void twm_float_resize(TwmState *t, PaneId id, int dw, int dh) {
  Pane *p = twm_pane_by_id(t, id);
  if(!p || !p->floating) return;
  p->rect.w = p->rect.w + dw < 80 ? 80 : p->rect.w + dw;
  p->rect.h = p->rect.h + dh < 60 ? 60 : p->rect.h + dh;
  if(p->rect.w > t->screen_w) p->rect.w = t->screen_w;
  if(p->rect.h > t->screen_h) p->rect.h = t->screen_h;
  p->float_rect = p->rect;
}

/* ── Focus direction ──────────────────────────────────────────────────────── */

void twm_focus_dir(TwmState *t, int dx, int dy) {
  Workspace *ws = &t->workspaces[t->active_ws];
  if(!ws->has_focused) return;
  Pane *cur = twm_pane_by_id(t, ws->focused);
  if(!cur) return;

  int cx = cur->rect.x + cur->rect.w / 2;
  int cy = cur->rect.y + cur->rect.h / 2;

  PaneId best    = 0;
  int    best_d2 = INT32_MAX;

  for(int i = 0; i < ws->pane_count; i++) {
    PaneId pid = ws->panes[i];
    if(pid == ws->focused) continue;
    Pane *p = twm_pane_by_id(t, pid);
    if(!p) continue;
    int nx  = p->rect.x + p->rect.w / 2;
    int ny  = p->rect.y + p->rect.h / 2;
    int dot = (nx - cx) * dx + (ny - cy) * dy;
    if(dot <= 0) continue;
    int d2 = (nx - cx) * (nx - cx) + (ny - cy) * (ny - cy);
    if(d2 < best_d2) {
      best_d2 = d2;
      best    = pid;
    }
  }
  if(best) {
    ws->focused     = best;
    ws->has_focused = true;
  }
}

void twm_swap(TwmState *t, bool forward) {
  Workspace *ws = &t->workspaces[t->active_ws];
  if(ws->pane_count < 2 || !ws->has_focused) return;

  int cur = -1;
  for(int i = 0; i < ws->pane_count; i++)
    if(ws->panes[i] == ws->focused) {
      cur = i;
      break;
    }
  if(cur < 0) return;

  int    n       = ws->pane_count;
  int    tgt     = forward ? (cur + 1) % n : (cur + n - 1) % n;
  PaneId tmp     = ws->panes[cur];
  ws->panes[cur] = ws->panes[tgt];
  ws->panes[tgt] = tmp;
}

/* ── Workspace switching ──────────────────────────────────────────────────── */

void twm_switch_ws(TwmState *t, int n) {
  /* hide all visible scratchpads on departure */
  for(int i = 0; i < t->scratch_count; i++) {
    Scratchpad *sp = &t->scratchpads[i];
    if(!sp->visible || !sp->has_pane) continue;
    Workspace *ws = &t->workspaces[t->active_ws];
    for(int j = 0; j < ws->pane_count; j++) {
      if(ws->panes[j] == sp->pane_id) {
        ws->panes[j] = ws->panes[--ws->pane_count];
        break;
      }
    }
    sp->visible = false;
  }
  n            = n < 0 ? 0 : (n >= t->ws_count ? t->ws_count - 1 : n);
  t->active_ws = n;
  twm_reflow(t);
}

void twm_move_to_ws(TwmState *t, int n) {
  PaneId id = twm_focused_id(t);
  if(!id) return;
  int target = n < 0 ? 0 : (n >= t->ws_count ? t->ws_count - 1 : n);
  if(target == t->active_ws) return;

  Workspace *src = &t->workspaces[t->active_ws];
  for(int i = 0; i < src->pane_count; i++) {
    if(src->panes[i] == id) {
      src->panes[i] = src->panes[--src->pane_count];
      break;
    }
  }
  if(src->has_focused && src->focused == id) {
    if(src->pane_count > 0)
      src->focused = src->panes[src->pane_count - 1];
    else
      src->has_focused = false;
  }

  Workspace *dst                = &t->workspaces[target];
  dst->panes[dst->pane_count++] = id;
  dst->focused                  = id;
  dst->has_focused              = true;
  twm_reflow(t);
}

/* ── Scratchpads ──────────────────────────────────────────────────────────── */

void twm_register_scratch(
    TwmState *t, const char *name, const char *app_id, float wpct, float hpct) {
  for(int i = 0; i < t->scratch_count; i++)
    if(strcmp(t->scratchpads[i].name, name) == 0) return;
  if(t->scratch_count >= MAX_SCRATCHPADS) return;
  Scratchpad *sp = &t->scratchpads[t->scratch_count++];
  memset(sp, 0, sizeof(*sp));
  strncpy(sp->name, name, sizeof(sp->name) - 1);
  strncpy(sp->app_id, app_id, sizeof(sp->app_id) - 1);
  sp->width_pct  = wpct;
  sp->height_pct = hpct;
}

static Rect scratch_rect(TwmState *t, Scratchpad *sp) {
  int w = (int)(t->screen_w * sp->width_pct);
  int h = (int)(t->screen_h * sp->height_pct);
  int x = (t->screen_w - w) / 2;
  int y = (t->screen_h - h) / 2;
  return (Rect){ x, y, w, h };
}

bool twm_try_assign_scratch(TwmState *t, PaneId id, const char *app_id) {
  /* never match on empty/null app_id — client hasn't sent it yet */
  if(!app_id || !app_id[0]) return false;

  for(int i = 0; i < t->scratch_count; i++) {
    Scratchpad *sp = &t->scratchpads[i];
    if(sp->has_pane) continue;
    if(strcmp(sp->app_id, app_id) != 0) continue;

    sp->pane_id  = id;
    sp->has_pane = true;
    Rect  r      = scratch_rect(t, sp);
    Pane *p      = twm_pane_by_id(t, id);
    if(p) {
      p->floating   = true;
      p->rect       = r;
      p->float_rect = r;
    }
    /* remove from any workspace — scratchpads live off-screen until shown */
    for(int wi = 0; wi < t->ws_count; wi++) {
      Workspace *ws = &t->workspaces[wi];
      for(int j = 0; j < ws->pane_count; j++) {
        if(ws->panes[j] == id) {
          ws->panes[j] = ws->panes[--ws->pane_count];
          if(ws->has_focused && ws->focused == id) {
            ws->has_focused = ws->pane_count > 0;
            if(ws->has_focused) ws->focused = ws->panes[ws->pane_count - 1];
          }
          break;
        }
      }
    }
    return true;
  }
  return false;
}

void twm_toggle_scratch(TwmState *t, const char *name) {
  Scratchpad *sp = NULL;
  for(int i = 0; i < t->scratch_count; i++) {
    if(strcmp(t->scratchpads[i].name, name) == 0) {
      sp = &t->scratchpads[i];
      break;
    }
  }
  if(!sp || !sp->has_pane) return;

  Workspace *ws = &t->workspaces[t->active_ws];
  if(sp->visible) {
    /* hide: remove from active workspace */
    for(int i = 0; i < ws->pane_count; i++) {
      if(ws->panes[i] == sp->pane_id) {
        ws->panes[i] = ws->panes[--ws->pane_count];
        break;
      }
    }
    if(ws->has_focused && ws->focused == sp->pane_id) {
      ws->has_focused = ws->pane_count > 0;
      if(ws->has_focused) ws->focused = ws->panes[ws->pane_count - 1];
    }
    sp->visible = false;
  } else {
    /* show: update position, add to active workspace */
    Rect  r = scratch_rect(t, sp);
    Pane *p = twm_pane_by_id(t, sp->pane_id);
    if(p) {
      p->rect       = r;
      p->float_rect = r;
    }
    ws->panes[ws->pane_count++] = sp->pane_id;
    ws->focused                 = sp->pane_id;
    ws->has_focused             = true;
    sp->visible                 = true;
  }
  twm_reflow(t);
}
