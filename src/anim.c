/* anim.c — Slide / scale / morph / fade animations for panes and workspace
 * transitions.
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
 *   reversed the direction.  The convention is now uniform: lerp from→target.
 *
 * Workspace slide
 * ───────────────
 *   anim_workspace_transition sets anim.ws.active and records the direction.
 *   anim_ws_incoming_x / anim_ws_outgoing_x return pixel offsets the renderer
 *   should apply to the incoming / outgoing workspace trees each frame.
 *
 * Performance notes
 * ─────────────────
 *   • clock_gettime() is snapshotted once per frame via anim_tick_begin() so
 *     anim_get_rect / anim_get_opacity / ws offset queries all share it.
 *   • Entry lookup uses a sorted id array + binary search (O(log n) vs O(n)).
 *   • anim_tick() compacts in a single pass (no mark-then-sweep).
 *   • lerp_i() uses integer arithmetic instead of roundf().
 *   • Workspace eased-progress is cached once per frame in the WsAnim so
 *     incoming/outgoing offset calls don't recompute it.
 */
#include "trixie.h"
#include <math.h>
#include <time.h>

/* ── Per-frame time snapshot ────────────────────────────────────────────────── */
/* Call anim_tick_begin() once at the start of each frame (inside anim_tick).
 * All progress queries then use g_frame_ns instead of a fresh clock_gettime(). */

static int64_t g_frame_ns = 0; /* nanoseconds, set each frame */

