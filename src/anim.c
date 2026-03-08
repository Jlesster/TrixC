/* anim.c — Slide / scale / morph / fade animations for panes and workspace
 * transitions.
 *
 * Mirrors the Rust AnimRect struct and the anim_* free functions from anim.c /
 * the Easing enum in twm_config.rs.  Every animation is a timed lerp between
 * a `from` rect and a `target` rect, sampled at render time via anim_get_rect.
 *
 * Animation kinds
 * ───────────────
 *   ANIM_OPEN / ANIM_CLOSE        — slide in/out from the nearest screen edge
 *   ANIM_FLOAT_OPEN / _CLOSE      — scale from/to center (back/cubic easing)
 *   ANIM_SCRATCH_OPEN / _CLOSE    — slide in/out from the top edge
 *   ANIM_MORPH                    — general position/size tween (layout change)
 *   ANIM_FADE_IN / ANIM_FADE_OUT  — opacity only, rect stays at target
 *
 * Frame convention
 * ────────────────
 *   For every kind, `from[4]` is where the animation STARTS and `target` is
 *   where it ENDS.  This is true for both open and close animations:
 *
 *     OPEN:  from = off-screen position,   target = on-screen rect
 *     CLOSE: from = on-screen rect,        target = off-screen position
 *
 *   BUG FIX: the original ANIM_CLOSE branch did lerp_rect(tgt, from, t) which
 *   reversed the direction.  The convention is now uniform: lerp from→target.
 *
 * Workspace slide
 * ───────────────
 *   anim_workspace_transition sets anim.ws.active and records the direction.
 *   anim_ws_incoming_x / anim_ws_outgoing_x return pixel offsets the renderer
 *   should apply to the incoming / outgoing workspace trees each frame.
 */
#include "trixie.h"
#include <math.h>
#include <time.h>

/* ── Time ───────────────────────────────────────────────────────────────────── */

static int64_t ms_elapsed(struct timespec *start) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (now.tv_sec - start->tv_sec) * 1000 +
         (now.tv_nsec - start->tv_nsec) / 1000000;
}

static float anim_progress(PaneAnim *a) {
  float t = (float)ms_elapsed(&a->start) / (float)a->duration_ms;
  if(t < 0.0f) t = 0.0f;
  if(t > 1.0f) t = 1.0f;
  return t;
}

/* ── Easing ─────────────────────────────────────────────────────────────────── */

static float ease_out_quint(float t) {
  float v = 1.0f - t;
  return 1.0f - v * v * v * v * v;
}

static float ease_in_cubic(float t) {
  return t * t * t;
}

/* Overshoots slightly then settles — used for float-open. */
static float ease_out_back(float t) {
  const float c1 = 1.70158f, c3 = c1 + 1.0f;
  float       v = t - 1.0f;
  return 1.0f + c3 * v * v * v + c1 * v * v;
}

/* ── Linear interpolation helpers ───────────────────────────────────────────── */

static int lerp_i(int a, int b, float t) {
  return (int)roundf((float)a + ((float)b - (float)a) * t);
}

static Rect lerp_rect(int from[4], int to[4], float t) {
  return (Rect){
    .x = lerp_i(from[0], to[0], t),
    .y = lerp_i(from[1], to[1], t),
    .w = lerp_i(from[2], to[2], t),
    .h = lerp_i(from[3], to[3], t),
  };
}

/* ── Edge helpers ───────────────────────────────────────────────────────────── */

typedef enum { EDGE_TOP, EDGE_BOTTOM, EDGE_LEFT, EDGE_RIGHT } Edge;

/* Pick the screen edge that a pane's centre is nearest to. */
static Edge nearest_edge(Rect r, int sw, int sh) {
  float cx = (float)(r.x + r.w / 2) / (float)(sw ? sw : 1) - 0.5f;
  float cy = (float)(r.y + r.h / 2) / (float)(sh ? sh : 1) - 0.5f;
  if(cx == 0.0f && cy == 0.0f) return EDGE_BOTTOM;
  if(fabsf(cx) >= fabsf(cy)) return cx >= 0 ? EDGE_RIGHT : EDGE_LEFT;
  return cy >= 0 ? EDGE_BOTTOM : EDGE_TOP;
}

