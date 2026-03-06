/* layout.c — tiling layout algorithms */
#include "trixie.h"

const char *layout_label(Layout l) {
	switch (l) {
	case LAYOUT_BSP:      return "BSP";
	case LAYOUT_COLUMNS:  return "Columns";
	case LAYOUT_ROWS:     return "Rows";
	case LAYOUT_THREECOL: return "ThreeCol";
	case LAYOUT_MONOCLE:  return "Monocle";
	default:              return "?";
	}
}

Layout layout_next(Layout l) { return (l + 1) % LAYOUT_COUNT; }
Layout layout_prev(Layout l) { return (l + LAYOUT_COUNT - 1) % LAYOUT_COUNT; }

/* ── Helpers ──────────────────────────────────────────────────────────────── */

static void bsp_inner(Rect area, int n, int gap, bool split_vert, Rect *out, int *idx) {
	if (n == 0) return;
	if (n == 1) { out[(*idx)++] = area; return; }

	if (split_vert) {
		int half_w = (area.w - gap) / 2;
		int rest_w = area.w - half_w - gap;
		Rect left  = {area.x, area.y, half_w, area.h};
		Rect right = {area.x + half_w + gap, area.y, rest_w, area.h};
		out[(*idx)++] = left;
		bsp_inner(right, n - 1, gap, false, out, idx);
	} else {
		int half_h = (area.h - gap) / 2;
		int rest_h = area.h - half_h - gap;
		Rect top = {area.x, area.y, area.w, half_h};
		Rect bot = {area.x, area.y + half_h + gap, area.w, rest_h};
		out[(*idx)++] = top;
		bsp_inner(bot, n - 1, gap, true, out, idx);
	}
}

static void layout_bsp(Rect area, int n, int gap, Rect *out) {
	int idx = 0;
	bsp_inner(area, n, gap, area.w >= area.h, out, &idx);
}

static void layout_columns(Rect area, int n, float ratio, int gap, Rect *out) {
	if (n == 1) { out[0] = area; return; }
	int main_w = (int)(area.w * ratio);
	if (main_w < 4) main_w = 4;
	int side_w = area.w - main_w - gap;
	int rest = n - 1;
	int gaps_total = gap * (rest - 1);
	int each_h = (area.h - gaps_total) / rest;

	out[0] = (Rect){area.x, area.y, main_w, area.h};
	for (int i = 0; i < rest; i++) {
		int y = area.y + i * (each_h + gap);
		int h = (i + 1 == rest) ? (area.y + area.h - y) : each_h;
		out[i + 1] = (Rect){area.x + main_w + gap, y, side_w, h};
	}
}

static void layout_rows(Rect area, int n, float ratio, int gap, Rect *out) {
	if (n == 1) { out[0] = area; return; }
	int main_h = (int)(area.h * ratio);
	if (main_h < 3) main_h = 3;
	int side_h = area.h - main_h - gap;
	int rest = n - 1;
	int gaps_total = gap * (rest - 1);
	int each_w = (area.w - gaps_total) / rest;

	out[0] = (Rect){area.x, area.y, area.w, main_h};
	for (int i = 0; i < rest; i++) {
		int x = area.x + i * (each_w + gap);
		int w = (i + 1 == rest) ? (area.x + area.w - x) : each_w;
		out[i + 1] = (Rect){x, area.y + main_h + gap, w, side_h};
	}
}

static void stack_col(Rect area, int sx, int side_w, int count, int gap, Rect *out) {
	if (count == 0) return;
	int gaps_total = gap * (count - 1);
	int each_h = (area.h - gaps_total) / count;
	for (int i = 0; i < count; i++) {
		int y = area.y + i * (each_h + gap);
		int h = (i + 1 == count) ? (area.y + area.h - y) : each_h;
		out[i] = (Rect){sx, y, side_w, h};
	}
}

static void layout_threecol(Rect area, int n, float ratio, int gap, Rect *out) {
	if (n == 1) { out[0] = area; return; }
	if (n == 2) { layout_columns(area, n, ratio, gap, out); return; }

	int main_w = (int)(area.w * ratio);
	if (main_w < 4) main_w = 4;
	int side_w = (area.w - main_w - gap * 2) / 2;
	int cx = area.x + side_w + gap;

	out[0] = (Rect){cx, area.y, main_w, area.h}; /* centre main */

	int left_count = (n - 1) / 2;
	int right_count = (n - 1) - left_count;

	stack_col(area, area.x,           side_w, left_count,  gap, out + 1);
	stack_col(area, cx + main_w + gap, side_w, right_count, gap, out + 1 + left_count);
}

/* ── Public entry ─────────────────────────────────────────────────────────── */

void layout_compute(Layout l, Rect area, int n, float ratio, int gap, Rect *out) {
	if (n == 0) return;
	switch (l) {
	case LAYOUT_BSP:
		layout_bsp(area, n, gap, out);
		break;
	case LAYOUT_COLUMNS:
		layout_columns(area, n, ratio, gap, out);
		break;
	case LAYOUT_ROWS:
		layout_rows(area, n, ratio, gap, out);
		break;
	case LAYOUT_THREECOL:
		layout_threecol(area, n, ratio, gap, out);
		break;
	case LAYOUT_MONOCLE:
		for (int i = 0; i < n; i++) out[i] = area;
		break;
	default:
		layout_bsp(area, n, gap, out);
		break;
	}
}
