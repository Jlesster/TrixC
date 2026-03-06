/* config.c — config file parser and hot-reload */
#include "trixie.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

/* ── Defaults ─────────────────────────────────────────────────────────────── */

void config_defaults(Config *c) {
	memset(c, 0, sizeof(*c));

	strncpy(c->font_path, "/usr/share/fonts/TTF/JetBrainsMono-Regular.ttf",
	        sizeof(c->font_path) - 1);
	c->font_size    = 14.0f;
	c->gap          = 4;
	c->border_width = 1;
	c->corner_radius = 0;
	c->smart_gaps   = false;
	strncpy(c->cursor_theme, "default", sizeof(c->cursor_theme) - 1);
	c->cursor_size  = 24;
	c->workspaces   = 9;
	strncpy(c->seat_name, "seat0", sizeof(c->seat_name) - 1);

	c->colors.active_border   = color_hex(0xb4befe);
	c->colors.inactive_border = color_hex(0x45475a);
	c->colors.pane_bg         = color_hex(0x11111b);

	BarCfg *b = &c->bar;
	b->position      = BAR_BOTTOM;
	b->height        = 28;
	b->item_spacing  = 4;
	b->pill_radius   = 4;
	b->bg            = color_hex(0x181825);
	b->fg            = color_hex(0xa6adc8);
	b->accent        = color_hex(0xb4befe);
	b->dim           = color_hex(0x585b70);
	b->active_ws_fg  = color_hex(0x11111b);
	b->active_ws_bg  = color_hex(0xb4befe);
	b->occupied_ws_fg = color_hex(0xb4befe);
	b->inactive_ws_fg = color_hex(0x585b70);
	b->separator     = false;
	b->separator_color = color_hex(0x313244);

	/* default left/center/right */
	strncpy(b->modules_left[0],   "workspaces", 63); b->modules_left_n   = 1;
	strncpy(b->modules_center[0], "clock",      63); b->modules_center_n = 1;
	strncpy(b->modules_right[0],  "layout",     63);
	strncpy(b->modules_right[1],  "battery",    63);
	strncpy(b->modules_right[2],  "network",    63);
	b->modules_right_n = 3;

	c->keyboard.repeat_rate  = 25;
	c->keyboard.repeat_delay = 600;

	/* default keybinds */
	int k = 0;
#define KB(m, sym_str, act)                                      \
	do {                                                         \
		c->keybinds[k].mods   = (m);                            \
		c->keybinds[k].sym    = xkb_keysym_from_name(           \
		    (sym_str), XKB_KEYSYM_CASE_INSENSITIVE);             \
		c->keybinds[k].action = (act);                           \
		k++;                                                     \
	} while (0)

	KB(MOD_SUPER, "Return",
	   ((Action){.kind=ACTION_EXEC, .exec_cmd="foot"}));
	KB(MOD_SUPER, "q",
	   ((Action){.kind=ACTION_CLOSE}));
	KB(MOD_SUPER, "f",
	   ((Action){.kind=ACTION_FULLSCREEN}));
	KB(MOD_SUPER|MOD_SHIFT, "space",
	   ((Action){.kind=ACTION_TOGGLE_FLOAT}));
	KB(MOD_SUPER|MOD_SHIFT, "b",
	   ((Action){.kind=ACTION_TOGGLE_BAR}));
	KB(MOD_SUPER, "h",
	   ((Action){.kind=ACTION_FOCUS_LEFT}));
	KB(MOD_SUPER, "l",
	   ((Action){.kind=ACTION_FOCUS_RIGHT}));
	KB(MOD_SUPER, "k",
	   ((Action){.kind=ACTION_FOCUS_UP}));
	KB(MOD_SUPER, "j",
	   ((Action){.kind=ACTION_FOCUS_DOWN}));
	KB(MOD_SUPER|MOD_SHIFT, "h",
	   ((Action){.kind=ACTION_MOVE_LEFT}));
	KB(MOD_SUPER|MOD_SHIFT, "l",
	   ((Action){.kind=ACTION_MOVE_RIGHT}));
	KB(MOD_SUPER, "Tab",
	   ((Action){.kind=ACTION_NEXT_LAYOUT}));
	KB(MOD_SUPER, "equal",
	   ((Action){.kind=ACTION_GROW_MAIN}));
	KB(MOD_SUPER, "minus",
	   ((Action){.kind=ACTION_SHRINK_MAIN}));
	KB(MOD_SUPER|MOD_CTRL, "Right",
	   ((Action){.kind=ACTION_NEXT_WS}));
	KB(MOD_SUPER|MOD_CTRL, "Left",
	   ((Action){.kind=ACTION_PREV_WS}));
	for (int i = 1; i <= 9; i++) {
		char sym[4]; snprintf(sym, sizeof(sym), "%d", i);
		c->keybinds[k].mods = MOD_SUPER;
		c->keybinds[k].sym  = xkb_keysym_from_name(sym, XKB_KEYSYM_CASE_INSENSITIVE);
		c->keybinds[k].action = (Action){.kind=ACTION_WORKSPACE, .n=i};
		k++;
		c->keybinds[k].mods = MOD_SUPER|MOD_SHIFT;
		c->keybinds[k].sym  = xkb_keysym_from_name(sym, XKB_KEYSYM_CASE_INSENSITIVE);
		c->keybinds[k].action = (Action){.kind=ACTION_MOVE_TO_WS, .n=i};
		k++;
	}
#undef KB
	c->keybind_count = k;
}

