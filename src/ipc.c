/* ipc.c — Unix socket IPC command dispatcher. */
#define _POSIX_C_SOURCE 200809L
#include "trixie.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* ── Event push helpers ─────────────────────────────────────────────────── */

static void ipc_push_event(TrixieServer *s, const char *buf, size_t len) {
  int live = 0;
  for (int i = 0; i < s->subscriber_count; i++) {
    int fd = s->subscriber_fds[i];
    size_t n = write(fd, buf, len);
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
      close(fd);
    else
      s->subscriber_fds[live++] = fd;
  }
  s->subscriber_count = live;
}

void ipc_push_focus_changed(TrixieServer *s) {
  if (!s || s->subscriber_count == 0)
    return;
  char buf[512];
  PaneId id = twm_focused_id(&s->twm);
  Pane *p = id ? twm_pane_by_id(&s->twm, id) : NULL;
  snprintf(buf, sizeof(buf),
           "{\"event\":\"focus_changed\",\"pane_id\":%u,"
           "\"title\":\"%s\",\"app_id\":\"%s\",\"workspace\":%d}\n",
           id, p ? p->title : "", p ? p->app_id : "", s->twm.active_ws + 1);
  ipc_push_event(s, buf, strlen(buf));
}

void ipc_push_workspace_changed(TrixieServer *s) {
  if (!s || s->subscriber_count == 0)
    return;
  char buf[128];
  snprintf(
      buf, sizeof(buf),
      "{\"event\":\"workspace_changed\",\"workspace\":%d,\"pane_count\":%d}\n",
      s->twm.active_ws + 1, s->twm.workspaces[s->twm.active_ws].pane_count);
  ipc_push_event(s, buf, strlen(buf));
}

void ipc_push_title_changed(TrixieServer *s, PaneId id) {
  if (!s || s->subscriber_count == 0)
    return;
  Pane *p = twm_pane_by_id(&s->twm, id);
  if (!p)
    return;
  char buf[512];
  snprintf(buf, sizeof(buf),
           "{\"event\":\"title_changed\",\"pane_id\":%u,"
           "\"title\":\"%s\",\"app_id\":\"%s\"}\n",
           id, p->title, p->app_id);
  ipc_push_event(s, buf, strlen(buf));
}

/* ── Main dispatcher ────────────────────────────────────────────────────── */

