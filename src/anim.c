/* anim.c — slide/scale/morph animations for panes and workspace transitions */
#include "trixie.h"
#include <math.h>
#include <time.h>

/* ── Time ─────────────────────────────────────────────────────────────────── */

static int64_t ms_since(struct timespec *start) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - start->tv_sec) * 1000 +
	       (now.tv_nsec - start->tv_nsec) / 1000000;
}

static float anim_progress(PaneAnim *a) {
	float t = (float)ms_since(&a->start) / (float)a->duration_ms;
	if (t < 0.0f) t = 0.0f;
	if (t > 1.0f) t = 1.0f;
	return t;
}

/* ── Easing ───────────────────────────────────────────────────────────────── */

static float ease_out_quint(float t) {
	float v = 1.0f - t;
	return 1.0f - v*v*v*v*v;
}
static float ease_in_cubic(float t) { return t*t*t; }
static float ease_out_back(float t) {
	const float c1 = 1.70158f, c3 = c1 + 1.0f;
	float v = t - 1.0f;
	return 1.0f + c3*v*v*v + c1*v*v;
}

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

/* ── Edge helpers ─────────────────────────────────────────────────────────── */

typedef enum { EDGE_TOP, EDGE_BOTTOM, EDGE_LEFT, EDGE_RIGHT } Edge;

static Edge nearest_edge(Rect r, int sw, int sh) {
	float cx = (float)(r.x + r.w/2) / (float)(sw ? sw : 1) - 0.5f;
	float cy = (float)(r.y + r.h/2) / (float)(sh ? sh : 1) - 0.5f;
	if (cx == 0.0f && cy == 0.0f) return EDGE_BOTTOM;
	if (fabsf(cx) >= fabsf(cy)) return cx >= 0 ? EDGE_RIGHT : EDGE_LEFT;
	return cy >= 0 ? EDGE_BOTTOM : EDGE_TOP;
}

static void edge_off(Rect r, Edge e, int sw, int sh, int out[4]) {
	out[0] = r.x; out[1] = r.y; out[2] = r.w; out[3] = r.h;
	switch (e) {
	case EDGE_TOP:    out[1] = -r.h; break;
	case EDGE_BOTTOM: out[1] = sh; break;
	case EDGE_LEFT:   out[0] = -r.w; break;
	case EDGE_RIGHT:  out[0] = sw; break;
	}
}

static void scratch_off(Rect r, int out[4]) {
	out[0] = r.x; out[1] = -r.h; out[2] = r.w; out[3] = r.h;
}

static Rect scale_from_center(Rect r, float scale) {
	float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
	float nw = r.w * scale, nh = r.h * scale;
	return (Rect){
		.x = (int)(cx - nw * 0.5f),
		.y = (int)(cy - nh * 0.5f),
		.w = (int)nw,
		.h = (int)nh,
	};
}

/* ── Entry lookup ─────────────────────────────────────────────────────────── */

static AnimEntry *find_entry(AnimSet *a, uint32_t id) {
	for (int i = 0; i < a->count; i++)
		if (a->entries[i].id == id) return &a->entries[i];
	return NULL;
}

static AnimEntry *get_or_create(AnimSet *a, uint32_t id) {
	AnimEntry *e = find_entry(a, id);
	if (e) return e;
	if (a->count >= MAX_PANES) return &a->entries[0]; /* fallback */
	e = &a->entries[a->count++];
	e->id = id;
	return e;
}