/* Return the rect displaced completely off-screen toward `e`. */
static void off_screen(Rect r, Edge e, int sw, int sh, int out[4]) {
  out[0] = r.x;
  out[1] = r.y;
  out[2] = r.w;
  out[3] = r.h;
  switch(e) {
    case EDGE_TOP: out[1] = -r.h; break;
    case EDGE_BOTTOM: out[1] = sh; break;
    case EDGE_LEFT: out[0] = -r.w; break;
    case EDGE_RIGHT: out[0] = sw; break;
  }
}

/* Scratchpads always slide from the top. */
static void scratch_off(Rect r, int out[4]) {
  out[0] = r.x;
  out[1] = -r.h;
  out[2] = r.w;
  out[3] = r.h;
}

/* Scale a rect uniformly from its own centre — for float open/close. */
static Rect scale_from_center(Rect r, float s) {
  float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
  float nw = r.w * s, nh = r.h * s;
  return (Rect){
    .x = (int)(cx - nw * 0.5f),
    .y = (int)(cy - nh * 0.5f),
    .w = (int)nw,
    .h = (int)nh,
  };
}

/* ── Entry lookup ───────────────────────────────────────────────────────────── */

static AnimEntry *find_entry(AnimSet *a, uint32_t id) {
  for(int i = 0; i < a->count; i++)
    if(a->entries[i].id == id) return &a->entries[i];
  return NULL;
}

static AnimEntry *get_or_create(AnimSet *a, uint32_t id) {
  AnimEntry *e = find_entry(a, id);
  if(e) return e;
  if(a->count >= MAX_PANES) return &a->entries[0];
  e     = &a->entries[a->count++];
  e->id = id;
  return e;
}

static void start_anim(AnimSet *a,
                       uint32_t id,
                       AnimKind kind,
                       int      from[4],
                       Rect     target,
                       int      duration_ms) {
  AnimEntry *e        = get_or_create(a, id);
  e->anim.kind        = kind;
  e->anim.target      = target;
  e->anim.from[0]     = from[0];
  e->anim.from[1]     = from[1];
  e->anim.from[2]     = from[2];
  e->anim.from[3]     = from[3];
  e->anim.duration_ms = duration_ms;
  e->anim.active      = true;
  clock_gettime(CLOCK_MONOTONIC, &e->anim.start);
}

/* ── Public API ─────────────────────────────────────────────────────────────── */

void anim_set_resize(AnimSet *a, int w, int h) {
  a->screen_w = w;
  a->screen_h = h;
}

/* OPEN: animate from off-screen → on-screen (from=off, target=on). */
void anim_open(AnimSet *a, uint32_t id, Rect r) {
  Edge e = nearest_edge(r, a->screen_w, a->screen_h);
  int  off[4];
  off_screen(r, e, a->screen_w, a->screen_h, off);
  start_anim(a, id, ANIM_OPEN, off, r, 200);
}

/* CLOSE: animate from on-screen → off-screen (from=on, target=off). */
void anim_close(AnimSet *a, uint32_t id, Rect r) {
  Edge e = nearest_edge(r, a->screen_w, a->screen_h);
  int  off[4];
  off_screen(r, e, a->screen_w, a->screen_h, off);
  int  on[4]    = { r.x, r.y, r.w, r.h };
  Rect off_rect = { off[0], off[1], off[2], off[3] };
  start_anim(a, id, ANIM_CLOSE, on, off_rect, 150);
}

/* ── Fade helpers ───────────────────────────────────────────────────────────── */

void anim_fade_in(AnimSet *a, uint32_t id, int duration_ms) {
  AnimEntry *e = get_or_create(a, id);
  if(!e) return;
  int from[4] = {
    e->anim.target.x, e->anim.target.y, e->anim.target.w, e->anim.target.h
  };
  e->anim.kind         = ANIM_FADE_IN;
  e->anim.opacity_from = 0.0f;
  e->anim.opacity_to   = 1.0f;
  e->anim.from[0]      = from[0];
  e->anim.from[1]      = from[1];
  e->anim.from[2]      = from[2];
  e->anim.from[3]      = from[3];
  e->anim.duration_ms  = duration_ms > 0 ? duration_ms : 150;
  e->anim.active       = true;
  clock_gettime(CLOCK_MONOTONIC, &e->anim.start);
}