/* ── Simple line parser ───────────────────────────────────────────────────── */

static char *trim(char *s) {
	while (isspace((unsigned char)*s)) s++;
	char *end = s + strlen(s) - 1;
	while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
	return s;
}

static Color parse_color(const char *s) {
	while (*s == '#') s++;
	if (strlen(s) == 6) {
		unsigned v = strtoul(s, NULL, 16);
		return color_hex(v);
	}
	if (strlen(s) == 8) {
		unsigned long v = strtoul(s, NULL, 16);
		return (Color){(v>>24)&0xff, (v>>16)&0xff, (v>>8)&0xff, v&0xff};
	}
	return (Color){0,0,0,255};
}

/* Very small state-machine parser for trixie.conf */
typedef struct {
	char block[64];
	char label[64];
	int  depth;
} ParseCtx;

static void handle_kv(Config *c, ParseCtx *ctx, const char *key, const char *val) {
	const char *blk = ctx->block;

	/* top level */
	if (ctx->depth == 0 || blk[0] == '\0') {
		if (!strcmp(key, "font"))         strncpy(c->font_path, val, sizeof(c->font_path)-1);
		else if (!strcmp(key, "font_size"))      c->font_size    = atof(val);
		else if (!strcmp(key, "gap"))            c->gap          = atoi(val);
		else if (!strcmp(key, "border_width"))   c->border_width = atoi(val);
		else if (!strcmp(key, "corner_radius"))  c->corner_radius= atoi(val);
		else if (!strcmp(key, "smart_gaps"))     c->smart_gaps   = !strcmp(val,"true");
		else if (!strcmp(key, "cursor_theme"))   strncpy(c->cursor_theme, val, sizeof(c->cursor_theme)-1);
		else if (!strcmp(key, "cursor_size"))    c->cursor_size  = atoi(val);
		else if (!strcmp(key, "workspaces"))     c->workspaces   = atoi(val);
		else if (!strcmp(key, "seat_name") || !strcmp(key,"seat"))
		                                         strncpy(c->seat_name, val, sizeof(c->seat_name)-1);
		else if (!strcmp(key, "exec_once") && c->exec_once_count < 16) {
			strncpy(c->exec_once[c->exec_once_count++], val, 255);
		}
		else if (!strcmp(key, "exec") && c->exec_count < 16) {
			strncpy(c->exec[c->exec_count++], val, 255);
		}
		else if (!strcmp(key, "keybind")) {
			/* keybind = COMBO, action [args...] */
			/* split on first comma or space-separated tokens */
			char buf[512]; strncpy(buf, val, sizeof(buf)-1);
			char *p = buf;
			/* first token: combo */
			char *combo = strtok(p, ", \t");
			char *action_str = combo ? strtok(NULL, ", \t") : NULL;
			if (!combo || !action_str) return;

			/* parse mods+key */
			char combo_buf[128]; strncpy(combo_buf, combo, sizeof(combo_buf)-1);
			uint32_t mods = 0;
			char *sym_str = combo_buf;
			/* find last + or : */
			char *last_sep = strrchr(combo_buf, '+');
			char *last_col = strrchr(combo_buf, ':');
			char *sep = last_sep > last_col ? last_sep : last_col;
			if (sep) {
				*sep = '\0';
				sym_str = sep + 1;
				char *m = strtok(combo_buf, "+:");
				while (m) {
					if (!strcasecmp(m,"super")||!strcasecmp(m,"mod4")) mods |= MOD_SUPER;
					else if (!strcasecmp(m,"ctrl")||!strcasecmp(m,"control")) mods |= MOD_CTRL;
					else if (!strcasecmp(m,"alt")||!strcasecmp(m,"mod1")) mods |= MOD_ALT;
					else if (!strcasecmp(m,"shift")) mods |= MOD_SHIFT;
					m = strtok(NULL, "+:");
				}
			}

			xkb_keysym_t sym = xkb_keysym_from_name(sym_str, XKB_KEYSYM_CASE_INSENSITIVE);
			if (sym == XKB_KEY_NoSymbol) return;

			Action act = {0};
			char *arg1 = strtok(NULL, ", \t");

			if (!strcmp(action_str,"exec")) {
				act.kind = ACTION_EXEC;
				/* remaining tokens form the command */
				if (arg1) strncpy(act.exec_cmd, arg1, sizeof(act.exec_cmd)-1);
				int ai = 0;
				char *a;
				while ((a = strtok(NULL, ", \t")) && ai < MAX_EXEC_ARGS)
					strncpy(act.exec_args[ai++], a, 255);
				act.exec_argc = ai;
			} else if (!strcmp(action_str,"close"))         act.kind = ACTION_CLOSE;
			else if (!strcmp(action_str,"fullscreen"))      act.kind = ACTION_FULLSCREEN;
			else if (!strcmp(action_str,"toggle_float"))    act.kind = ACTION_TOGGLE_FLOAT;
			else if (!strcmp(action_str,"toggle_bar"))      act.kind = ACTION_TOGGLE_BAR;
			else if (!strcmp(action_str,"focus")) {
				if (arg1) {
					if (!strcmp(arg1,"left"))  act.kind = ACTION_FOCUS_LEFT;
					else if (!strcmp(arg1,"right")) act.kind = ACTION_FOCUS_RIGHT;
					else if (!strcmp(arg1,"up"))    act.kind = ACTION_FOCUS_UP;
					else if (!strcmp(arg1,"down"))  act.kind = ACTION_FOCUS_DOWN;
				}
			}
			else if (!strcmp(action_str,"move")) {
				if (arg1) {
					if (!strcmp(arg1,"left"))  act.kind = ACTION_MOVE_LEFT;
					else if (!strcmp(arg1,"right")) act.kind = ACTION_MOVE_RIGHT;
				}
			}
			else if (!strcmp(action_str,"workspace"))      { act.kind=ACTION_WORKSPACE; act.n=arg1?atoi(arg1):1; }
			else if (!strcmp(action_str,"move_to_workspace")){ act.kind=ACTION_MOVE_TO_WS; act.n=arg1?atoi(arg1):1; }
			else if (!strcmp(action_str,"next_layout"))    act.kind = ACTION_NEXT_LAYOUT;
			else if (!strcmp(action_str,"prev_layout"))    act.kind = ACTION_PREV_LAYOUT;
			else if (!strcmp(action_str,"grow_main"))      act.kind = ACTION_GROW_MAIN;
			else if (!strcmp(action_str,"shrink_main"))    act.kind = ACTION_SHRINK_MAIN;
			else if (!strcmp(action_str,"next_workspace")) act.kind = ACTION_NEXT_WS;
			else if (!strcmp(action_str,"prev_workspace")) act.kind = ACTION_PREV_WS;
			else if (!strcmp(action_str,"quit"))           act.kind = ACTION_QUIT;
			else if (!strcmp(action_str,"reload"))         act.kind = ACTION_RELOAD;
			else if (!strcmp(action_str,"scratchpad")) {
				act.kind = ACTION_SCRATCHPAD;
				if (arg1) strncpy(act.name, arg1, sizeof(act.name)-1);
			}
			else return; /* unknown */

			if (c->keybind_count < MAX_KEYBINDS) {
				c->keybinds[c->keybind_count].mods   = mods;
				c->keybinds[c->keybind_count].sym    = sym;
				c->keybinds[c->keybind_count].action = act;
				c->keybind_count++;
			}
		}
		return;
	}

	/* bar {} */
	if (!strcmp(blk, "bar")) {
		BarCfg *b = &c->bar;
		if (!strcmp(key,"position"))    b->position = !strcmp(val,"top") ? BAR_TOP : BAR_BOTTOM;
		else if (!strcmp(key,"height")) b->height   = atoi(val);
		else if (!strcmp(key,"item_spacing")) b->item_spacing = atoi(val);
		else if (!strcmp(key,"pill_radius"))  b->pill_radius  = atoi(val);
		else if (!strcmp(key,"font_size"))    b->font_size    = atof(val);
		else if (!strcmp(key,"bg"))      b->bg      = parse_color(val);
		else if (!strcmp(key,"fg"))      b->fg      = parse_color(val);
		else if (!strcmp(key,"accent"))  b->accent  = parse_color(val);
		else if (!strcmp(key,"dim"))     b->dim     = parse_color(val);
		else if (!strcmp(key,"active_ws_fg"))   b->active_ws_fg   = parse_color(val);
		else if (!strcmp(key,"active_ws_bg"))   b->active_ws_bg   = parse_color(val);
		else if (!strcmp(key,"occupied_ws_fg")) b->occupied_ws_fg = parse_color(val);
		else if (!strcmp(key,"inactive_ws_fg")) b->inactive_ws_fg = parse_color(val);
		else if (!strcmp(key,"separator"))       b->separator = !strcmp(val,"true");
		else if (!strcmp(key,"separator_color")) b->separator_color = parse_color(val);
		else if (!strcmp(key,"modules_left")) {
			b->modules_left_n = 0;
			char buf[512]; strncpy(buf,val,sizeof(buf)-1);
			char *tok = strtok(buf," ,[]");
			while (tok && b->modules_left_n < MAX_BAR_MODS)
				strncpy(b->modules_left[b->modules_left_n++],tok,63), tok=strtok(NULL," ,[]");
		}
		else if (!strcmp(key,"modules_center")) {
			b->modules_center_n = 0;
			char buf[512]; strncpy(buf,val,sizeof(buf)-1);
			char *tok = strtok(buf," ,[]");
			while (tok && b->modules_center_n < MAX_BAR_MODS)
				strncpy(b->modules_center[b->modules_center_n++],tok,63), tok=strtok(NULL," ,[]");
		}
		else if (!strcmp(key,"modules_right")) {
			b->modules_right_n = 0;
			char buf[512]; strncpy(buf,val,sizeof(buf)-1);
			char *tok = strtok(buf," ,[]");
			while (tok && b->modules_right_n < MAX_BAR_MODS)
				strncpy(b->modules_right[b->modules_right_n++],tok,63), tok=strtok(NULL," ,[]");
		}
		return;
	}

	/* colors {} */
	if (!strcmp(blk, "colors")) {
		if (!strcmp(key,"active_border"))   c->colors.active_border   = parse_color(val);
		else if (!strcmp(key,"inactive_border")) c->colors.inactive_border = parse_color(val);
		else if (!strcmp(key,"pane_bg"))    c->colors.pane_bg   = parse_color(val);
		else if (!strcmp(key,"bar_bg"))     c->bar.bg           = parse_color(val);
		else if (!strcmp(key,"bar_fg"))     c->bar.fg           = parse_color(val);
		else if (!strcmp(key,"bar_accent")) c->bar.accent       = parse_color(val);
		return;
	}

	/* keyboard {} */
	if (!strcmp(blk, "keyboard")) {
		if (!strcmp(key,"layout"))       strncpy(c->keyboard.kb_layout,  val, sizeof(c->keyboard.kb_layout)-1);
		else if (!strcmp(key,"variant")) strncpy(c->keyboard.kb_variant, val, sizeof(c->keyboard.kb_variant)-1);
		else if (!strcmp(key,"options")) strncpy(c->keyboard.kb_options, val, sizeof(c->keyboard.kb_options)-1);
		else if (!strcmp(key,"repeat_rate"))  c->keyboard.repeat_rate  = atoi(val);
		else if (!strcmp(key,"repeat_delay")) c->keyboard.repeat_delay = atoi(val);
		return;
	}

	/* monitor <name> {} */
	if (!strcmp(blk, "monitor") && c->monitor_count < MAX_MONITORS) {
		MonitorCfg *m = &c->monitors[c->monitor_count - 1];
		if (!strcmp(key,"width"))   m->width   = atoi(val);
		else if (!strcmp(key,"height"))  m->height  = atoi(val);
		else if (!strcmp(key,"refresh")) m->refresh = atoi(val);
		else if (!strcmp(key,"scale"))   m->scale   = atof(val);
		else if (!strcmp(key,"position")) {
			/* accept "x y" or "x,y" */
			char buf[64]; strncpy(buf,val,sizeof(buf)-1);
			char *tok = strtok(buf," ,");
			if (tok) { m->pos_x = atoi(tok); tok = strtok(NULL," ,"); }
			if (tok)   m->pos_y = atoi(tok);
		}
		return;
	}

	/* scratchpad <name> {} */
	if (!strcmp(blk, "scratchpad") && c->scratchpad_count > 0) {
		ScratchpadCfg *sp = &c->scratchpads[c->scratchpad_count - 1];
		if (!strcmp(key,"app_id")) strncpy(sp->app_id, val, sizeof(sp->app_id)-1);
		else if (!strcmp(key,"width")) {
			/* accept percent like 70% or float 0.7 */
			float v = atof(val);
			if (strchr(val,'%')) v /= 100.0f;
			sp->width_pct = v > 1.0f ? 1.0f : v < 0.1f ? 0.1f : v;
		}
		else if (!strcmp(key,"height")) {
			float v = atof(val);
			if (strchr(val,'%')) v /= 100.0f;
			sp->height_pct = v > 1.0f ? 1.0f : v < 0.1f ? 0.1f : v;
		}
		return;
	}
}