static inline void snapshot_time(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  g_frame_ns = (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
}

static inline float progress_from_start(const struct timespec *start,
                                        int                    duration_ms) {
  int64_t start_ns = (int64_t)start->tv_sec * 1000000000LL + start->tv_nsec;
  float   t        = (float)((g_frame_ns - start_ns) / 1000000) / (float)duration_ms;
  if(t < 0.0f) t = 0.0f;
  if(t > 1.0f) t = 1.0f;
  return t;
}

/* ── Easing ─────────────────────────────────────────────────────────────────── */

static inline float ease_out_quint(float t) {
  float v = 1.0f - t;
  return 1.0f - v * v * v * v * v;
}

static inline float ease_in_cubic(float t) {
  return t * t * t;
}

/* Overshoots slightly then settles — used for float-open. */
static inline float ease_out_back(float t) {
  const float c1 = 1.70158f, c3 = c1 + 1.0f;
  float       v = t - 1.0f;
  return 1.0f + c3 * v * v * v + c1 * v * v;
}

/* ── Linear interpolation helpers ───────────────────────────────────────────── */

/* Integer lerp without roundf() — just truncate with 0.5 bias. */
static inline int lerp_i(int a, int b, float t) {
  return a + (int)((float)(b - a) * t + 0.5f);
}

static inline Rect lerp_rect(const int from[4], const int to[4], float t) {
  return (Rect){
    .x = lerp_i(from[0], to[0], t),
    .y = lerp_i(from[1], to[1], t),
    .w = lerp_i(from[2], to[2], t),
    .h = lerp_i(from[3], to[3], t),
  };
}

/* ── Edge helpers ───────────────────────────────────────────────────────────── */

typedef enum { EDGE_TOP, EDGE_BOTTOM, EDGE_LEFT, EDGE_RIGHT } Edge;

static Edge nearest_edge(Rect r, int sw, int sh) {
  float cx = (float)(r.x + r.w / 2) / (float)(sw ? sw : 1) - 0.5f;
  float cy = (float)(r.y + r.h / 2) / (float)(sh ? sh : 1) - 0.5f;
  if(cx == 0.0f && cy == 0.0f) return EDGE_BOTTOM;
  if(fabsf(cx) >= fabsf(cy)) return cx >= 0 ? EDGE_RIGHT : EDGE_LEFT;
  return cy >= 0 ? EDGE_BOTTOM : EDGE_TOP;
}

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

static void scratch_off(Rect r, int out[4]) {
  out[0] = r.x;
  out[1] = -r.h;
  out[2] = r.w;
  out[3] = r.h;
}

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

/* ── Entry lookup — sorted id array + binary search ────────────────────────── */
/*
 * We maintain a parallel uint32_t id_index[] sorted by id.  Binary search
 * gives O(log n) find vs O(n) linear scan.  The index is kept sorted on
 * every insert; since n is small (≤ MAX_PANES, typically < 20) insertion
 * sort is faster than qsort for the common case.
 */

static AnimEntry *find_entry(AnimSet *a, uint32_t id) {
  /* Binary search on id_index for the id. */
  int lo = 0, hi = a->count - 1;
  while(lo <= hi) {
    int mid = (lo + hi) >> 1;
    if(a->id_index[mid] == id) return &a->entries[a->idx_map[mid]];
    if(a->id_index[mid] < id)
      lo = mid + 1;
    else
      hi = mid - 1;
  }
  return NULL;
}

/* Insert id→slot into the sorted index. */
static void index_insert(AnimSet *a, uint32_t id, int slot) {
  int n = a->count - 1; /* entry already counted */
  int i = n - 1;
  /* shift right while id_index[i] > id */
  while(i >= 0 && a->id_index[i] > id) {
    a->id_index[i + 1] = a->id_index[i];
    a->idx_map[i + 1]  = a->idx_map[i];
    i--;
  }
  a->id_index[i + 1] = id;
  a->idx_map[i + 1]  = slot;
}

/* Rebuild the sorted index from scratch — called after compaction. */
static void index_rebuild(AnimSet *a) {
  /* Simple insertion sort — n is tiny. */
  for(int i = 0; i < a->count; i++) {
    a->id_index[i] = a->entries[i].id;
    a->idx_map[i]  = i;
  }
  /* insertion sort by id_index */
  for(int i = 1; i < a->count; i++) {
    uint32_t ki = a->id_index[i];
    int      vi = a->idx_map[i];
    int      j  = i - 1;
    while(j >= 0 && a->id_index[j] > ki) {
      a->id_index[j + 1] = a->id_index[j];
      a->idx_map[j + 1]  = a->idx_map[j];
      j--;
    }
    a->id_index[j + 1] = ki;
    a->idx_map[j + 1]  = vi;
  }
}

static AnimEntry *get_or_create(AnimSet *a, uint32_t id) {
  AnimEntry *e = find_entry(a, id);
  if(e) return e;
  if(a->count >= MAX_PANES) return &a->entries[0];
  int slot = a->count;
  e        = &a->entries[slot];
  e->id    = id;
  a->count++;
  index_insert(a, id, slot);
  return e;
}

static void start_anim(AnimSet  *a,
                       uint32_t  id,
                       AnimKind  kind,
                       const int from[4],
                       Rect      target,
                       int       duration_ms) {
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

void anim_open(AnimSet *a, uint32_t id, Rect r) {
  Edge e = nearest_edge(r, a->screen_w, a->screen_h);
  int  off[4];
  off_screen(r, e, a->screen_w, a->screen_h, off);
  start_anim(a, id, ANIM_OPEN, off, r, 200);
}

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
  PaneAnim *pa    = &e->anim;
  float     t     = progress_from_start(&pa->start, pa->duration_ms);
  float     range = pa->opacity_to - pa->opacity_from;
  switch(pa->kind) {
    case ANIM_FADE_IN:
    case ANIM_FLOAT_OPEN: return pa->opacity_from + range * ease_out_quint(t);
    case ANIM_FADE_OUT:
    case ANIM_FLOAT_CLOSE: return pa->opacity_from + range * ease_in_cubic(t);
    default: return fallback;
  }
}

void anim_workspace_transition(AnimSet *a, WsDir dir) {
  a->ws.active      = true;
  a->ws.dir         = dir;
  a->ws.duration_ms = 240;
  a->ws.screen_w    = a->screen_w;
  a->ws.cached_e    = -1.0f; /* invalidate cache */
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

void anim_scratch_open(AnimSet *a, uint32_t id, Rect r) {
  int off[4];
  scratch_off(r, off);
  start_anim(a, id, ANIM_SCRATCH_OPEN, off, r, 220);
}

void anim_scratch_close(AnimSet *a, uint32_t id, Rect r) {
  int off[4];
  scratch_off(r, off);
  int  on[4]    = { r.x, r.y, r.w, r.h };
  Rect off_rect = { off[0], off[1], off[2], off[3] };
  start_anim(a, id, ANIM_SCRATCH_CLOSE, on, off_rect, 160);
}

void anim_morph(AnimSet *a, uint32_t id, Rect from, Rect to) {
  AnimEntry *e = find_entry(a, id);
  if(e && e->anim.active) {
    AnimKind k = e->anim.kind;
    /* Never interrupt open/close animations. */
    if(k == ANIM_OPEN || k == ANIM_CLOSE || k == ANIM_FLOAT_OPEN ||
       k == ANIM_FLOAT_CLOSE || k == ANIM_SCRATCH_OPEN || k == ANIM_SCRATCH_CLOSE)
      return;
    /* Already morphing toward the same target — nothing to do. */
    if(e->anim.target.x == to.x && e->anim.target.y == to.y &&
       e->anim.target.w == to.w && e->anim.target.h == to.h)
      return;
    /* Chain: use the current in-flight interpolated position as the new start
     * so rapid grow/shrink keypresses feel continuous, not snappy. */
    from = anim_get_rect(a, id, from);
  }
  if(from.x == to.x && from.y == to.y && from.w == to.w && from.h == to.h) return;
  int f[4] = { from.x, from.y, from.w, from.h };
  start_anim(a, id, ANIM_MORPH, f, to, 200);
}

/* ── Tick — snapshot time, single-pass compact, rebuild index ───────────────── */

/* Cancel any active animation for a pane, returning the current interpolated
 * rect (so callers can snap geometry to the in-flight position).
 * Needed at drag-start: without this, anim_get_rect keeps overriding p->rect
 * and the window won't follow the cursor.  After anim_cancel the fallback path
 * in anim_get_rect returns p->rect directly on every subsequent call. */
void anim_cancel(AnimSet *a, uint32_t id) {
  AnimEntry *e = find_entry(a, id);
  if(e) e->anim.active = false;
}

bool anim_tick(AnimSet *a) {
  /* Snapshot clock once for this frame — all progress queries use g_frame_ns. */
  snapshot_time();

  /* Single-pass in-place compaction: keep only active entries. */
  int live = 0;
  for(int i = 0; i < a->count; i++) {
    PaneAnim *pa = &a->entries[i].anim;
    if(pa->active) {
      if(progress_from_start(&pa->start, pa->duration_ms) >= 1.0f)
        pa->active = false;
    }
    if(pa->active) {
      if(live != i) a->entries[live] = a->entries[i];
      live++;
    }
  }
  bool compacted = (live != a->count);
  a->count       = live;

  /* Rebuild sorted index only when entries were removed. */
  if(compacted) index_rebuild(a);

  /* Workspace transition — cache the eased progress for this frame. */
  if(a->ws.active) {
    float t = (float)((g_frame_ns - ((int64_t)a->ws.start.tv_sec * 1000000000LL +
                                     a->ws.start.tv_nsec)) /
                      1000000) /
              (float)a->ws.duration_ms;
    if(t > 1.0f) t = 1.0f;
    if(t < 0.0f) t = 0.0f;
    a->ws.cached_e = ease_out_quint(t);
    if(t >= 1.0f) {
      a->ws.active   = false;
      a->ws.cached_e = 1.0f;
    }
  }

  return a->count > 0 || a->ws.active;
}

/* ── Sample ─────────────────────────────────────────────────────────────────── */

Rect anim_get_rect(AnimSet *a, uint32_t id, Rect fallback) {
  AnimEntry *e = find_entry(a, id);
  if(!e || !e->anim.active) return fallback;
  PaneAnim *pa     = &e->anim;
  float     t      = progress_from_start(&pa->start, pa->duration_ms);
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
/*
 * Both functions use ws.cached_e computed once in anim_tick().
 * This avoids a second clock_gettime + ease computation for every frame
 * where both incoming and outgoing offsets are queried.
 */

int anim_ws_incoming_x(AnimSet *a) {
  if(!a->ws.active && a->ws.cached_e < 0.0f) return 0;
  if(!a->ws.active && a->ws.cached_e >= 1.0f) return 0;
  float e  = (a->ws.cached_e >= 0.0f) ? a->ws.cached_e : 0.0f;
  float sw = (float)a->ws.screen_w;
  return a->ws.dir == WS_DIR_RIGHT ? (int)(sw * (1.0f - e))
                                   : (int)(-sw * (1.0f - e));
}

int anim_ws_outgoing_x(AnimSet *a) {
  if(!a->ws.active && a->ws.cached_e < 0.0f) return 0;
  if(!a->ws.active && a->ws.cached_e >= 1.0f) return 0;
  float e  = (a->ws.cached_e >= 0.0f) ? a->ws.cached_e : 0.0f;
  float sw = (float)a->ws.screen_w * 0.7f;
  return a->ws.dir == WS_DIR_RIGHT ? (int)(-sw * e) : (int)(sw * e);
}