void ipc_dispatch(TrixieServer *s, const char *line, char *reply,
                  size_t reply_sz) {
  wlr_log(WLR_INFO, "ipc: '%s'", line);
  char buf[1024];
  strncpy(buf, line, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  char *cmd = strtok(buf, " \t");
  char *rest = strtok(NULL, "");
  if (rest)
    while (*rest == ' ' || *rest == '\t')
      rest++;
  if (!cmd) {
    snprintf(reply, reply_sz, "err: empty command");
    return;
  }

#define REPLY(...) snprintf(reply, reply_sz, __VA_ARGS__)

  /* Server lifecycle */
  if (!strcmp(cmd, "reload")) {
    server_schedule_reload(s);
    REPLY("ok: config reloaded");
  } else if (!strcmp(cmd, "binary_reload")) {
    server_binary_reload(s);
    REPLY("ok: binary reload initiated");

  } else if (!strcmp(cmd, "quit")) {
    s->running = false;
    wl_display_terminate(s->display);
    REPLY("ok: shutting down");

  } else if (!strcmp(cmd, "switch_vt")) {
    REPLY("err: vt switching not supported");

    /* Workspace */
  } else if (!strcmp(cmd, "workspace")) {
    int n = rest ? atoi(rest) : 0;
    if (n >= 1) {
      int old = s->twm.active_ws;
      twm_switch_ws(&s->twm, n - 1);
      if (s->twm.active_ws != old)
        anim_workspace_transition(
            &s->anim, s->twm.active_ws > old ? WS_DIR_RIGHT : WS_DIR_LEFT);
      server_sync_focus(s);
      server_sync_windows(s);
      ipc_push_workspace_changed(s);
      REPLY("ok: workspace %d", n);
    } else
      REPLY("err: usage: workspace <n>");

  } else if (!strcmp(cmd, "next_workspace")) {
    int old = s->twm.active_ws;
    twm_switch_ws(&s->twm, (old + 1) % s->twm.ws_count);
    if (s->twm.active_ws != old)
      anim_workspace_transition(&s->anim, WS_DIR_RIGHT);
    server_sync_focus(s);
    server_sync_windows(s);
    ipc_push_workspace_changed(s);
    REPLY("ok");

  } else if (!strcmp(cmd, "prev_workspace")) {
    int old = s->twm.active_ws;
    twm_switch_ws(&s->twm, (old + s->twm.ws_count - 1) % s->twm.ws_count);
    if (s->twm.active_ws != old)
      anim_workspace_transition(&s->anim, WS_DIR_LEFT);
    server_sync_focus(s);
    server_sync_windows(s);
    ipc_push_workspace_changed(s);
    REPLY("ok");

  } else if (!strcmp(cmd, "move_to_workspace")) {
    int n = rest ? atoi(rest) : 0;
    if (n >= 1) {
      twm_move_to_ws(&s->twm, n - 1);
      server_sync_windows(s);
      REPLY("ok: moved to workspace %d", n);
    } else
      REPLY("err: usage: move_to_workspace <n>");

    /* Scratchpad */
  } else if (!strcmp(cmd, "scratchpad")) {
    if (!rest || !rest[0]) {
      REPLY("err: usage: scratchpad <name>");
      return;
    }
    server_scratch_toggle(s, rest);
    REPLY("ok: toggled '%s'", rest);

    /* Float */
  } else if (!strcmp(cmd, "float")) {
    server_float_toggle(s);
    REPLY("ok: float toggled");

  } else if (!strcmp(cmd, "float_move")) {
    int dx = 0, dy = 0;
    if (rest)
      sscanf(rest, "%d %d", &dx, &dy);
    PaneId id = twm_focused_id(&s->twm);
    if (id) {
      twm_float_move(&s->twm, id, dx, dy);
      server_sync_windows(s);
      server_request_redraw(s);
      REPLY("ok: moved (%d,%d)", dx, dy);
    } else
      REPLY("err: no focused window");

  } else if (!strcmp(cmd, "float_resize")) {
    int dw = 0, dh = 0;
    if (rest)
      sscanf(rest, "%d %d", &dw, &dh);
    PaneId id = twm_focused_id(&s->twm);
    if (id) {
      twm_float_resize(&s->twm, id, dw, dh);
      server_sync_windows(s);
      server_request_redraw(s);
      REPLY("ok: resized (%d,%d)", dw, dh);
    } else
      REPLY("err: no focused window");

    /* Pane actions */
  } else if (!strcmp(cmd, "close")) {
    PaneId id = twm_focused_id(&s->twm);
    if (id) {
      TrixieView *v = view_from_pane(s, id);
      if (v)
        wlr_xdg_toplevel_send_close(v->xdg_toplevel);
    }
    REPLY("ok: close sent");

  } else if (!strcmp(cmd, "fullscreen")) {
    Action a = {.kind = ACTION_FULLSCREEN};
    server_dispatch_action(s, &a);
    REPLY("ok: fullscreen toggled");

    /* Focus */
  } else if (!strcmp(cmd, "focus")) {
    if (!rest) {
      REPLY("err: usage: focus <left|right|up|down>");
      return;
    }
    Action a = {0};
    if (!strcmp(rest, "left"))
      a.kind = ACTION_FOCUS_LEFT;
    else if (!strcmp(rest, "right"))
      a.kind = ACTION_FOCUS_RIGHT;
    else if (!strcmp(rest, "up"))
      a.kind = ACTION_FOCUS_UP;
    else if (!strcmp(rest, "down"))
      a.kind = ACTION_FOCUS_DOWN;
    else {
      REPLY("err: usage: focus <left|right|up|down>");
      return;
    }
    server_dispatch_action(s, &a);
    ipc_push_focus_changed(s);
    REPLY("ok");

    /* Swap */
  } else if (!strcmp(cmd, "swap")) {
    bool forward = !(rest && !strcmp(rest, "back"));
    twm_swap(&s->twm, forward);
    server_sync_windows(s);
    REPLY("ok: swapped %s", forward ? "forward" : "back");

  } else if (!strcmp(cmd, "swap_main")) {
    twm_swap_main(&s->twm);
    server_sync_windows(s);
    REPLY("ok: swapped with main");

    /* Layout */
  } else if (!strcmp(cmd, "layout")) {
    if (!rest || !strcmp(rest, "next") || !strcmp(rest, "")) {
      Action a = {.kind = ACTION_NEXT_LAYOUT};
      server_dispatch_action(s, &a);
      REPLY("ok: layout %s",
            layout_label(s->twm.workspaces[s->twm.active_ws].layout));
    } else if (!strcmp(rest, "prev")) {
      Action a = {.kind = ACTION_PREV_LAYOUT};
      server_dispatch_action(s, &a);
      REPLY("ok: layout %s",
            layout_label(s->twm.workspaces[s->twm.active_ws].layout));
    } else
      REPLY("err: usage: layout <next|prev>");

  } else if (!strcmp(cmd, "grow_main")) {
    Action a = {.kind = ACTION_GROW_MAIN};
    server_dispatch_action(s, &a);
    REPLY("ok: ratio %.2f", s->twm.workspaces[s->twm.active_ws].main_ratio);

  } else if (!strcmp(cmd, "shrink_main")) {
    Action a = {.kind = ACTION_SHRINK_MAIN};
    server_dispatch_action(s, &a);
    REPLY("ok: ratio %.2f", s->twm.workspaces[s->twm.active_ws].main_ratio);

  } else if (!strcmp(cmd, "ratio")) {
    if (!rest) {
      REPLY("err: usage: ratio <0.1..0.9>");
      return;
    }
    float r = (float)atof(rest);
    if (r < 0.1f)
      r = 0.1f;
    if (r > 0.9f)
      r = 0.9f;
    s->twm.workspaces[s->twm.active_ws].main_ratio = r;
    twm_reflow(&s->twm);
    server_sync_windows(s);
    REPLY("ok: ratio %.2f", r);

    /* Spawn */
  } else if (!strcmp(cmd, "spawn")) {
    if (!rest || !rest[0]) {
      REPLY("err: usage: spawn <cmd>");
      return;
    }
    server_spawn(s, rest);
    REPLY("ok: spawned");

    /* DPMS */
  } else if (!strcmp(cmd, "dpms")) {
    bool on = !rest || !strcmp(rest, "on");
    TrixieOutput *o;
    wl_list_for_each(o, &s->outputs, link) {
      struct wlr_output_state st;
      wlr_output_state_init(&st);
      wlr_output_state_set_enabled(&st, on);
      wlr_output_commit_state(o->wlr_output, &st);
      wlr_output_state_finish(&st);
    }
    REPLY("ok: dpms %s", on ? "on" : "off");

  } else if (!strcmp(cmd, "idle_reset")) {
    server_reset_idle(s);
    REPLY("ok: idle timer reset");

    /* Status */
  } else if (!strcmp(cmd, "status")) {
    int len = 0;
    len += snprintf(reply + len, reply_sz - len,
                    "active_workspace: %d\nscreen: %dx%d\n",
                    s->twm.active_ws + 1, s->twm.screen_w, s->twm.screen_h);
    for (int i = 0; i < s->twm.ws_count; i++) {
      Workspace *ws = &s->twm.workspaces[i];
      len += snprintf(reply + len, reply_sz - len,
                      "workspace %d: panes=%d layout=%s ratio=%.2f\n", i + 1,
                      ws->pane_count, layout_label(ws->layout), ws->main_ratio);
      if (i == s->twm.active_ws) {
        for (int j = 0; j < ws->pane_count && len < (int)reply_sz - 80; j++) {
          Pane *p = twm_pane_by_id(&s->twm, ws->panes[j]);
          if (!p)
            continue;
          len += snprintf(reply + len, reply_sz - len, "  pane %u: %s%s [%s]\n",
                          p->id, p->title,
                          ws->has_focused && ws->focused == p->id ? " (focused)"
                                                                  : "",
                          p->floating     ? "float"
                          : p->fullscreen ? "fullscreen"
                                          : "tiled");
        }
      }
    }
    if (s->twm.scratch_count > 0) {
      len += snprintf(reply + len, reply_sz - len, "scratchpads:\n");
      for (int i = 0; i < s->twm.scratch_count && len < (int)reply_sz - 80;
           i++) {
        Scratchpad *sp = &s->twm.scratchpads[i];
        len += snprintf(
            reply + len, reply_sz - len, "  %s: has_pane=%s visible=%s\n",
            sp->name, sp->has_pane ? "yes" : "no", sp->visible ? "yes" : "no");
      }
    }

  } else if (!strcmp(cmd, "status_json")) {
    int len = 0;
    len += snprintf(reply + len, reply_sz - len,
                    "{\"active_workspace\":%d,\"screen\":{\"w\":%d,\"h\":%d},"
                    "\"workspaces\":[",
                    s->twm.active_ws + 1, s->twm.screen_w, s->twm.screen_h);
    for (int i = 0; i < s->twm.ws_count; i++) {
      Workspace *ws = &s->twm.workspaces[i];
      bool is_active = (i == s->twm.active_ws);
      const char *ft = "", *fa = "";
      if (ws->has_focused) {
        Pane *fp = twm_pane_by_id(&s->twm, ws->focused);
        if (fp) {
          ft = fp->title;
          fa = fp->app_id;
        }
      }
      len += snprintf(reply + len, reply_sz - len,
                      "%s{\"index\":%d,\"active\":%s,\"panes\":%d,\"layout\":"
                      "\"%s\",\"ratio\":%.2f,"
                      "\"focused_title\":\"%s\",\"focused_app_id\":\"%s\"",
                      i > 0 ? "," : "", i + 1, is_active ? "true" : "false",
                      ws->pane_count, layout_label(ws->layout), ws->main_ratio,
                      ft, fa);
      if (is_active && ws->pane_count > 0) {
        len += snprintf(reply + len, reply_sz - len, ",\"pane_list\":[");
        for (int j = 0; j < ws->pane_count; j++) {
          Pane *p = twm_pane_by_id(&s->twm, ws->panes[j]);
          if (!p)
            continue;
          bool is_focused = ws->has_focused && ws->focused == p->id;
          len += snprintf(reply + len, reply_sz - len,
                          "%s{\"id\":%u,\"title\":\"%s\",\"app_id\":\"%s\","
                          "\"focused\":%s,\"floating\":%s,\"fullscreen\":%s}",
                          j > 0 ? "," : "", p->id, p->title, p->app_id,
                          is_focused ? "true" : "false",
                          p->floating ? "true" : "false",
                          p->fullscreen ? "true" : "false");
          if (len >= (int)reply_sz - 128)
            break;
        }
        len += snprintf(reply + len, reply_sz - len, "]");
      }
      len += snprintf(reply + len, reply_sz - len, "}");
    }
    len += snprintf(reply + len, reply_sz - len, "]");
    len += ipc_scratch_json(&s->twm, reply + len, reply_sz - (size_t)len);
    len += snprintf(reply + len, reply_sz - len, "}");

  } else if (!strcmp(cmd, "clients")) {
    bool plain = (rest && !strcmp(rest, "plain"));
    int len = 0;
    PaneId focused_id = twm_focused_id(&s->twm);
    if (plain) {
      len += snprintf(reply + len, reply_sz - len,
                      "%-6s %-3s %-10s %-8s %-8s %s\n", "ID", "WS", "APP_ID",
                      "FLOAT", "FOCUSED", "TITLE");
      len += snprintf(reply + len, reply_sz - len,
                      "%-6s %-3s %-10s %-8s %-8s %s\n", "------", "---",
                      "----------", "--------", "--------", "-----");
      for (int i = 0; i < s->twm.ws_count && len < (int)reply_sz - 128; i++) {
        Workspace *ws = &s->twm.workspaces[i];
        for (int j = 0; j < ws->pane_count && len < (int)reply_sz - 128; j++) {
          Pane *p = twm_pane_by_id(&s->twm, ws->panes[j]);
          if (!p)
            continue;
          len += snprintf(reply + len, reply_sz - len,
                          "%-6u %-3d %-10s %-8s %-8s %s\n", p->id, i + 1,
                          p->app_id[0] ? p->app_id : "(none)",
                          p->floating ? "yes" : "no",
                          (p->id == focused_id) ? "yes" : "no", p->title);
        }
      }
    } else {
      len += snprintf(reply + len, reply_sz - len, "[");
      bool first = true;
      for (int i = 0; i < s->twm.ws_count && len < (int)reply_sz - 256; i++) {
        Workspace *ws = &s->twm.workspaces[i];
        for (int j = 0; j < ws->pane_count && len < (int)reply_sz - 256; j++) {
          Pane *p = twm_pane_by_id(&s->twm, ws->panes[j]);
          if (!p)
            continue;
          bool is_focused = (p->id == focused_id);
          len += snprintf(reply + len, reply_sz - len,
                          "%s{\"id\":%u,\"app_id\":\"%s\",\"title\":\"%s\","
                          "\"workspace\":%d,\"floating\":%s,\"fullscreen\":%s,"
                          "\"focused\":%s}",
                          first ? "" : ",", (unsigned)p->id, p->app_id,
                          p->title, i + 1, p->floating ? "true" : "false",
                          p->fullscreen ? "true" : "false",
                          is_focused ? "true" : "false");
          first = false;
        }
      }
      len += snprintf(reply + len, reply_sz - len, "]");
    }

  } else if (!strcmp(cmd, "set_layout")) {
    if (!rest || !rest[0]) {
      REPLY("err: usage: set_layout <dwindle|columns|rows|threecol|monocle>");
      return;
    }
    Layout lay = -1;
    if (!strcasecmp(rest, "dwindle"))
      lay = LAYOUT_DWINDLE;
    else if (!strcasecmp(rest, "columns") || !strcasecmp(rest, "cols"))
      lay = LAYOUT_COLUMNS;
    else if (!strcasecmp(rest, "rows"))
      lay = LAYOUT_ROWS;
    else if (!strcasecmp(rest, "threecol") || !strcasecmp(rest, "three"))
      lay = LAYOUT_THREECOL;
    else if (!strcasecmp(rest, "monocle"))
      lay = LAYOUT_MONOCLE;
    if ((int)lay < 0) {
      REPLY("err: unknown layout '%s'", rest);
    } else {
      s->twm.workspaces[s->twm.active_ws].layout = lay;
      twm_reflow(&s->twm);
      server_sync_windows(s);
      REPLY("ok: layout %s", layout_label(lay));
    }

  } else if (!strcmp(cmd, "query_pane")) {
    uint32_t qid = rest ? (uint32_t)atoi(rest) : 0;
    if (!qid)
      qid = (uint32_t)twm_focused_id(&s->twm);
    Pane *p = qid ? twm_pane_by_id(&s->twm, qid) : NULL;
    if (!p) {
      REPLY("err: pane %u not found", qid);
    } else {
      int ws_idx = -1;
      for (int i = 0; i < s->twm.ws_count; i++)
        for (int j = 0; j < s->twm.workspaces[i].pane_count; j++)
          if (s->twm.workspaces[i].panes[j] == qid) {
            ws_idx = i;
            break;
          }
      const char *scratch_name = "";
      bool in_scratch = false;
      for (int i = 0; i < s->twm.scratch_count; i++)
        if (s->twm.scratchpads[i].has_pane &&
            s->twm.scratchpads[i].pane_id == qid) {
          scratch_name = s->twm.scratchpads[i].name;
          in_scratch = true;
          break;
        }
      Rect r = anim_get_rect(&s->anim, qid, p->rect);
      REPLY("{\"id\":%u,\"title\":\"%s\",\"app_id\":\"%s\",\"workspace\":%d,"
            "\"scratchpad\":\"%s\","
            "\"rect\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d},"
            "\"anim_rect\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d},"
            "\"floating\":%s,\"fullscreen\":%s,\"focused\":%s}",
            p->id, p->title, p->app_id, ws_idx + 1,
            in_scratch ? scratch_name : "", p->rect.x, p->rect.y, p->rect.w,
            p->rect.h, r.x, r.y, r.w, r.h, p->floating ? "true" : "false",
            p->fullscreen ? "true" : "false",
            (twm_focused_id(&s->twm) == qid) ? "true" : "false");
    }

  } else if (!strcmp(cmd, "subscribe")) {
    REPLY("err: use ipc_subscribe() directly");

  } else {
    REPLY("err: unknown command '%s'", cmd);
  }

#undef REPLY
}

/* ── subscribe ──────────────────────────────────────────────────────────── */

bool ipc_subscribe(TrixieServer *s, int client_fd) {
  if (s->subscriber_count >= MAX_IPC_SUBSCRIBERS)
    return false;
  int flags = fcntl(client_fd, F_GETFL, 0);
  if (flags >= 0)
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
  s->subscriber_fds[s->subscriber_count++] = client_fd;
  char buf[256];
  snprintf(buf, sizeof(buf),
           "{\"event\":\"subscribed\",\"version\":\"%s\",\"workspace\":%d}\n",
           TRIXIE_VERSION_STR, s->twm.active_ws + 1);
  write(client_fd, buf, strlen(buf));
  return true;
}