void config_load(Config *c, const char *path) {
	config_defaults(c);

	FILE *f = fopen(path, "r");
	if (!f) {
		wlr_log(WLR_INFO, "config: no file at %s, using defaults", path);
		return;
	}

	ParseCtx ctx = {0};
	char line[1024];

	/* track current block stack */
	char block_stack[8][64] = {0};
	char label_stack[8][64] = {0};
	int  depth = 0;

	while (fgets(line, sizeof(line), f)) {
		char *s = trim(line);
		if (!s[0] || s[0] == '#') continue;

		/* strip inline comment */
		char *hash = strchr(s, '#');
		if (hash) *hash = '\0';
		s = trim(s);

		if (s[0] == '{') { depth++; continue; }

		if (s[0] == '}') {
			if (depth > 0) depth--;
			if (depth < (int)(sizeof(block_stack)/sizeof(block_stack[0]))) {
				block_stack[depth][0] = '\0';
				label_stack[depth][0] = '\0';
			}
			continue;
		}

		/* block open: "name [label] {" or "name [label]" followed by { on next line */
		/* detect if line ends with { */
		char *brace = strchr(s, '{');
		if (brace) {
			*brace = '\0';
			char tmp[256]; strncpy(tmp, s, sizeof(tmp)-1);
			char *tok = strtok(tmp, " \t");
			if (tok) {
				strncpy(block_stack[depth], tok, 63);
				tok = strtok(NULL, " \t");
				if (tok) strncpy(label_stack[depth], tok, 63);
				else label_stack[depth][0] = '\0';
			}

			/* handle monitor / scratchpad block starts */
			if (!strcmp(block_stack[depth], "monitor") && c->monitor_count < MAX_MONITORS) {
				MonitorCfg *m = &c->monitors[c->monitor_count++];
				memset(m, 0, sizeof(*m));
				m->refresh = 60; m->scale = 1.0f;
				strncpy(m->name, label_stack[depth], sizeof(m->name)-1);
			}
			if (!strcmp(block_stack[depth], "scratchpad") && c->scratchpad_count < MAX_SCRATCHPADS) {
				ScratchpadCfg *sp = &c->scratchpads[c->scratchpad_count++];
				memset(sp, 0, sizeof(*sp));
				sp->width_pct = 0.6f; sp->height_pct = 0.6f;
				strncpy(sp->name, label_stack[depth], sizeof(sp->name)-1);
				/* default app_id = name */
				strncpy(sp->app_id, label_stack[depth], sizeof(sp->app_id)-1);
			}

			depth++;
			continue;
		}

		/* key = value assignment */
		char *eq = strchr(s, '=');
		if (!eq) {
			/* might be bare block opener on this line */
			/* skip unknown bare tokens */
			continue;
		}
		*eq = '\0';
		char *key = trim(s);
		char *val = trim(eq + 1);
		/* strip surrounding quotes */
		if ((val[0]=='"' || val[0]=='\'') && strlen(val)>1) {
			val++;
			char *end = val + strlen(val) - 1;
			if (*end=='"' || *end=='\'') *end = '\0';
		}

		ctx.depth = depth;
		if (depth > 0 && depth <= 8)
			strncpy(ctx.block, block_stack[depth-1], 63);
		else
			ctx.block[0] = '\0';
		strncpy(ctx.label, label_stack[depth > 0 ? depth-1 : 0], 63);

		handle_kv(c, &ctx, key, val);
	}

	fclose(f);
	wlr_log(WLR_INFO, "config: loaded %s (%d keybinds, %d monitors, %d scratchpads)",
	        path, c->keybind_count, c->monitor_count, c->scratchpad_count);
}

static char config_path_buf[512];

static const char *get_config_path(void) {
	if (config_path_buf[0]) return config_path_buf;
	const char *xdg = getenv("XDG_CONFIG_HOME");
	if (xdg) snprintf(config_path_buf, sizeof(config_path_buf), "%s/trixie/trixie.conf", xdg);
	else {
		const char *home = getenv("HOME");
		if (!home) home = "/root";
		snprintf(config_path_buf, sizeof(config_path_buf), "%s/.config/trixie/trixie.conf", home);
	}
	return config_path_buf;
}

void config_reload(Config *c) {
	/* preserve exec_once_done — caller handles it */
	int exec_count_save = c->exec_count;
	char exec_save[16][256];
	memcpy(exec_save, c->exec, sizeof(exec_save));

	config_load(c, get_config_path());

	/* exec is allowed to re-run on reload, exec_once is not */
	(void)exec_count_save;
}
