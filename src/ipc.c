/* ipc.c — Unix socket IPC command dispatcher.
 *
 * Commands
 * ────────
 *   workspace <n>              switch to workspace n
 *   next_workspace             switch to next workspace
 *   prev_workspace             switch to previous workspace
 *   move_to_workspace <n>      move focused pane to workspace n
 *   focus <left|right|up|down> directional focus
 *   layout [next|prev]         cycle tiling layout
 *   grow_main / shrink_main    adjust main_ratio ±5 %
 *   ratio <0.1..0.9>           set main_ratio to exact value
 *   swap [forward|back]        cycle pane order in workspace
 *   swap_main                  swap focused pane with master slot
 *   float                      toggle float on focused pane
 *   float_move <dx> <dy>       move floating pane
 *   float_resize <dw> <dh>     resize floating pane
 *   scratchpad <name>          toggle named scratchpad
 *   close                      close focused window
 *   fullscreen                 toggle fullscreen
 *   spawn <cmd>                exec a command through the compositor
 *   reload                     hot-reload config
 *   quit                       terminate compositor
 *   dpms <on|off>              enable or disable outputs (DPMS)
 *   idle_reset                 reset the idle timer and re-enable outputs
 *   status                     human-readable workspace / pane summary
 *   status_json                machine-readable JSON status (with pane detail)
 *   set_layout <name>          set layout by name
 *   query_pane [id]            JSON details for a pane (default: focused)
 *   subscribe                  keep connection open; receive JSON events
 *   overlay [...]              overlay control commands
 *   build [run]                trigger async build
 *
 * Nvim bridge commands (pushed by the trixie.lua nvim plugin)
 * ────────────────────────────────────────────────────────────
 *   nvim_state <file>\t<line>\t<col>\t<ft>
 *   nvim_diag  <json_array>
 *   nvim_buffers <json_array>
 *   quickfix_get               returns JSON quickfix array for nvim setqflist()
 *   nvim_open <file> [<line>]  ask nvim to open a file
 */
#define _POSIX_C_SOURCE 200809L
#include "trixie.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* forward declarations from nvim_panel.c */
bool nvim_is_connected(void);
void nvim_open_file(const char *path, int line);

/* ── Event push helpers ─────────────────────────────────────────────────── */

static void ipc_push_event(TrixieServer *s, const char *buf, size_t len) {
  int live = 0;
  for(int i = 0; i < s->subscriber_count; i++) {
    int     fd = s->subscriber_fds[i];
    ssize_t n  = write(fd, buf, len);
    if(n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
      close(fd);
    } else {
      s->subscriber_fds[live++] = fd;
    }
  }
  s->subscriber_count = live;
}

void ipc_push_focus_changed(TrixieServer *s) {
  if(!s || s->subscriber_count == 0) return;
  char   buf[512];
  PaneId id = twm_focused_id(&s->twm);
  Pane  *p  = id ? twm_pane_by_id(&s->twm, id) : NULL;
  snprintf(buf,
           sizeof(buf),
           "{\"event\":\"focus_changed\",\"pane_id\":%u,"
           "\"title\":\"%s\",\"app_id\":\"%s\","
           "\"workspace\":%d}\n",
           id,
           p ? p->title : "",
           p ? p->app_id : "",
           s->twm.active_ws + 1);
  ipc_push_event(s, buf, strlen(buf));
}

void ipc_push_workspace_changed(TrixieServer *s) {
  if(!s || s->subscriber_count == 0) return;
  char buf[128];
  snprintf(buf,
           sizeof(buf),
           "{\"event\":\"workspace_changed\",\"workspace\":%d,\"pane_count\":%d}\n",
           s->twm.active_ws + 1,
           s->twm.workspaces[s->twm.active_ws].pane_count);
  ipc_push_event(s, buf, strlen(buf));
}

void ipc_push_title_changed(TrixieServer *s, PaneId id) {
  if(!s || s->subscriber_count == 0) return;
  Pane *p = twm_pane_by_id(&s->twm, id);
  if(!p) return;
  char buf[512];
  snprintf(buf,
           sizeof(buf),
           "{\"event\":\"title_changed\",\"pane_id\":%u,"
           "\"title\":\"%s\",\"app_id\":\"%s\"}\n",
           id,
           p->title,
           p->app_id);
  ipc_push_event(s, buf, strlen(buf));
}