static void start_anim(AnimSet *a, uint32_t id, AnimKind kind,
                       Rect target, int from[4], int duration_ms) {
	AnimEntry *e = get_or_create(a, id);
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

/* ── Public API ───────────────────────────────────────────────────────────── */

void anim_set_resize(AnimSet *a, int w, int h) {
	a->screen_w = w; a->screen_h = h;
}

void anim_open(AnimSet *a, uint32_t id, Rect r) {
	Edge e = nearest_edge(r, a->screen_w, a->screen_h);
	int off[4]; edge_off(r, e, a->screen_w, a->screen_h, off);
	start_anim(a, id, ANIM_OPEN, r, off, 200);
}

void anim_close(AnimSet *a, uint32_t id, Rect r) {
	Edge e = nearest_edge(r, a->screen_w, a->screen_h);
	int off[4]; edge_off(r, e, a->screen_w, a->screen_h, off);
	int tgt[4] = {r.x, r.y, r.w, r.h};
	/* For close: target = on-screen, from = off-screen (we swap lerp direction) */
	start_anim(a, id, ANIM_CLOSE, r, off, 150);
}

void anim_float_open(AnimSet *a, uint32_t id, Rect r) {
	int from[4] = {r.x, r.y, r.w, r.h};
	start_anim(a, id, ANIM_FLOAT_OPEN, r, from, 180);
}

void anim_float_close(AnimSet *a, uint32_t id, Rect r) {
	int from[4] = {r.x, r.y, r.w, r.h};
	start_anim(a, id, ANIM_FLOAT_CLOSE, r, from, 120);
}

void anim_scratch_open(AnimSet *a, uint32_t id, Rect r) {
	int off[4]; scratch_off(r, off);
	start_anim(a, id, ANIM_SCRATCH_OPEN, r, off, 220);
}

void anim_scratch_close(AnimSet *a, uint32_t id, Rect r) {
	int off[4]; scratch_off(r, off);
	start_anim(a, id, ANIM_SCRATCH_CLOSE, r, off, 160);
}

void anim_morph(AnimSet *a, uint32_t id, Rect from, Rect to) {
	AnimEntry *e = find_entry(a, id);
	if (e && e->anim.active) {
		AnimKind k = e->anim.kind;
		if (k == ANIM_OPEN || k == ANIM_CLOSE ||
		    k == ANIM_FLOAT_OPEN || k == ANIM_FLOAT_CLOSE ||
		    k == ANIM_SCRATCH_OPEN || k == ANIM_SCRATCH_CLOSE)
			return; /* don't interrupt open/close */
	}
	int f[4] = {from.x, from.y, from.w, from.h};
	start_anim(a, id, ANIM_MORPH, to, f, 200);
}

void anim_workspace_transition(AnimSet *a, WsDir dir) {
	a->ws.active      = true;
	a->ws.dir         = dir;
	a->ws.duration_ms = 240;
	a->ws.screen_w    = a->screen_w;
	clock_gettime(CLOCK_MONOTONIC, &a->ws.start);
}

bool anim_tick(AnimSet *a) {
	/* expire finished pane anims */
	int live = 0;
	for (int i = 0; i < a->count; i++) {
		PaneAnim *pa = &a->entries[i].anim;
		if (!pa->active) continue;
		float t = anim_progress(pa);
		if (t >= 1.0f) pa->active = false;
		else a->entries[live++] = a->entries[i];
	}
	/* compact */
	for (int i = 0; i < a->count; i++) {
		if (!a->entries[i].anim.active && i < live) {
			/* already handled above */
		}
	}
	a->count = live;

	if (a->ws.active) {
		float t = (float)ms_since(&a->ws.start) / (float)a->ws.duration_ms;
		if (t >= 1.0f) a->ws.active = false;
	}

	return a->count > 0 || a->ws.active;
}

Rect anim_get_rect(AnimSet *a, uint32_t id, Rect fallback) {
	AnimEntry *e = find_entry(a, id);
	if (!e || !e->anim.active) return fallback;
	PaneAnim *pa = &e->anim;
	float t = anim_progress(pa);

	int tgt[4] = {pa->target.x, pa->target.y, pa->target.w, pa->target.h};

	switch (pa->kind) {
	case ANIM_OPEN:
	case ANIM_SCRATCH_OPEN:
		return lerp_rect(pa->from, tgt, ease_out_quint(t));

	case ANIM_CLOSE:
	case ANIM_SCRATCH_CLOSE:
		return lerp_rect(tgt, pa->from, ease_in_cubic(t));

	case ANIM_MORPH:
		return lerp_rect(pa->from, tgt, ease_out_quint(t));

	case ANIM_FLOAT_OPEN: {
		float scale = 0.85f + 0.15f * ease_out_back(t);
		return scale_from_center(pa->target, scale);
	}
	case ANIM_FLOAT_CLOSE: {
		float scale = 1.0f - 0.18f * ease_in_cubic(t);
		return scale_from_center(pa->target, scale);
	}
	}
	return fallback;
}

bool anim_is_closing(AnimSet *a, uint32_t id) {
	AnimEntry *e = find_entry(a, id);
	if (!e || !e->anim.active) return false;
	AnimKind k = e->anim.kind;
	return k == ANIM_CLOSE || k == ANIM_FLOAT_CLOSE || k == ANIM_SCRATCH_CLOSE;
}

bool anim_any(AnimSet *a) {
	return a->count > 0 || a->ws.active;
}

int anim_ws_incoming_x(AnimSet *a) {
	if (!a->ws.active) return 0;
	float t = (float)ms_since(&a->ws.start) / (float)a->ws.duration_ms;
	if (t > 1.0f) t = 1.0f;
	float e = ease_out_quint(t);
	float sw = (float)a->ws.screen_w;
	return a->ws.dir == WS_DIR_RIGHT
		? (int)(sw * (1.0f - e))
		: (int)(-sw * (1.0f - e));
}

int anim_ws_outgoing_x(AnimSet *a) {
	if (!a->ws.active) return 0;
	float t = (float)ms_since(&a->ws.start) / (float)a->ws.duration_ms;
	if (t > 1.0f) t = 1.0f;
	float e = ease_out_quint(t);
	float sw = (float)a->ws.screen_w;
	return a->ws.dir == WS_DIR_RIGHT
		? (int)(-sw * e)
		: (int)(sw * e);
}