void anim_fade_out(AnimSet *a, uint32_t id, int duration_ms) {
  AnimEntry *e = get_or_create(a, id);
  if(!e) return;
  int from[4] = {
    e->anim.target.x, e->anim.target.y, e->anim.target.w, e->anim.target.h
  };
  e->anim.kind         = ANIM_FADE_OUT;
  e->anim.opacity_from = 1.0f;
  e->anim.opacity_to   = 0.0f;
  e->anim.from[0]      = from[0];
  e->anim.from[1]      = from[1];
  e->anim.from[2]      = from[2];
  e->anim.from[3]      = from[3];
  e->anim.duration_ms  = duration_ms > 0 ? duration_ms : 150;
  e->anim.active       = true;
  clock_gettime(CLOCK_MONOTONIC, &e->anim.start);
}

float anim_get_opacity(AnimSet *a, uint32_t id, float fallback) {
  AnimEntry *e = find_entry(a, id);
  if(!e || !e->anim.active) return fallback;
  PaneAnim *pa = &e->anim;
  float     t  = anim_progress(pa);
  switch(pa->kind) {
    case ANIM_FADE_IN:
      return pa->opacity_from +
             (pa->opacity_to - pa->opacity_from) * ease_out_quint(t);
    case ANIM_FADE_OUT:
      return pa->opacity_from +
             (pa->opacity_to - pa->opacity_from) * ease_in_cubic(t);
    case ANIM_FLOAT_OPEN:
      return pa->opacity_from +
             (pa->opacity_to - pa->opacity_from) * ease_out_quint(t);
    case ANIM_FLOAT_CLOSE:
      return pa->opacity_from +
             (pa->opacity_to - pa->opacity_from) * ease_in_cubic(t);
    default: return fallback;
  }
}

void anim_workspace_transition(AnimSet *a, WsDir dir) {
  a->ws.active      = true;
  a->ws.dir         = dir;
  a->ws.duration_ms = 240;
  a->ws.screen_w    = a->screen_w;
  clock_gettime(CLOCK_MONOTONIC, &a->ws.start);
}

void anim_float_open(AnimSet *a, uint32_t id, Rect r) {
  int from[4] = { r.x, r.y, r.w, r.h };
  start_anim(a, id, ANIM_FLOAT_OPEN, from, r, 220);
  AnimEntry *e = find_entry(a, id);
  if(e) {
    e->anim.opacity_from = 0.0f;
    e->anim.opacity_to   = 1.0f;
  }
}

void anim_float_close(AnimSet *a, uint32_t id, Rect r) {
  int from[4] = { r.x, r.y, r.w, r.h };
  start_anim(a, id, ANIM_FLOAT_CLOSE, from, r, 140);
  AnimEntry *e = find_entry(a, id);
  if(e) {
    e->anim.opacity_from = 1.0f;
    e->anim.opacity_to   = 0.0f;
  }
}

/* SCRATCH OPEN: from above the top edge → on-screen. */
void anim_scratch_open(AnimSet *a, uint32_t id, Rect r) {
  int off[4];
  scratch_off(r, off);
  start_anim(a, id, ANIM_SCRATCH_OPEN, off, r, 220);
}

/* SCRATCH CLOSE: from on-screen → above the top edge. */
void anim_scratch_close(AnimSet *a, uint32_t id, Rect r) {
  int off[4];
  scratch_off(r, off);
  int  on[4]    = { r.x, r.y, r.w, r.h };
  Rect off_rect = { off[0], off[1], off[2], off[3] };
  start_anim(a, id, ANIM_SCRATCH_CLOSE, on, off_rect, 160);
}