void ipc_push_build_done(TrixieServer *s, int exit_code, int err_count) {
  if(!s || s->subscriber_count == 0) return;
  char buf[128];
  snprintf(buf,
           sizeof(buf),
           "{\"event\":\"build_done\",\"exit_code\":%d,\"diagnostics\":%d}\n",
           exit_code,
           err_count);
  ipc_push_event(s, buf, strlen(buf));
}

/* ── Main dispatcher ────────────────────────────────────────────────────── */

void ipc_dispatch(TrixieServer *s, const char *line, char *reply, size_t reply_sz) {
  char buf[1024];
  strncpy(buf, line, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  char *cmd  = strtok(buf, " \t");
  char *rest = strtok(NULL, "");
  if(rest)
    while(*rest == ' ' || *rest == '\t')
      rest++;
  if(!cmd) {
    snprintf(reply, reply_sz, "err: empty command");
    return;
  }

#define REPLY(...) snprintf(reply, reply_sz, __VA_ARGS__)

  /* ── Server lifecycle ───────────────────────────────────────────────── */

  if(!strcmp(cmd, "reload")) {
    server_apply_config_reload(s);
    REPLY("ok: config reloaded");

  } else if(!strcmp(cmd, "quit")) {
    s->running = false;
    wl_display_terminate(s->display);
    REPLY("ok: shutting down");

  } else if(!strcmp(cmd, "switch_vt")) {
    REPLY("err: vt switching not supported in this build");

    /* ── Workspace routing ──────────────────────────────────────────────── */

  } else if(!strcmp(cmd, "workspace")) {
    int n = rest ? atoi(rest) : 0;
    if(n >= 1) {
      int old = s->twm.active_ws;
      twm_switch_ws(&s->twm, n - 1);
      if(s->twm.active_ws != old)
        anim_workspace_transition(
            &s->anim, s->twm.active_ws > old ? WS_DIR_RIGHT : WS_DIR_LEFT);
      server_sync_focus(s);
      server_sync_windows(s);
      ipc_push_workspace_changed(s);
      REPLY("ok: workspace %d", n);
    } else {
      REPLY("err: usage: workspace <n>");
    }

  } else if(!strcmp(cmd, "next_workspace")) {
    int old = s->twm.active_ws;
    twm_switch_ws(&s->twm, (old + 1) % s->twm.ws_count);
    if(s->twm.active_ws != old) anim_workspace_transition(&s->anim, WS_DIR_RIGHT);
    server_sync_focus(s);
    server_sync_windows(s);
    ipc_push_workspace_changed(s);
    REPLY("ok");

  } else if(!strcmp(cmd, "prev_workspace")) {
    int old = s->twm.active_ws;
    twm_switch_ws(&s->twm, (old + s->twm.ws_count - 1) % s->twm.ws_count);
    if(s->twm.active_ws != old) anim_workspace_transition(&s->anim, WS_DIR_LEFT);
    server_sync_focus(s);
    server_sync_windows(s);
    ipc_push_workspace_changed(s);
    REPLY("ok");

  } else if(!strcmp(cmd, "move_to_workspace")) {
    int n = rest ? atoi(rest) : 0;
    if(n >= 1) {
      twm_move_to_ws(&s->twm, n - 1);
      server_sync_windows(s);
      REPLY("ok: moved to workspace %d", n);
    } else {
      REPLY("err: usage: move_to_workspace <n>");
    }

    /* ── Scratchpad ─────────────────────────────────────────────────────── */

  } else if(!strcmp(cmd, "scratchpad")) {
    if(!rest || !rest[0]) {
      REPLY("err: usage: scratchpad <name>");
      return;
    }
    server_scratch_toggle(s, rest);
    REPLY("ok: toggled '%s'", rest);

    /* ── Float ──────────────────────────────────────────────────────────── */

  } else if(!strcmp(cmd, "float")) {
    server_float_toggle(s);
    REPLY("ok: float toggled");

  } else if(!strcmp(cmd, "float_move")) {
    int dx = 0, dy = 0;
    if(rest) sscanf(rest, "%d %d", &dx, &dy);
    PaneId id = twm_focused_id(&s->twm);
    if(id) {
      twm_float_move(&s->twm, id, dx, dy);
      server_sync_windows(s);
      server_request_redraw(s);
      REPLY("ok: moved (%d,%d)", dx, dy);
    } else {
      REPLY("err: no focused window");
    }

  } else if(!strcmp(cmd, "float_resize")) {
    int dw = 0, dh = 0;
    if(rest) sscanf(rest, "%d %d", &dw, &dh);
    PaneId id = twm_focused_id(&s->twm);
    if(id) {
      twm_float_resize(&s->twm, id, dw, dh);
      server_sync_windows(s);
      server_request_redraw(s);
      REPLY("ok: resized (%d,%d)", dw, dh);
    } else {
      REPLY("err: no focused window");
    }

    /* ── Pane actions ───────────────────────────────────────────────────── */

  } else if(!strcmp(cmd, "close")) {
    PaneId id = twm_focused_id(&s->twm);
    if(id) {
      TrixieView *v = view_from_pane(s, id);
      if(v) wlr_xdg_toplevel_send_close(v->xdg_toplevel);
    }
    REPLY("ok: close sent");

  } else if(!strcmp(cmd, "fullscreen")) {
    Action a = { .kind = ACTION_FULLSCREEN };
    server_dispatch_action(s, &a);
    REPLY("ok: fullscreen toggled");

    /* ── Directional focus ──────────────────────────────────────────────── */

  } else if(!strcmp(cmd, "focus")) {
    if(!rest) {
      REPLY("err: usage: focus <left|right|up|down>");
      return;
    }
    Action a = { 0 };
    if(!strcmp(rest, "left"))
      a.kind = ACTION_FOCUS_LEFT;
    else if(!strcmp(rest, "right"))
      a.kind = ACTION_FOCUS_RIGHT;
    else if(!strcmp(rest, "up"))
      a.kind = ACTION_FOCUS_UP;
    else if(!strcmp(rest, "down"))
      a.kind = ACTION_FOCUS_DOWN;
    else {
      REPLY("err: usage: focus <left|right|up|down>");
      return;
    }
    server_dispatch_action(s, &a);
    ipc_push_focus_changed(s);
    REPLY("ok");

    /* ── Swap / reorder ─────────────────────────────────────────────────── */

  } else if(!strcmp(cmd, "swap")) {
    bool forward = true;
    if(rest && !strcmp(rest, "back")) forward = false;
    twm_swap(&s->twm, forward);
    server_sync_windows(s);
    REPLY("ok: swapped %s", forward ? "forward" : "back");

  } else if(!strcmp(cmd, "swap_main")) {
    twm_swap_main(&s->twm);
    server_sync_windows(s);
    REPLY("ok: swapped with main");

    /* ── Layout cycling ─────────────────────────────────────────────────── */

  } else if(!strcmp(cmd, "layout")) {
    if(!rest || !strcmp(rest, "next") || !strcmp(rest, "")) {
      Action a = { .kind = ACTION_NEXT_LAYOUT };
      server_dispatch_action(s, &a);
      REPLY("ok: layout %s",
            layout_label(s->twm.workspaces[s->twm.active_ws].layout));
    } else if(!strcmp(rest, "prev")) {
      Action a = { .kind = ACTION_PREV_LAYOUT };
      server_dispatch_action(s, &a);
      REPLY("ok: layout %s",
            layout_label(s->twm.workspaces[s->twm.active_ws].layout));
    } else {
      REPLY("err: usage: layout <next|prev>");
    }

  } else if(!strcmp(cmd, "grow_main")) {
    Action a = { .kind = ACTION_GROW_MAIN };
    server_dispatch_action(s, &a);
    REPLY("ok: ratio %.2f", s->twm.workspaces[s->twm.active_ws].main_ratio);

  } else if(!strcmp(cmd, "shrink_main")) {
    Action a = { .kind = ACTION_SHRINK_MAIN };
    server_dispatch_action(s, &a);
    REPLY("ok: ratio %.2f", s->twm.workspaces[s->twm.active_ws].main_ratio);

  } else if(!strcmp(cmd, "ratio")) {
    if(!rest) {
      REPLY("err: usage: ratio <0.1..0.9>");
      return;
    }
    float r = (float)atof(rest);
    if(r < 0.1f) r = 0.1f;
    if(r > 0.9f) r = 0.9f;
    Workspace *ws  = &s->twm.workspaces[s->twm.active_ws];
    ws->main_ratio = r;
    twm_reflow(&s->twm);
    server_sync_windows(s);
    REPLY("ok: ratio %.2f", r);

    /* ── Spawn ──────────────────────────────────────────────────────────── */

  } else if(!strcmp(cmd, "spawn")) {
    if(!rest || !rest[0]) {
      REPLY("err: usage: spawn <cmd>");
      return;
    }
    server_spawn(s, rest);
    REPLY("ok: spawned");

    /* ── DPMS ───────────────────────────────────────────────────────────── */

  } else if(!strcmp(cmd, "dpms")) {
    bool          on = !rest || !strcmp(rest, "on");
    TrixieOutput *o;
    wl_list_for_each(o, &s->outputs, link) {
      struct wlr_output_state st;
      wlr_output_state_init(&st);
      wlr_output_state_set_enabled(&st, on);
      wlr_output_commit_state(o->wlr_output, &st);
      wlr_output_state_finish(&st);
    }
    REPLY("ok: dpms %s", on ? "on" : "off");

    /* ── Idle reset ─────────────────────────────────────────────────────── */

  } else if(!strcmp(cmd, "idle_reset")) {
    server_reset_idle(s);
    REPLY("ok: idle timer reset");

    /* ── Introspection ──────────────────────────────────────────────────── */

  } else if(!strcmp(cmd, "status")) {
    int len = 0;
    len += snprintf(reply + len,
                    reply_sz - len,
                    "active_workspace: %d\nscreen: %dx%d\n",
                    s->twm.active_ws + 1,
                    s->twm.screen_w,
                    s->twm.screen_h);
    for(int i = 0; i < s->twm.ws_count; i++) {
      Workspace *ws = &s->twm.workspaces[i];
      len += snprintf(reply + len,
                      reply_sz - len,
                      "workspace %d: panes=%d layout=%s ratio=%.2f\n",
                      i + 1,
                      ws->pane_count,
                      layout_label(ws->layout),
                      ws->main_ratio);
      if(i == s->twm.active_ws) {
        for(int j = 0; j < ws->pane_count && len < (int)reply_sz - 80; j++) {
          Pane *p = twm_pane_by_id(&s->twm, ws->panes[j]);
          if(!p) continue;
          len +=
              snprintf(reply + len,
                       reply_sz - len,
                       "  pane %u: %s%s [%s]\n",
                       p->id,
                       p->title,
                       ws->has_focused && ws->focused == p->id ? " (focused)" : "",
                       p->floating     ? "float"
                       : p->fullscreen ? "fullscreen"
                                       : "tiled");
        }
      }
    }

  } else if(!strcmp(cmd, "status_json")) {
    int len = 0;
    len += snprintf(reply + len,
                    reply_sz - len,
                    "{\"active_workspace\":%d,\"screen\":{\"w\":%d,\"h\":%d},"
                    "\"workspaces\":[",
                    s->twm.active_ws + 1,
                    s->twm.screen_w,
                    s->twm.screen_h);
    for(int i = 0; i < s->twm.ws_count; i++) {
      Workspace  *ws        = &s->twm.workspaces[i];
      bool        is_active = (i == s->twm.active_ws);
      const char *ft        = "";
      const char *fa        = "";
      if(ws->has_focused) {
        Pane *fp = twm_pane_by_id(&s->twm, ws->focused);
        if(fp) {
          ft = fp->title;
          fa = fp->app_id;
        }
      }
      len += snprintf(reply + len,
                      reply_sz - len,
                      "%s{\"index\":%d,\"active\":%s,\"panes\":%d,"
                      "\"layout\":\"%s\",\"ratio\":%.2f,"
                      "\"focused_title\":\"%s\",\"focused_app_id\":\"%s\"",
                      i > 0 ? "," : "",
                      i + 1,
                      is_active ? "true" : "false",
                      ws->pane_count,
                      layout_label(ws->layout),
                      ws->main_ratio,
                      ft,
                      fa);
      if(is_active && ws->pane_count > 0) {
        len += snprintf(reply + len, reply_sz - len, ",\"pane_list\":[");
        for(int j = 0; j < ws->pane_count; j++) {
          Pane *p = twm_pane_by_id(&s->twm, ws->panes[j]);
          if(!p) continue;
          bool is_focused = ws->has_focused && ws->focused == p->id;
          len += snprintf(reply + len,
                          reply_sz - len,
                          "%s{\"id\":%u,\"title\":\"%s\",\"app_id\":\"%s\","
                          "\"focused\":%s,\"floating\":%s,\"fullscreen\":%s}",
                          j > 0 ? "," : "",
                          p->id,
                          p->title,
                          p->app_id,
                          is_focused ? "true" : "false",
                          p->floating ? "true" : "false",
                          p->fullscreen ? "true" : "false");
          if(len >= (int)reply_sz - 128) break;
        }
        len += snprintf(reply + len, reply_sz - len, "]");
      }
      len += snprintf(reply + len, reply_sz - len, "}");
    }
    len += snprintf(reply + len, reply_sz - len, "]}");

    /* ── set_layout ─────────────────────────────────────────────────────── */

  } else if(!strcmp(cmd, "set_layout")) {
    if(!rest || !rest[0]) {
      REPLY("err: usage: set_layout <bsp|spiral|columns|rows|threecol|monocle>");
      return;
    }
    Layout lay = -1;
    if(!strcasecmp(rest, "bsp"))
      lay = LAYOUT_BSP;
    else if(!strcasecmp(rest, "spiral"))
      lay = LAYOUT_SPIRAL;
    else if(!strcasecmp(rest, "columns"))
      lay = LAYOUT_COLUMNS;
    else if(!strcasecmp(rest, "rows"))
      lay = LAYOUT_ROWS;
    else if(!strcasecmp(rest, "threecol"))
      lay = LAYOUT_THREECOL;
    else if(!strcasecmp(rest, "monocle"))
      lay = LAYOUT_MONOCLE;
    if((int)lay < 0) {
      REPLY("err: unknown layout '%s'", rest);
    } else {
      s->twm.workspaces[s->twm.active_ws].layout = lay;
      twm_reflow(&s->twm);
      server_sync_windows(s);
      REPLY("ok: layout %s", layout_label(lay));
    }

    /* ── query_pane ─────────────────────────────────────────────────────── */

  } else if(!strcmp(cmd, "query_pane")) {
    uint32_t qid = rest ? (uint32_t)atoi(rest) : 0;
    if(!qid) qid = (uint32_t)twm_focused_id(&s->twm);
    Pane *p = qid ? twm_pane_by_id(&s->twm, qid) : NULL;
    if(!p) {
      REPLY("err: pane %u not found", qid);
    } else {
      int ws_idx = -1;
      for(int i = 0; i < s->twm.ws_count; i++) {
        for(int j = 0; j < s->twm.workspaces[i].pane_count; j++) {
          if(s->twm.workspaces[i].panes[j] == qid) {
            ws_idx = i;
            break;
          }
        }
        if(ws_idx >= 0) break;
      }
      Rect r = anim_get_rect(&s->anim, qid, p->rect);
      REPLY("{\"id\":%u,\"title\":\"%s\",\"app_id\":\"%s\","
            "\"workspace\":%d,"
            "\"rect\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d},"
            "\"anim_rect\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d},"
            "\"floating\":%s,\"fullscreen\":%s,\"focused\":%s}",
            p->id,
            p->title,
            p->app_id,
            ws_idx + 1,
            p->rect.x,
            p->rect.y,
            p->rect.w,
            p->rect.h,
            r.x,
            r.y,
            r.w,
            r.h,
            p->floating ? "true" : "false",
            p->fullscreen ? "true" : "false",
            (twm_focused_id(&s->twm) == qid) ? "true" : "false");
    }

    /* ── subscribe ──────────────────────────────────────────────────────── */

  } else if(!strcmp(cmd, "subscribe")) {
    REPLY("err: use ipc_subscribe() directly (internal call path required)");

    /* ── Overlay ────────────────────────────────────────────────────────── */

  } else if(!strcmp(cmd, "overlay")) {
    TrixieOutput *o;
    if(!rest || !strcmp(rest, "toggle")) {
      wl_list_for_each(o, &s->outputs, link) if(o->overlay)
          overlay_toggle(o->overlay);
      server_request_redraw(s);
      REPLY("ok: overlay toggled");

    } else if(!strcmp(rest, "show")) {
      wl_list_for_each(
          o, &s->outputs, link) if(o->overlay && !overlay_visible(o->overlay))
          overlay_toggle(o->overlay);
      server_request_redraw(s);
      REPLY("ok: overlay shown");

    } else if(!strcmp(rest, "hide")) {
      wl_list_for_each(
          o, &s->outputs, link) if(o->overlay && overlay_visible(o->overlay))
          overlay_toggle(o->overlay);
      server_request_redraw(s);
      REPLY("ok: overlay hidden");

    } else if(!strncmp(rest, "cd ", 3)) {
      const char *path = rest + 3;
      while(*path == ' ' || *path == '\t')
        path++;
      if(!path[0]) {
        REPLY("err: usage: overlay cd <path>");
        return;
      }
      wl_list_for_each(o, &s->outputs, link) if(o->overlay)
          overlay_set_cwd(o->overlay, path);
      server_request_redraw(s);
      REPLY("ok: cwd → %s", path);

    } else if(!strncmp(rest, "notify ", 7)) {
      char nbuf[512];
      strncpy(nbuf, rest + 7, sizeof(nbuf) - 1);
      char       *sp    = strchr(nbuf, ' ');
      const char *title = nbuf, *msg = "";
      if(sp) {
        *sp = '\0';
        msg = sp + 1;
      }
      wl_list_for_each(o, &s->outputs, link) if(o->overlay)
          overlay_notify(o->overlay, title, msg);
      server_request_redraw(s);
      REPLY("ok: notified");

    } else if(!strncmp(rest, "run ", 4)) {
      const char *runcmd = rest + 4;
      while(*runcmd == ' ' || *runcmd == '\t')
        runcmd++;
      if(!runcmd[0]) {
        REPLY("err: usage: overlay run <cmd>");
        return;
      }
      wl_list_for_each(o, &s->outputs, link) {
        if(o->overlay) {
          if(!overlay_visible(o->overlay)) overlay_toggle(o->overlay);
          overlay_show_panel(o->overlay, "log");
        }
      }
      server_spawn(s, runcmd);
      server_request_redraw(s);
      REPLY("ok: running %s", runcmd);

    } else if(!strncmp(rest, "panel ", 6)) {
      const char *pname = rest + 6;
      while(*pname == ' ' || *pname == '\t')
        pname++;
      wl_list_for_each(o, &s->outputs, link) if(o->overlay)
          overlay_show_panel(o->overlay, pname);
      server_request_redraw(s);
      REPLY("ok: panel %s", pname);

    } else if(!strcmp(rest, "git-refresh")) {
      wl_list_for_each(o, &s->outputs, link) if(o->overlay)
          overlay_git_invalidate(o->overlay);
      server_request_redraw(s);
      REPLY("ok: git refreshed");

    } else {
      REPLY("err: usage: overlay [toggle|show|hide|cd <path>|notify <title> "
            "<msg>|run <cmd>|panel <name>|git-refresh]");
    }

    /* ── Build ──────────────────────────────────────────────────────────── */

  } else if(!strcmp(cmd, "build")) {
    TrixieOutput *o;
    wl_list_for_each(o, &s->outputs, link) {
      if(o->overlay) {
        if(!overlay_visible(o->overlay)) overlay_toggle(o->overlay);
        overlay_key(o->overlay, XKB_KEY_6, 0);
        overlay_key(o->overlay, XKB_KEY_Return, 0);
      }
    }
    server_request_redraw(s);
    REPLY("ok: build triggered");

    /* ── Nvim bridge: nvim_state ────────────────────────────────────────── */

  } else if(!strcmp(cmd, "nvim_state")) {
    if(!rest || !rest[0]) {
      REPLY("err: usage: nvim_state <file>\\t<line>\\t<col>\\t<ft>");
      return;
    }
    char tmp[1024];
    strncpy(tmp, rest, sizeof(tmp) - 1);

    char *file = tmp;
    char *ln_s = strchr(file, '\t');
    if(ln_s)
      *ln_s++ = '\0';
    else
      ln_s = (char *)"0";
    char *col_s = strchr(ln_s, '\t');
    if(col_s)
      *col_s++ = '\0';
    else
      col_s = (char *)"0";
    char *ft = strchr(col_s, '\t');
    if(ft)
      *ft++ = '\0';
    else
      ft = (char *)"";

    strncpy(s->nvim.file, file, sizeof(s->nvim.file) - 1);
    strncpy(s->nvim.filetype, ft, sizeof(s->nvim.filetype) - 1);
    s->nvim.line      = atoi(ln_s);
    s->nvim.col       = atoi(col_s);
    s->nvim.connected = file[0] != '\0';

    /* Forward to overlay nvim panel */
    TrixieOutput *o;
    wl_list_for_each(o, &s->outputs, link) if(o->overlay)
        overlay_nvim_state(o->overlay, file, s->nvim.line, s->nvim.col, ft);

    /* Broadcast to other subscribers */
    char ev[1280];
    snprintf(ev,
             sizeof(ev),
             "{\"event\":\"nvim_state\",\"file\":\"%s\","
             "\"line\":%d,\"col\":%d,\"filetype\":\"%s\"}\n",
             file,
             s->nvim.line,
             s->nvim.col,
             ft);
    ipc_push_event(s, ev, strlen(ev));

    /* Auto-update overlay cwd to the file's directory */
    if(file[0]) {
      char dir[1024];
      strncpy(dir, file, sizeof(dir) - 1);
      char *slash = strrchr(dir, '/');
      if(slash && slash != dir) {
        *slash = '\0';
        wl_list_for_each(o, &s->outputs, link) if(o->overlay)
            overlay_set_cwd(o->overlay, dir);
      }
    }
    REPLY("ok");

    /* ── Nvim bridge: nvim_diag ─────────────────────────────────────────── */

  } else if(!strcmp(cmd, "nvim_diag")) {
    if(!rest) rest = (char *)"[]";
    strncpy(s->nvim.diag_json, rest, sizeof(s->nvim.diag_json) - 1);
    TrixieOutput *o;
    wl_list_for_each(o, &s->outputs, link) if(o->overlay)
        overlay_nvim_diag(o->overlay, rest);
    server_request_redraw(s);
    REPLY("ok");

    /* ── Nvim bridge: nvim_buffers ──────────────────────────────────────── */

  } else if(!strcmp(cmd, "nvim_buffers")) {
    if(!rest) rest = (char *)"[]";
    strncpy(s->nvim.buffers_json, rest, sizeof(s->nvim.buffers_json) - 1);
    TrixieOutput *o;
    wl_list_for_each(o, &s->outputs, link) if(o->overlay)
        overlay_nvim_buffers(o->overlay, rest);
    REPLY("ok");

    /* ── Nvim bridge: quickfix_get ──────────────────────────────────────── */

  } else if(!strcmp(cmd, "quickfix_get")) {
    /* overlay_quickfix_json writes into reply directly */
    overlay_quickfix_json(reply, reply_sz);

    /* ── Nvim bridge: nvim_open ─────────────────────────────────────────── */

  } else if(!strcmp(cmd, "nvim_open")) {
    if(!rest || !rest[0]) {
      REPLY("err: usage: nvim_open <file> [<line>]");
      return;
    }
    char tmp2[1024];
    strncpy(tmp2, rest, sizeof(tmp2) - 1);
    char *sp2 = strchr(tmp2, ' ');
    int   ln2 = 0;
    if(sp2) {
      *sp2++ = '\0';
      ln2    = atoi(sp2);
    }
    if(nvim_is_connected()) {
      nvim_open_file(tmp2, ln2);
      REPLY("ok: opening %s:%d in nvim", tmp2, ln2);
    } else {
      REPLY("err: nvim not connected");
    }
  } else if(!strcmp(cmd, "saturation")) {
    /* saturation <value>          — set all outputs
     * saturation <output> <value> — set one output by name
     * saturation                  — query current value(s) */
    if(!rest || !rest[0]) {
      /* Query */
      int           len = 0;
      char         *p   = reply;
      TrixieOutput *o;
      wl_list_for_each(o, &s->outputs, link) {
        len += snprintf(p + len,
                        reply_sz - (size_t)len,
                        "%s: %.3f\n",
                        o->wlr_output->name,
                        o->saturation);
      }
      if(len == 0) snprintf(reply, reply_sz, "err: no outputs");

    } else {
      /* Parse: optional output name then float value */
      char        arg1[64] = { 0 };
      char        arg2[32] = { 0 };
      int         nargs    = sscanf(rest, "%63s %31s", arg1, arg2);
      float       val;
      const char *target_name = NULL;

      if(nargs == 2) {
        /* saturation <name> <value> */
        target_name = arg1;
        val         = (float)atof(arg2);
      } else {
        /* saturation <value> */
        val = (float)atof(arg1);
      }

      if(val < 0.0f) val = 0.0f;
      if(val > 4.0f) val = 4.0f;

      int           count = 0;
      TrixieOutput *o;
      wl_list_for_each(o, &s->outputs, link) {
        if(target_name && strcmp(o->wlr_output->name, target_name) != 0) continue;
        o->saturation = val;
        /* Also update config so reload preserves the value */
        if(!target_name)
          s->cfg.saturation = val;
        else {
          for(int i = 0; i < s->cfg.monitor_count; i++) {
            if(!strcmp(s->cfg.monitors[i].name, target_name)) {
              s->cfg.monitors[i].saturation = val;
              s->cfg.monitors[i].shader_set = true;
              break;
            }
          }
        }
        count++;
      }
      server_request_redraw(s);

      if(count == 0 && target_name)
        REPLY("err: output '%s' not found", target_name);
      else
        REPLY("ok: saturation %.3f on %d output(s)", val, count);
    }

  } else if(!strcmp(cmd, "shader")) {
    /* shader [on|off]              — toggle/set all outputs
     * shader <output> [on|off]     — set one output by name */
    if(!rest || !rest[0]) {
      /* Toggle all */
      TrixieOutput *o;
      wl_list_for_each(o, &s->outputs, link) {
        o->shader_enabled    = !o->shader_enabled;
        o->shader_init_tried = !o->shader_enabled; /* disable retry when off */
      }
      s->cfg.shader_enabled = !s->cfg.shader_enabled;
      server_request_redraw(s);
      REPLY("ok: shader toggled → %s", s->cfg.shader_enabled ? "on" : "off");

    } else {
      char arg1[64] = { 0 };
      char arg2[8]  = { 0 };
      int  nargs    = sscanf(rest, "%63s %7s", arg1, arg2);

      bool        set_val     = true;
      const char *target_name = NULL;

      if(nargs == 2) {
        target_name = arg1;
        set_val =
            (!strcmp(arg2, "on") || !strcmp(arg2, "1") || !strcmp(arg2, "true"));
      } else {
        /* single arg: on/off or output name */
        if(!strcmp(arg1, "on") || !strcmp(arg1, "1") || !strcmp(arg1, "true")) {
          set_val = true;
        } else if(!strcmp(arg1, "off") || !strcmp(arg1, "0") ||
                  !strcmp(arg1, "false")) {
          set_val = false;
        } else {
          /* treat as output name, toggle */
          target_name = arg1;
          TrixieOutput *o;
          wl_list_for_each(o, &s->outputs, link) {
            if(!strcmp(o->wlr_output->name, target_name)) {
              set_val = !o->shader_enabled;
              break;
            }
          }
        }
      }

      int           count = 0;
      TrixieOutput *o;
      wl_list_for_each(o, &s->outputs, link) {
        if(target_name && strcmp(o->wlr_output->name, target_name) != 0) continue;
        o->shader_enabled    = set_val;
        o->shader_init_tried = !set_val;
        /* Reset gl_init_done so it re-inits next frame when re-enabled */
        if(set_val && !o->shader.gl_init_done) o->shader_init_tried = false;
        count++;
        if(!target_name)
          s->cfg.shader_enabled = set_val;
        else {
          for(int i = 0; i < s->cfg.monitor_count; i++) {
            if(!strcmp(s->cfg.monitors[i].name, target_name)) {
              s->cfg.monitors[i].shader_enabled = set_val;
              s->cfg.monitors[i].shader_set     = true;
              break;
            }
          }
        }
      }
      server_request_redraw(s);

      if(count == 0 && target_name)
        REPLY("err: output '%s' not found", target_name);
      else
        REPLY("ok: shader %s on %d output(s)", set_val ? "on" : "off", count);
    }
  } else {
    REPLY("err: unknown command '%s'", cmd);
  }

#undef REPLY
}

/* ── subscribe variant ──────────────────────────────────────────────────── */

bool ipc_subscribe(TrixieServer *s, int client_fd) {
  if(s->subscriber_count >= MAX_IPC_SUBSCRIBERS) return false;
  int flags = fcntl(client_fd, F_GETFL, 0);
  if(flags >= 0) fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
  s->subscriber_fds[s->subscriber_count++] = client_fd;
  char buf[256];
  snprintf(buf,
           sizeof(buf),
           "{\"event\":\"subscribed\",\"version\":\"%s\","
           "\"workspace\":%d}\n",
           TRIXIE_VERSION_STR,
           s->twm.active_ws + 1);
  write(client_fd, buf, strlen(buf));
  return true;
}
