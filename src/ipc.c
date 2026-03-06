/* ipc.c — Unix socket IPC command dispatcher */
#include "trixie.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void ipc_dispatch(TrixieServer *s, const char *line, char *reply, size_t reply_sz) {
	char buf[1024]; strncpy(buf, line, sizeof(buf) - 1);
	char *cmd  = strtok(buf, " \t");
	char *rest = strtok(NULL, "");
	if (rest) while (*rest == ' ' || *rest == '\t') rest++;
	if (!cmd) { snprintf(reply, reply_sz, "err: empty command"); return; }

#define REPLY(...) snprintf(reply, reply_sz, __VA_ARGS__)

	if (!strcmp(cmd, "reload")) {
		server_apply_config_reload(s);
		REPLY("ok: config reloaded");

	} else if (!strcmp(cmd, "quit")) {
		s->running = false;
		wl_display_terminate(s->display);
		REPLY("ok: shutting down");

	} else if (!strcmp(cmd, "switch_vt")) {
		int n = rest ? atoi(rest) : 0;
		if (n >= 1 && n <= 12 && wlr_backend_is_session(s->backend)) {
			struct wlr_session *session = wlr_backend_get_session(s->backend);
			wlr_session_change_vt(session, n);
			REPLY("ok: switch vt %d", n);
		} else { REPLY("err: usage: switch_vt <1-12>"); }

	} else if (!strcmp(cmd, "workspace")) {
		int n = rest ? atoi(rest) : 0;
		if (n >= 1) {
			int old = s->twm.active_ws;
			twm_switch_ws(&s->twm, n - 1);
			int nw = s->twm.active_ws;
			if (nw != old)
				anim_workspace_transition(&s->anim, nw > old ? WS_DIR_RIGHT : WS_DIR_LEFT);
			server_sync_focus(s);
			server_sync_windows(s);
			REPLY("ok: workspace %d", n);
		} else { REPLY("err: usage: workspace <n>"); }

	} else if (!strcmp(cmd, "next_workspace")) {
		int old = s->twm.active_ws;
		twm_switch_ws(&s->twm, (old + 1) % s->twm.ws_count);
		if (s->twm.active_ws != old) anim_workspace_transition(&s->anim, WS_DIR_RIGHT);
		server_sync_focus(s); server_sync_windows(s);
		REPLY("ok");

	} else if (!strcmp(cmd, "prev_workspace")) {
		int old = s->twm.active_ws;
		twm_switch_ws(&s->twm, (old + s->twm.ws_count - 1) % s->twm.ws_count);
		if (s->twm.active_ws != old) anim_workspace_transition(&s->anim, WS_DIR_LEFT);
		server_sync_focus(s); server_sync_windows(s);
		REPLY("ok");

	} else if (!strcmp(cmd, "scratchpad")) {
		if (!rest || !rest[0]) { REPLY("err: usage: scratchpad <name>"); return; }
		server_scratch_toggle(s, rest);
		REPLY("ok: toggled '%s'", rest);

	} else if (!strcmp(cmd, "float")) {
		server_float_toggle(s);
		REPLY("ok: float toggled");

	} else if (!strcmp(cmd, "float_move")) {
		int dx = 0, dy = 0;
		if (rest) sscanf(rest, "%d %d", &dx, &dy);
		PaneId id = twm_focused_id(&s->twm);
		if (id) {
			twm_float_move(&s->twm, id, dx, dy);
			server_sync_windows(s);
			server_request_redraw(s);
			REPLY("ok: moved (%d,%d)", dx, dy);
		} else { REPLY("err: no focused window"); }

	} else if (!strcmp(cmd, "float_resize")) {
		int dw = 0, dh = 0;
		if (rest) sscanf(rest, "%d %d", &dw, &dh);
		PaneId id = twm_focused_id(&s->twm);
		if (id) {
			twm_float_resize(&s->twm, id, dw, dh);
			server_sync_windows(s);
			server_request_redraw(s);
			REPLY("ok: resized (%d,%d)", dw, dh);
		} else { REPLY("err: no focused window"); }

	} else if (!strcmp(cmd, "close")) {
		PaneId id = twm_focused_id(&s->twm);
		if (id) {
			TrixieView *v = view_from_pane(s, id);
			if (v) wlr_xdg_toplevel_send_close(v->xdg_toplevel);
		}
		REPLY("ok: close sent");

	} else if (!strcmp(cmd, "fullscreen")) {
		Action a = {.kind = ACTION_FULLSCREEN};
		server_dispatch_action(s, &a);
		REPLY("ok: fullscreen toggled");

	} else if (!strcmp(cmd, "focus")) {
		Action a = {0};
		if (!strcmp(rest, "left"))       a.kind = ACTION_FOCUS_LEFT;
		else if (!strcmp(rest, "right")) a.kind = ACTION_FOCUS_RIGHT;
		else if (!strcmp(rest, "up"))    a.kind = ACTION_FOCUS_UP;
		else if (!strcmp(rest, "down"))  a.kind = ACTION_FOCUS_DOWN;
		else { REPLY("err: usage: focus <left|right|up|down>"); return; }
		server_dispatch_action(s, &a);
		REPLY("ok");

	} else if (!strcmp(cmd, "move_to_workspace")) {
		int n = rest ? atoi(rest) : 0;
		if (n >= 1) {
			twm_move_to_ws(&s->twm, n - 1);
			server_sync_windows(s);
			REPLY("ok: moved to workspace %d", n);
		} else { REPLY("err: usage: move_to_workspace <n>"); }

	} else if (!strcmp(cmd, "layout")) {
		if (!rest || !strcmp(rest, "next") || !strcmp(rest, "")) {
			Action a = {.kind = ACTION_NEXT_LAYOUT};
			server_dispatch_action(s, &a);
			REPLY("ok: next layout");
		} else if (!strcmp(rest, "prev")) {
			Action a = {.kind = ACTION_PREV_LAYOUT};
			server_dispatch_action(s, &a);
			REPLY("ok: prev layout");
		} else { REPLY("err: usage: layout <next|prev>"); }

	} else if (!strcmp(cmd, "grow_main")) {
		Action a = {.kind = ACTION_GROW_MAIN};
		server_dispatch_action(s, &a);
		REPLY("ok");

	} else if (!strcmp(cmd, "shrink_main")) {
		Action a = {.kind = ACTION_SHRINK_MAIN};
		server_dispatch_action(s, &a);
		REPLY("ok");

	} else if (!strcmp(cmd, "spawn")) {
		if (!rest || !rest[0]) { REPLY("err: usage: spawn <cmd>"); return; }
		server_spawn(s, rest);
		REPLY("ok: spawned");

	} else if (!strcmp(cmd, "status")) {
		/* brief status */
		int len = 0;
		len += snprintf(reply + len, reply_sz - len,
		    "active_workspace: %d\nscreen: %dx%d\n",
		    s->twm.active_ws + 1, s->twm.screen_w, s->twm.screen_h);
		for (int i = 0; i < s->twm.ws_count; i++) {
			Workspace *ws = &s->twm.workspaces[i];
			len += snprintf(reply + len, reply_sz - len,
			    "workspace %d: panes=%d layout=%s\n",
			    i + 1, ws->pane_count, layout_label(ws->layout));
		}

	} else if (!strcmp(cmd, "status_json")) {
		/* minimal JSON */
		int len = 0;
		len += snprintf(reply + len, reply_sz - len,
		    "{\"active_workspace\":%d,\"screen\":{\"w\":%d,\"h\":%d},\"workspaces\":[",
		    s->twm.active_ws + 1, s->twm.screen_w, s->twm.screen_h);
		for (int i = 0; i < s->twm.ws_count; i++) {
			Workspace *ws = &s->twm.workspaces[i];
			len += snprintf(reply + len, reply_sz - len,
			    "%s{\"index\":%d,\"active\":%s,\"panes\":%d,\"layout\":\"%s\"}",
			    i > 0 ? "," : "", i + 1,
			    i == s->twm.active_ws ? "true" : "false",
			    ws->pane_count, layout_label(ws->layout));
		}
		len += snprintf(reply + len, reply_sz - len, "]}");

	} else {
		REPLY("err: unknown command '%s'", cmd);
	}
#undef REPLY
}