/* Only fires if no open/close is active, and only when destination differs. */
void anim_morph(AnimSet *a, uint32_t id, Rect from, Rect to) {
  AnimEntry *e = find_entry(a, id);
  if(e && e->anim.active) {
    AnimKind k = e->anim.kind;
    if(k == ANIM_OPEN || k == ANIM_CLOSE || k == ANIM_FLOAT_OPEN ||
       k == ANIM_FLOAT_CLOSE || k == ANIM_SCRATCH_OPEN || k == ANIM_SCRATCH_CLOSE)
      return;
    if(e->anim.target.x == to.x && e->anim.target.y == to.y &&
       e->anim.target.w == to.w && e->anim.target.h == to.h)
      return;
  }
  if(from.x == to.x && from.y == to.y && from.w == to.w && from.h == to.h) return;
  int f[4] = { from.x, from.y, from.w, from.h };
  start_anim(a, id, ANIM_MORPH, f, to, 200);
}

/* ── Tick — two-pass: mark finished, then compact ───────────────────────────── */

bool anim_tick(AnimSet *a) {
  for(int i = 0; i < a->count; i++) {
    PaneAnim *pa = &a->entries[i].anim;
    if(pa->active && anim_progress(pa) >= 1.0f) pa->active = false;
  }
  int live = 0;
  for(int i = 0; i < a->count; i++)
    if(a->entries[i].anim.active) a->entries[live++] = a->entries[i];
  a->count = live;

  if(a->ws.active) {
    float t = (float)ms_elapsed(&a->ws.start) / (float)a->ws.duration_ms;
    if(t >= 1.0f) a->ws.active = false;
  }
  return a->count > 0 || a->ws.active;
}

/* ── Sample ─────────────────────────────────────────────────────────────────── */

Rect anim_get_rect(AnimSet *a, uint32_t id, Rect fallback) {
  AnimEntry *e = find_entry(a, id);
  if(!e || !e->anim.active) return fallback;
  PaneAnim *pa     = &e->anim;
  float     t      = anim_progress(pa);
  int       tgt[4] = { pa->target.x, pa->target.y, pa->target.w, pa->target.h };

  switch(pa->kind) {
    case ANIM_OPEN:
    case ANIM_SCRATCH_OPEN:
    case ANIM_MORPH: return lerp_rect(pa->from, tgt, ease_out_quint(t));

    case ANIM_CLOSE:
    case ANIM_SCRATCH_CLOSE: return lerp_rect(pa->from, tgt, ease_in_cubic(t));

    case ANIM_FLOAT_OPEN: {
      float s = 0.85f + 0.15f * ease_out_back(t);
      return scale_from_center(pa->target, s);
    }
    case ANIM_FLOAT_CLOSE: {
      float s = 1.0f - 0.18f * ease_in_cubic(t);
      return scale_from_center(pa->target, s);
    }

    case ANIM_FADE_IN:
    case ANIM_FADE_OUT: return pa->target;
  }
  return fallback;
}

bool anim_is_closing(AnimSet *a, uint32_t id) {
  AnimEntry *e = find_entry(a, id);
  if(!e || !e->anim.active) return false;
  AnimKind k = e->anim.kind;
  return k == ANIM_CLOSE || k == ANIM_FLOAT_CLOSE || k == ANIM_SCRATCH_CLOSE;
}

bool anim_any(AnimSet *a) {
  return a->count > 0 || a->ws.active;
}

/* ── Workspace slide offsets ────────────────────────────────────────────────── */

int anim_ws_incoming_x(AnimSet *a) {
  if(!a->ws.active) return 0;
  float t = (float)ms_elapsed(&a->ws.start) / (float)a->ws.duration_ms;
  if(t > 1.0f) t = 1.0f;
  float e  = ease_out_quint(t);
  float sw = (float)a->ws.screen_w;
  return a->ws.dir == WS_DIR_RIGHT ? (int)(sw * (1.0f - e))
                                   : (int)(-sw * (1.0f - e));
}

int anim_ws_outgoing_x(AnimSet *a) {
  if(!a->ws.active) return 0;
  float t = (float)ms_elapsed(&a->ws.start) / (float)a->ws.duration_ms;
  if(t > 1.0f) t = 1.0f;
  float e  = ease_out_quint(t);
  /* Parallax: outgoing workspace travels at 0.7× the incoming speed for depth. */
  float sw = (float)a->ws.screen_w * 0.7f;
  return a->ws.dir == WS_DIR_RIGHT ? (int)(-sw * e) : (int)(sw * e);
}
