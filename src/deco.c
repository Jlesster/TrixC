/* deco.c — Window border/title decorations using wlr_scene colored rects.
 *
 * For each visible pane we maintain up to 4 border rect scene nodes
 * (top, bottom, left, right) + optionally a title text buffer node.
 * Rects are repositioned each frame to match animated pane positions.
 */
#include "trixie.h"
#include <string.h>
#include <stdlib.h>

#define MAX_DECO_PANES 256

typedef struct {
	PaneId              id;
	bool                active;

	/* 4 border rects: 0=top 1=bottom 2=left 3=right */
	struct wlr_scene_rect *borders[4];
	bool                  has_rects;
} DecoEntry;

struct TrixieDeco {
	struct wlr_scene *scene;
	DecoEntry         entries[MAX_DECO_PANES];
	int               count;
};

TrixieDeco *deco_create(struct wlr_scene *scene) {
	TrixieDeco *d = calloc(1, sizeof(*d));
	d->scene = scene;
	return d;
}

void deco_destroy(TrixieDeco *d) {
	if (!d) return;
	for (int i = 0; i < d->count; i++) {
		DecoEntry *e = &d->entries[i];
		for (int k = 0; k < 4; k++)
			if (e->borders[k]) wlr_scene_node_destroy(&e->borders[k]->node);
	}
	free(d);
}

static DecoEntry *deco_find(TrixieDeco *d, PaneId id) {
	for (int i = 0; i < d->count; i++)
		if (d->entries[i].id == id) return &d->entries[i];
	return NULL;
}

static DecoEntry *deco_get_or_create(TrixieDeco *d, PaneId id) {
	DecoEntry *e = deco_find(d, id);
	if (e) return e;
	if (d->count >= MAX_DECO_PANES) return NULL;
	e = &d->entries[d->count++];
	memset(e, 0, sizeof(*e));
	e->id = id;
	return e;
}

static float *color_f(Color c, float out[4]) {
	out[0] = c.r / 255.0f;
	out[1] = c.g / 255.0f;
	out[2] = c.b / 255.0f;
	out[3] = c.a / 255.0f;
	return out;
}

static void ensure_borders(TrixieDeco *d, DecoEntry *e) {
	if (e->has_rects) return;
	for (int k = 0; k < 4; k++) {
		float col[4] = {0,0,0,1};
		e->borders[k] = wlr_scene_rect_create(&d->scene->tree, 1, 1, col);
		wlr_scene_node_set_enabled(&e->borders[k]->node, false);
	}
	e->has_rects = true;
}

static void position_borders(DecoEntry *e, Rect r, int bw, bool focused,
                              Color active, Color inactive) {
	Color col = focused ? active : inactive;
	float fc[4]; color_f(col, fc);

	/* top */
	wlr_scene_rect_set_size(e->borders[0], r.w, bw);
	wlr_scene_node_set_position(&e->borders[0]->node, r.x, r.y);
	wlr_scene_rect_set_color(e->borders[0], fc);
	wlr_scene_node_set_enabled(&e->borders[0]->node, true);

	/* bottom */
	wlr_scene_rect_set_size(e->borders[1], r.w, bw);
	wlr_scene_node_set_position(&e->borders[1]->node, r.x, r.y + r.h - bw);
	wlr_scene_rect_set_color(e->borders[1], fc);
	wlr_scene_node_set_enabled(&e->borders[1]->node, true);

	/* left */
	wlr_scene_rect_set_size(e->borders[2], bw, r.h);
	wlr_scene_node_set_position(&e->borders[2]->node, r.x, r.y);
	wlr_scene_rect_set_color(e->borders[2], fc);
	wlr_scene_node_set_enabled(&e->borders[2]->node, true);

	/* right */
	wlr_scene_rect_set_size(e->borders[3], bw, r.h);
	wlr_scene_node_set_position(&e->borders[3]->node, r.x + r.w - bw, r.y);
	wlr_scene_rect_set_color(e->borders[3], fc);
	wlr_scene_node_set_enabled(&e->borders[3]->node, true);
}

static void hide_borders(DecoEntry *e) {
	if (!e->has_rects) return;
	for (int k = 0; k < 4; k++)
		wlr_scene_node_set_enabled(&e->borders[k]->node, false);
}

void deco_update(TrixieDeco *d, TwmState *twm, AnimSet *anim, const Config *cfg) {
	if (!d || !twm) return;

	int bw = cfg->border_width;
	Workspace *ws = &twm->workspaces[twm->active_ws];

	/* mark all entries as not-seen */
	for (int i = 0; i < d->count; i++) d->entries[i].active = false;

	PaneId focused_id = twm_focused_id(twm);

	for (int i = 0; i < ws->pane_count; i++) {
		PaneId pid = ws->panes[i];
		Pane  *p   = twm_pane_by_id(twm, pid);
		if (!p) continue;
		if (p->floating || p->fullscreen || bw <= 0) {
			/* no border for floating / fullscreen */
			DecoEntry *e = deco_find(d, pid);
			if (e) { hide_borders(e); e->active = true; }
			continue;
		}

		Rect r = anim_get_rect(anim, pid, p->rect);
		bool focused = (pid == focused_id);

		DecoEntry *e = deco_get_or_create(d, pid);
		if (!e) continue;
		e->active = true;
		ensure_borders(d, e);
		position_borders(e, r, bw, focused,
		                 cfg->colors.active_border,
		                 cfg->colors.inactive_border);
	}

	/* hide borders for panes no longer on this workspace */
	for (int i = 0; i < d->count; i++) {
		if (!d->entries[i].active)
			hide_borders(&d->entries[i]);
	}
}
