/* main.c — Trixie compositor entry point, wlroots backend & event loop */
#include "trixie.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wlr/types/wlr_xdg_shell.h>

/* ── Forward declarations ─────────────────────────────────────────────────── */

static void handle_new_output(struct wl_listener *l, void *data);
static void handle_new_input(struct wl_listener *l, void *data);
static void handle_new_xdg_surface(struct wl_listener *l, void *data);
static void handle_new_layer_surface(struct wl_listener *l, void *data);
static void handle_new_deco(struct wl_listener *l, void *data);
static void handle_cursor_motion(struct wl_listener *l, void *data);
static void handle_cursor_motion_abs(struct wl_listener *l, void *data);
static void handle_cursor_button(struct wl_listener *l, void *data);
static void handle_cursor_axis(struct wl_listener *l, void *data);
static void handle_cursor_frame(struct wl_listener *l, void *data);
static void handle_request_cursor(struct wl_listener *l, void *data);
static void handle_request_set_selection(struct wl_listener *l, void *data);
static void handle_request_set_primary_selection(struct wl_listener *l, void *data);

static void view_handle_map(struct wl_listener *l, void *data);
static void view_handle_unmap(struct wl_listener *l, void *data);
static void view_handle_destroy(struct wl_listener *l, void *data);
static void view_handle_commit(struct wl_listener *l, void *data);
static void view_handle_request_fullscreen(struct wl_listener *l, void *data);
static void view_handle_set_title(struct wl_listener *l, void *data);
static void view_handle_set_app_id(struct wl_listener *l, void *data);

static void output_handle_frame(struct wl_listener *l, void *data);
static void output_handle_request_state(struct wl_listener *l, void *data);
static void output_handle_destroy(struct wl_listener *l, void *data);

static void keyboard_handle_key(struct wl_listener *l, void *data);
static void keyboard_handle_modifiers(struct wl_listener *l, void *data);
static void keyboard_handle_destroy(struct wl_listener *l, void *data);

/* ── Utility ──────────────────────────────────────────────────────────────── */

#define CONTAINER_OF(ptr, type, member) \
  ((type *)((char *)(ptr) - offsetof(type, member)))

/* ── Spawn ────────────────────────────────────────────────────────────────── */

void server_spawn(TrixieServer *s, const char *cmd) {
  if(!cmd || !cmd[0]) return;
  wlr_log(WLR_INFO,
          "SPAWN: cmd='%s' WAYLAND_DISPLAY='%s'",
          cmd,
          getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "(unset)");

  pid_t pid = fork();
  if(pid < 0) {
    wlr_log(WLR_ERROR, "SPAWN: fork: %s", strerror(errno));
    return;
  }
  if(pid == 0) {
    pid_t pid2 = fork();
    if(pid2 < 0) _exit(1);
    if(pid2 == 0) {
      setsid();

      /* Close wlroots fds but keep stdin/stdout/stderr */
      int maxfd = 256;
      for(int fd = 3; fd < maxfd; fd++)
        close(fd);

      /* Redirect stdout+stderr to a log for debugging */
      int logfd = open("/tmp/trixie-spawn.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
      if(logfd >= 0) {
        dup2(logfd, STDOUT_FILENO);
        dup2(logfd, STDERR_FILENO);
        close(logfd);
      }

      /* Ensure a sane PATH so the binary can be found */
      if(!getenv("PATH")) setenv("PATH", "/usr/local/bin:/usr/bin:/bin", 1);

      /* Log what we are about to exec so trixie-spawn.log is useful */
      dprintf(STDOUT_FILENO,
              "trixie-spawn: execing '%s' WAYLAND_DISPLAY=%s\n",
              cmd,
              getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "(unset)");

      execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
      dprintf(STDERR_FILENO, "trixie-spawn: execl failed: %s\n", strerror(errno));
      _exit(127);
    }
    _exit(0);
  }
  waitpid(pid, NULL, 0);
  wlr_log(WLR_INFO, "SPAWN: fork done");
}

/* ── Focus ────────────────────────────────────────────────────────────────── */

TrixieView *view_from_pane(TrixieServer *s, PaneId id) {
  TrixieView *v;
  wl_list_for_each(v, &s->views, link) {
    if(v->pane_id == id && v->mapped) return v;
  }
  return NULL;
}

TrixieView *view_at(TrixieServer        *s,
                    double               lx,
                    double               ly,
                    struct wlr_surface **surf_out,
                    double              *sx,
                    double              *sy) {
  struct wlr_scene_node *node =
      wlr_scene_node_at(&s->scene->tree.node, lx, ly, sx, sy);
  if(!node || node->type != WLR_SCENE_NODE_BUFFER) return NULL;
  struct wlr_scene_buffer  *sb = wlr_scene_buffer_from_node(node);
  struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(sb);
  if(!ss) return NULL;
  if(surf_out) *surf_out = ss->surface;
  /* Walk up to find the TrixieView owning this tree */
  struct wlr_scene_tree *tree = node->parent;
  while(tree && &tree->node != &s->scene->tree.node) {
    TrixieView *v;
    wl_list_for_each(v, &s->views, link) {
      if(v->scene_tree == tree && v->mapped) return v;
    }
    tree = tree->node.parent;
  }
  return NULL;
}

/* Trigger / refresh an animation for a pane whose rect just changed.
 * Used after config reloads and direct rect mutations outside of twm_reflow. */
void server_apply_anim(TrixieServer *s, PaneId id) {
  Pane *p = twm_pane_by_id(&s->twm, id);
  if(!p) return;
  /* Only morph — open/close anims are handled at map/unmap time */
  if(!anim_is_closing(&s->anim, id))
    anim_morph(&s->anim, id, p->rect, p->rect);
  server_request_redraw(s);
}

void server_focus_pane(TrixieServer *s, PaneId id) {
  TrixieView *v = view_from_pane(s, id);
  if(!v) return;

  struct wlr_surface  *surf = v->xdg_toplevel->base->surface;
  struct wlr_keyboard *kb   = wlr_seat_get_keyboard(s->seat);

  wlr_scene_node_raise_to_top(&v->scene_tree->node);
  if(kb)
    wlr_seat_keyboard_notify_enter(
        s->seat, surf, kb->keycodes, kb->num_keycodes, &kb->modifiers);
}

void server_sync_focus(TrixieServer *s) {
  PaneId id = twm_focused_id(&s->twm);
  if(id) server_focus_pane(s, id);
}

/* ── Window sync ──────────────────────────────────────────────────────────── */

void server_sync_windows(TrixieServer *s) {
  Workspace *ws         = &s->twm.workspaces[s->twm.active_ws];
  int        incoming_x = anim_ws_incoming_x(&s->anim);

  TrixieView *v;
  wl_list_for_each(v, &s->views, link) {
    if(!v->mapped) continue;
    Pane *p = twm_pane_by_id(&s->twm, v->pane_id);
    if(!p) continue;

    bool on_ws = false;
    for(int i = 0; i < ws->pane_count; i++) {
      if(ws->panes[i] == v->pane_id) {
        on_ws = true;
        break;
      }
    }

    bool is_closing = anim_is_closing(&s->anim, v->pane_id);

    wlr_log(WLR_INFO,
            "SYNC DIAG: pane=%u on_ws=%d is_closing=%d mapped=%d "
            "rect=(%d,%d,%d,%d) node.enabled=%d node.parent=%p",
            v->pane_id,
            on_ws,
            is_closing,
            v->mapped,
            p->rect.x,
            p->rect.y,
            p->rect.w,
            p->rect.h,
            v->scene_tree->node.enabled,
            (void *)v->scene_tree->node.parent);

    if(!on_ws && !is_closing) continue;

    Rect r  = anim_get_rect(&s->anim, v->pane_id, p->rect);
    int  bw = (p->floating || p->fullscreen) ? 0 : s->twm.border_w;

    int win_x = r.x + bw + incoming_x;
    int win_y = r.y + bw;
    int win_w = r.w - bw * 2;
    int win_h = r.h - bw * 2;
    if(win_w < 1) win_w = 1;
    if(win_h < 1) win_h = 1;

    wlr_log(WLR_DEBUG,
            "SYNC: pane=%u pos=(%d,%d) size=%dx%d node_enabled=%d",
            v->pane_id,
            win_x,
            win_y,
            win_w,
            win_h,
            v->scene_tree->node.enabled);

    wlr_scene_node_set_position(&v->scene_tree->node, win_x, win_y);
    wlr_log(WLR_INFO,
            "SYNC DIAG: positioned pane=%u at (%d,%d) size=(%d,%d)",
            v->pane_id,
            win_x,
            win_y,
            win_w,
            win_h);

    if(!anim_is_closing(&s->anim, v->pane_id)) {
      int cfg_w = p->rect.w - bw * 2;
      int cfg_h = p->rect.h - bw * 2;
      if(cfg_w < 1) cfg_w = 1;
      if(cfg_h < 1) cfg_h = 1;
      struct wlr_xdg_toplevel *tl = v->xdg_toplevel;
      struct wlr_box           cur;
      wlr_xdg_surface_get_geometry(tl->base, &cur);
      if(cur.width != cfg_w || cur.height != cfg_h)
        wlr_xdg_toplevel_set_size(tl, cfg_w, cfg_h);
    }
  }
}

/* ── Redraw request ───────────────────────────────────────────────────────── */

void server_request_redraw(TrixieServer *s) {
  TrixieOutput *o;
  wl_list_for_each(o, &s->outputs, link) wlr_output_schedule_frame(o->wlr_output);
}

/* ── Action dispatch ──────────────────────────────────────────────────────── */

void server_dispatch_action(TrixieServer *s, Action *a) {
  switch(a->kind) {
    case ACTION_QUIT:
      s->running = false;
      wl_display_terminate(s->display);
      break;

    case ACTION_RELOAD: server_apply_config_reload(s); break;

    case ACTION_EXEC:
      wlr_log(WLR_INFO, "action: exec '%s'", a->exec_cmd);
      server_spawn(s, a->exec_cmd);
      break;

    case ACTION_CLOSE: {
      PaneId id = twm_focused_id(&s->twm);
      if(id) {
        TrixieView *v = view_from_pane(s, id);
        if(v) wlr_xdg_toplevel_send_close(v->xdg_toplevel);
      }
      break;
    }

    case ACTION_FULLSCREEN: {
      PaneId id = twm_focused_id(&s->twm);
      Pane  *p  = id ? twm_pane_by_id(&s->twm, id) : NULL;
      if(p) {
        p->fullscreen = !p->fullscreen;
        TrixieView *v = view_from_pane(s, id);
        if(v) {
          wlr_xdg_toplevel_set_fullscreen(v->xdg_toplevel, p->fullscreen);
          /* move between layers */
          struct wlr_scene_tree *tgt =
              p->fullscreen ? s->layer_floating : s->layer_windows;
          wlr_scene_node_reparent(&v->scene_tree->node, tgt);
          wlr_scene_node_raise_to_top(&v->scene_tree->node);
        }
        twm_reflow(&s->twm);
        server_sync_windows(s);
      }
      break;
    }

    case ACTION_TOGGLE_FLOAT: server_float_toggle(s); break;

    case ACTION_TOGGLE_BAR:
      s->twm.bar_visible = !s->twm.bar_visible;
      twm_set_bar_height(&s->twm,
                         s->twm.bar_visible ? s->twm.bar_rect.h : 0,
                         s->cfg.bar.position == BAR_BOTTOM);
      server_request_redraw(s);
      break;

    case ACTION_FOCUS_LEFT:
      twm_focus_dir(&s->twm, -1, 0);
      server_sync_focus(s);
      break;
    case ACTION_FOCUS_RIGHT:
      twm_focus_dir(&s->twm, 1, 0);
      server_sync_focus(s);
      break;
    case ACTION_FOCUS_UP:
      twm_focus_dir(&s->twm, 0, -1);
      server_sync_focus(s);
      break;
    case ACTION_FOCUS_DOWN:
      twm_focus_dir(&s->twm, 0, 1);
      server_sync_focus(s);
      break;

    case ACTION_MOVE_LEFT:
      twm_swap(&s->twm, false);
      server_sync_windows(s);
      break;
    case ACTION_MOVE_RIGHT:
      twm_swap(&s->twm, true);
      server_sync_windows(s);
      break;

    case ACTION_WORKSPACE: {
      int old = s->twm.active_ws;
      twm_switch_ws(&s->twm, a->n - 1);
      int nw = s->twm.active_ws;
      if(nw != old)
        anim_workspace_transition(&s->anim, nw > old ? WS_DIR_RIGHT : WS_DIR_LEFT);
      server_sync_focus(s);
      server_sync_windows(s);
      break;
    }

    case ACTION_MOVE_TO_WS:
      twm_move_to_ws(&s->twm, a->n - 1);
      server_sync_windows(s);
      break;

    case ACTION_NEXT_WS: {
      int old = s->twm.active_ws;
      twm_switch_ws(&s->twm, (old + 1) % s->twm.ws_count);
      if(s->twm.active_ws != old) anim_workspace_transition(&s->anim, WS_DIR_RIGHT);
      server_sync_focus(s);
      server_sync_windows(s);
      break;
    }

    case ACTION_PREV_WS: {
      int old = s->twm.active_ws;
      twm_switch_ws(&s->twm, (old + s->twm.ws_count - 1) % s->twm.ws_count);
      if(s->twm.active_ws != old) anim_workspace_transition(&s->anim, WS_DIR_LEFT);
      server_sync_focus(s);
      server_sync_windows(s);
      break;
    }

    case ACTION_NEXT_LAYOUT: {
      Workspace *ws = &s->twm.workspaces[s->twm.active_ws];
      ws->layout    = layout_next(ws->layout);
      twm_reflow(&s->twm);
      server_sync_windows(s);
      break;
    }
    case ACTION_PREV_LAYOUT: {
      Workspace *ws = &s->twm.workspaces[s->twm.active_ws];
      ws->layout    = layout_prev(ws->layout);
      twm_reflow(&s->twm);
      server_sync_windows(s);
      break;
    }

    case ACTION_GROW_MAIN: {
      Workspace *ws = &s->twm.workspaces[s->twm.active_ws];
      ws->main_ratio += 0.05f;
      if(ws->main_ratio > 0.9f) ws->main_ratio = 0.9f;
      twm_reflow(&s->twm);
      server_sync_windows(s);
      break;
    }
    case ACTION_SHRINK_MAIN: {
      Workspace *ws = &s->twm.workspaces[s->twm.active_ws];
      ws->main_ratio -= 0.05f;
      if(ws->main_ratio < 0.1f) ws->main_ratio = 0.1f;
      twm_reflow(&s->twm);
      server_sync_windows(s);
      break;
    }

    case ACTION_SCRATCHPAD: server_scratch_toggle(s, a->name); break;
    case ACTION_SWITCH_VT: break;
  }
}

/* ── Float / scratch ──────────────────────────────────────────────────────── */

void server_float_toggle(TrixieServer *s) {
  PaneId id = twm_focused_id(&s->twm);
  if(!id) return;
  Pane *p = twm_pane_by_id(&s->twm, id);
  if(!p) return;
  bool was_float = p->floating;
  twm_toggle_float(&s->twm);

  TrixieView *v = view_from_pane(s, id);
  if(v) {
    struct wlr_scene_tree *tgt = p->floating ? s->layer_floating : s->layer_windows;
    wlr_scene_node_reparent(&v->scene_tree->node, tgt);
    wlr_scene_node_raise_to_top(&v->scene_tree->node);
  }

  if(p->floating && !was_float)
    anim_float_open(&s->anim, id, p->rect);
  else if(!p->floating && was_float)
    anim_open(&s->anim, id, p->rect);

  server_sync_windows(s);
  server_sync_focus(s);
}

void server_scratch_toggle(TrixieServer *s, const char *name) {
  Scratchpad *sp = NULL;
  for(int i = 0; i < s->twm.scratch_count; i++) {
    if(!strcmp(s->twm.scratchpads[i].name, name)) {
      sp = &s->twm.scratchpads[i];
      break;
    }
  }
  if(!sp) return;

  bool   was_visible = sp->visible;
  PaneId pid         = sp->has_pane ? sp->pane_id : 0;

  twm_toggle_scratch(&s->twm, name);

  if(pid) {
    Pane *p = twm_pane_by_id(&s->twm, pid);
    if(p) {
      if(!was_visible && sp->visible)
        anim_scratch_open(&s->anim, pid, p->rect);
      else if(was_visible && !sp->visible)
        anim_scratch_close(&s->anim, pid, p->rect);
    }
  }
  server_sync_windows(s);
  server_sync_focus(s);
}

/* ── Config reload ────────────────────────────────────────────────────────── */

void server_apply_config_reload(TrixieServer *s) {
  config_reload(&s->cfg);
  s->twm.gap        = s->cfg.gap;
  s->twm.border_w   = s->cfg.border_width;
  s->twm.smart_gaps = s->cfg.smart_gaps;
  twm_set_bar_height(&s->twm, s->cfg.bar.height, s->cfg.bar.position == BAR_BOTTOM);
  wlr_xcursor_manager_load(s->xcursor_mgr, s->cfg.cursor_size);

  TrixieKeyboard *kb;
  wl_list_for_each(kb, &s->keyboards, link) wlr_keyboard_set_repeat_info(
      kb->wlr_keyboard, s->cfg.keyboard.repeat_rate, s->cfg.keyboard.repeat_delay);
  server_request_redraw(s);
  wlr_log(WLR_INFO, "Config reloaded");
}

/* ── Keyboard ─────────────────────────────────────────────────────────────── */

static uint32_t wlr_mods_to_trixie(uint32_t mods) {
  uint32_t out = 0;
  if(mods & WLR_MODIFIER_LOGO) out |= MOD_SUPER;
  if(mods & WLR_MODIFIER_CTRL) out |= MOD_CTRL;
  if(mods & WLR_MODIFIER_ALT) out |= MOD_ALT;
  if(mods & WLR_MODIFIER_SHIFT) out |= MOD_SHIFT;
  return out;
}

static void keyboard_handle_key(struct wl_listener *listener, void *data) {
  TrixieKeyboard                *kb    = CONTAINER_OF(listener, TrixieKeyboard, key);
  TrixieServer                  *s     = kb->server;
  struct wlr_keyboard_key_event *event = data;
  struct wlr_seat               *seat  = s->seat;

  uint32_t            keycode = event->keycode + 8;
  const xkb_keysym_t *syms;
  int  nsyms   = xkb_state_key_get_syms(kb->wlr_keyboard->xkb_state, keycode, &syms);
  bool handled = false;

  uint32_t mods = wlr_mods_to_trixie(wlr_keyboard_get_modifiers(kb->wlr_keyboard));

  if(event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
    for(int i = 0; i < nsyms; i++) {
      if(syms[i] == XKB_KEY_Print && (mods & MOD_SUPER) && (mods & MOD_SHIFT)) {
        wlr_log(WLR_INFO, "Emergency quit");
        s->running = false;
        wl_display_terminate(s->display);
        return;
      }
    }
    for(int i = 0; i < nsyms && !handled; i++) {
      for(int ki = 0; ki < s->cfg.keybind_count; ki++) {
        Keybind *bind = &s->cfg.keybinds[ki];
        if(bind->sym == syms[i] && bind->mods == mods) {
          server_dispatch_action(s, &bind->action);
          handled = true;
          break;
        }
      }
    }
  }

  if(!handled) {
    wlr_seat_set_keyboard(seat, kb->wlr_keyboard);
    wlr_seat_keyboard_notify_key(
        seat, event->time_msec, event->keycode, event->state);
  }
}

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data) {
  TrixieKeyboard *kb = CONTAINER_OF(listener, TrixieKeyboard, modifiers);
  wlr_seat_set_keyboard(kb->server->seat, kb->wlr_keyboard);
  wlr_seat_keyboard_notify_modifiers(kb->server->seat, &kb->wlr_keyboard->modifiers);
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
  TrixieKeyboard *kb = CONTAINER_OF(listener, TrixieKeyboard, destroy);
  wl_list_remove(&kb->modifiers.link);
  wl_list_remove(&kb->key.link);
  wl_list_remove(&kb->destroy.link);
  wl_list_remove(&kb->link);
  free(kb);
}

static void server_new_keyboard(TrixieServer *s, struct wlr_input_device *dev) {
  struct wlr_keyboard *wlr_kb = wlr_keyboard_from_input_device(dev);
  TrixieKeyboard      *kb     = calloc(1, sizeof(*kb));
  kb->server                  = s;
  kb->wlr_keyboard            = wlr_kb;

  struct xkb_context   *ctx   = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  struct xkb_rule_names rules = {
    .layout  = s->cfg.keyboard.kb_layout[0] ? s->cfg.keyboard.kb_layout : NULL,
    .variant = s->cfg.keyboard.kb_variant[0] ? s->cfg.keyboard.kb_variant : NULL,
    .options = s->cfg.keyboard.kb_options[0] ? s->cfg.keyboard.kb_options : NULL,
  };
  struct xkb_keymap *keymap =
      xkb_keymap_new_from_names(ctx, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);
  wlr_keyboard_set_keymap(wlr_kb, keymap);
  xkb_keymap_unref(keymap);
  xkb_context_unref(ctx);

  wlr_keyboard_set_repeat_info(
      wlr_kb, s->cfg.keyboard.repeat_rate, s->cfg.keyboard.repeat_delay);

  kb->modifiers.notify = keyboard_handle_modifiers;
  wl_signal_add(&wlr_kb->events.modifiers, &kb->modifiers);
  kb->key.notify = keyboard_handle_key;
  wl_signal_add(&wlr_kb->events.key, &kb->key);
  kb->destroy.notify = keyboard_handle_destroy;
  wl_signal_add(&dev->events.destroy, &kb->destroy);

  wlr_seat_set_keyboard(s->seat, wlr_kb);
  wl_list_insert(&s->keyboards, &kb->link);
}

static void server_new_pointer(TrixieServer *s, struct wlr_input_device *dev) {
  wlr_cursor_attach_input_device(s->cursor, dev);
}

/* ── Input handler ────────────────────────────────────────────────────────── */

static void handle_new_input(struct wl_listener *listener, void *data) {
  TrixieServer            *s   = CONTAINER_OF(listener, TrixieServer, new_input);
  struct wlr_input_device *dev = data;
  switch(dev->type) {
    case WLR_INPUT_DEVICE_KEYBOARD: server_new_keyboard(s, dev); break;
    case WLR_INPUT_DEVICE_POINTER: server_new_pointer(s, dev); break;
    default: break;
  }
  uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
  if(!wl_list_empty(&s->keyboards)) caps |= WL_SEAT_CAPABILITY_KEYBOARD;
  wlr_seat_set_capabilities(s->seat, caps);
}

/* ── Cursor ───────────────────────────────────────────────────────────────── */

static PaneId pane_at_cursor(TrixieServer *s, double lx, double ly) {
  uint32_t   px = (uint32_t)lx, py = (uint32_t)ly;
  Workspace *ws = &s->twm.workspaces[s->twm.active_ws];
  int        bw = s->twm.border_w;

  for(int i = ws->pane_count - 1; i >= 0; i--) {
    Pane *p = twm_pane_by_id(&s->twm, ws->panes[i]);
    if(!p || !p->floating) continue;
    if(rect_contains(p->rect, px, py)) return p->id;
  }
  for(int i = ws->pane_count - 1; i >= 0; i--) {
    Pane *p = twm_pane_by_id(&s->twm, ws->panes[i]);
    if(!p || p->floating) continue;
    Rect inner = (p->fullscreen || bw == 0) ? p->rect : rect_inset(p->rect, bw);
    if(rect_contains(inner, px, py)) return p->id;
  }
  return 0;
}

static void update_cursor_focus(TrixieServer *s, uint32_t time) {
  double              cx = s->cursor->x, cy = s->cursor->y;
  struct wlr_surface *surface = NULL;
  double              sx = 0, sy = 0;

  PaneId pid = pane_at_cursor(s, cx, cy);
  if(pid) {
    TrixieView *v = view_from_pane(s, pid);
    if(v && v->mapped) {
      struct wlr_scene_node *node =
          wlr_scene_node_at(&s->scene->tree.node, cx, cy, &sx, &sy);
      if(node && node->type == WLR_SCENE_NODE_BUFFER) {
        struct wlr_scene_buffer  *sb = wlr_scene_buffer_from_node(node);
        struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(sb);
        if(ss) surface = ss->surface;
      }
    }
  }

  if(!surface) {
    wlr_cursor_set_xcursor(s->cursor, s->xcursor_mgr, "default");
    wlr_seat_pointer_notify_clear_focus(s->seat);
  } else {
    wlr_seat_pointer_notify_enter(s->seat, surface, sx, sy);
    wlr_seat_pointer_notify_motion(s->seat, time, sx, sy);
  }
}

static void handle_cursor_motion(struct wl_listener *listener, void *data) {
  TrixieServer *s = CONTAINER_OF(listener, TrixieServer, cursor_motion);
  struct wlr_pointer_motion_event *ev = data;
  wlr_cursor_move(s->cursor, &ev->pointer->base, ev->delta_x, ev->delta_y);

  if(s->drag_mode == DRAG_MOVE) {
    twm_float_move(&s->twm, s->drag_pane, (int)ev->delta_x, (int)ev->delta_y);
    server_sync_windows(s);
    server_request_redraw(s);
    return;
  }
  if(s->drag_mode == DRAG_RESIZE) {
    twm_float_resize(&s->twm, s->drag_pane, (int)ev->delta_x, (int)ev->delta_y);
    server_sync_windows(s);
    server_request_redraw(s);
    return;
  }
  update_cursor_focus(s, ev->time_msec);
}

static void handle_cursor_motion_abs(struct wl_listener *listener, void *data) {
  TrixieServer *s = CONTAINER_OF(listener, TrixieServer, cursor_motion_abs);
  struct wlr_pointer_motion_absolute_event *ev = data;
  wlr_cursor_warp_absolute(s->cursor, &ev->pointer->base, ev->x, ev->y);
  update_cursor_focus(s, ev->time_msec);
}

static void handle_cursor_button(struct wl_listener *listener, void *data) {
  TrixieServer *s = CONTAINER_OF(listener, TrixieServer, cursor_button);
  struct wlr_pointer_button_event *ev = data;
  wlr_seat_pointer_notify_button(s->seat, ev->time_msec, ev->button, ev->state);

  if(ev->state == WL_POINTER_BUTTON_STATE_RELEASED) {
    s->drag_mode = DRAG_NONE;
    return;
  }

  double cx = s->cursor->x, cy = s->cursor->y;
  PaneId pid = pane_at_cursor(s, cx, cy);

  if(pid) {
    twm_set_focused(&s->twm, pid);
    server_focus_pane(s, pid);

    bool            super_held = false;
    TrixieKeyboard *kb;
    wl_list_for_each(kb, &s->keyboards, link) {
      if(wlr_keyboard_get_modifiers(kb->wlr_keyboard) & WLR_MODIFIER_LOGO) {
        super_held = true;
        break;
      }
    }

    Pane *p        = twm_pane_by_id(&s->twm, pid);
    bool  is_float = p && p->floating;

    if(super_held && ev->button == BTN_LEFT && is_float) {
      s->drag_mode = DRAG_MOVE;
      s->drag_pane = pid;
      return;
    }
    if(super_held && ev->button == BTN_RIGHT && is_float) {
      s->drag_mode = DRAG_RESIZE;
      s->drag_pane = pid;
      return;
    }
  }
}

static void handle_cursor_axis(struct wl_listener *listener, void *data) {
  TrixieServer *s = CONTAINER_OF(listener, TrixieServer, cursor_axis);
  struct wlr_pointer_axis_event *ev = data;
  wlr_seat_pointer_notify_axis(s->seat,
                               ev->time_msec,
                               ev->orientation,
                               ev->delta,
                               ev->delta_discrete,
                               ev->source,
                               ev->relative_direction);
}

static void handle_cursor_frame(struct wl_listener *listener, void *data) {
  TrixieServer *s = CONTAINER_OF(listener, TrixieServer, cursor_frame);
  wlr_seat_pointer_notify_frame(s->seat);
}

static void handle_request_cursor(struct wl_listener *listener, void *data) {
  TrixieServer *s = CONTAINER_OF(listener, TrixieServer, seat_request_cursor);
  struct wlr_seat_pointer_request_set_cursor_event *ev = data;
  struct wlr_seat_client *focused = s->seat->pointer_state.focused_client;
  if(focused == ev->seat_client)
    wlr_cursor_set_surface(s->cursor, ev->surface, ev->hotspot_x, ev->hotspot_y);
}

static void handle_request_set_selection(struct wl_listener *listener, void *data) {
  TrixieServer *s = CONTAINER_OF(listener, TrixieServer, seat_request_set_selection);
  struct wlr_seat_request_set_selection_event *ev = data;
  wlr_seat_set_selection(s->seat, ev->source, ev->serial);
}

static void handle_request_set_primary_selection(struct wl_listener *listener,
                                                 void               *data) {
  TrixieServer *s =
      CONTAINER_OF(listener, TrixieServer, seat_request_set_primary_selection);
  struct wlr_seat_request_set_primary_selection_event *ev = data;
  wlr_seat_set_primary_selection(s->seat, ev->source, ev->serial);
}

/* ── Output ───────────────────────────────────────────────────────────────── */

static void output_handle_frame(struct wl_listener *listener, void *data) {
  TrixieOutput *o = CONTAINER_OF(listener, TrixieOutput, frame);
  TrixieServer *s = o->server;

  bool still_animating = anim_tick(&s->anim);
  if(still_animating) server_sync_windows(s);

  if(o->bar) bar_update(o->bar, &s->twm, &s->cfg);
  if(o->deco) deco_update(o->deco, &s->twm, &s->anim, &s->cfg);

  struct wlr_scene_output *scene_output = o->scene_output;
  wlr_scene_output_commit(scene_output, NULL);

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  wlr_scene_output_send_frame_done(scene_output, &now);

  if(still_animating) wlr_output_schedule_frame(o->wlr_output);
}

static void output_handle_request_state(struct wl_listener *listener, void *data) {
  TrixieOutput *o = CONTAINER_OF(listener, TrixieOutput, request_state);
  struct wlr_output_event_request_state *ev = data;
  wlr_output_commit_state(o->wlr_output, ev->state);
}

static void output_handle_destroy(struct wl_listener *listener, void *data) {
  TrixieOutput *o = CONTAINER_OF(listener, TrixieOutput, destroy);
  wl_list_remove(&o->frame.link);
  wl_list_remove(&o->request_state.link);
  wl_list_remove(&o->destroy.link);
  wl_list_remove(&o->link);
  if(o->bar) bar_destroy(o->bar);
  if(o->deco) deco_destroy(o->deco);
  free(o);
}

static void handle_new_output(struct wl_listener *listener, void *data) {
  TrixieServer      *s          = CONTAINER_OF(listener, TrixieServer, new_output);
  struct wlr_output *wlr_output = data;

  wlr_output_init_render(wlr_output, s->allocator, s->renderer);

  struct wlr_output_state state;
  wlr_output_state_init(&state);
  wlr_output_state_set_enabled(&state, true);

  struct wlr_output_mode *mode = NULL;
  MonitorCfg             *mcfg = NULL;
  for(int i = 0; i < s->cfg.monitor_count; i++) {
    if(!strcmp(s->cfg.monitors[i].name, wlr_output->name)) {
      mcfg = &s->cfg.monitors[i];
      break;
    }
  }
  if(mcfg) {
    struct wlr_output_mode *m;
    wl_list_for_each(m, &wlr_output->modes, link) {
      if(m->width == mcfg->width && m->height == mcfg->height &&
         m->refresh == mcfg->refresh * 1000) {
        mode = m;
        break;
      }
    }
  }
  if(!mode) mode = wlr_output_preferred_mode(wlr_output);
  if(mode) wlr_output_state_set_mode(&state, mode);

  wlr_output_commit_state(wlr_output, &state);
  wlr_output_state_finish(&state);

  TrixieOutput *o = calloc(1, sizeof(*o));
  o->server       = s;
  o->wlr_output   = wlr_output;

  o->frame.notify = output_handle_frame;
  wl_signal_add(&wlr_output->events.frame, &o->frame);
  o->request_state.notify = output_handle_request_state;
  wl_signal_add(&wlr_output->events.request_state, &o->request_state);
  o->destroy.notify = output_handle_destroy;
  wl_signal_add(&wlr_output->events.destroy, &o->destroy);

  wl_list_insert(&s->outputs, &o->link);

  struct wlr_output_layout_output *layout_output =
      wlr_output_layout_add_auto(s->output_layout, wlr_output);
  o->scene_output = wlr_scene_output_create(s->scene, wlr_output);
  wlr_scene_output_layout_add_output(
      s->scene_layout, layout_output, o->scene_output);

  int ow = wlr_output->width, oh = wlr_output->height;
  if(mode) {
    ow = mode->width;
    oh = mode->height;
  }
  o->width  = ow;
  o->height = oh;

  if(wl_list_length(&s->outputs) == 1) {
    twm_resize(&s->twm, ow, oh);
    anim_set_resize(&s->anim, ow, oh);
  }

  /* bar and deco attach to the overlay layer */
  o->bar  = bar_create(s->layer_overlay, ow, oh, &s->cfg);
  o->deco = deco_create(s->layer_overlay);

  wlr_log(WLR_INFO, "New output: %s %dx%d", wlr_output->name, ow, oh);
}

/* ── XDG shell ────────────────────────────────────────────────────────────── */

static void view_handle_map(struct wl_listener *listener, void *data) {
  TrixieView   *v = CONTAINER_OF(listener, TrixieView, map);
  TrixieServer *s = v->server;
  v->mapped       = true;

  Pane *p = twm_pane_by_id(&s->twm, v->pane_id);
  if(!p) {
    wlr_log(WLR_ERROR, "MAP: pane not found for id=%u!", v->pane_id);
    return;
  }

  /* win rules */
  const char *app_id      = v->xdg_toplevel->app_id ? v->xdg_toplevel->app_id : "";
  bool        force_float = false, force_fs = false;
  for(int i = 0; i < s->cfg.win_rule_count; i++) {
    WinRule *r = &s->cfg.win_rules[i];
    if(!strcmp(r->app_id, app_id) || strstr(app_id, r->app_id)) {
      if(r->float_rule) force_float = true;
      if(r->fullscreen_rule) force_fs = true;
    }
  }
  if(force_float) {
    p->floating = true;
    if(rect_empty(p->float_rect)) p->float_rect = p->rect;
  }
  if(force_fs) p->fullscreen = true;

  twm_reflow(&s->twm);
  wlr_log(WLR_INFO,
          "MAP DIAG: pane=%u rect=(%d,%d,%d,%d) floating=%d fs=%d",
          v->pane_id,
          p->rect.x,
          p->rect.y,
          p->rect.w,
          p->rect.h,
          p->floating,
          p->fullscreen);
  wlr_log(WLR_INFO,
          "MAP DIAG: scene_tree=%p node.enabled=%d node.parent=%p",
          (void *)v->scene_tree,
          v->scene_tree->node.enabled,
          (void *)v->scene_tree->node.parent);
  wlr_log(WLR_INFO,
          "MAP DIAG: layer_windows=%p layer_floating=%p layer_overlay=%p",
          (void *)s->layer_windows,
          (void *)s->layer_floating,
          (void *)s->layer_overlay);
  struct wlr_scene_tree *tgt =
      (p->floating || p->fullscreen) ? s->layer_floating : s->layer_windows;
  wlr_scene_node_reparent(&v->scene_tree->node, tgt);
  wlr_scene_node_raise_to_top(&v->scene_tree->node);
  wlr_log(WLR_INFO,
          "MAP DIAG: after reparent node.parent=%p (expected tgt=%p)",
          (void *)v->scene_tree->node.parent,
          (void *)tgt);
  /* Send the configure FIRST and record that we've done so. The client
   * must ack before it will render at the right size. We do NOT call
   * server_sync_windows yet — that will happen on the next output frame
   * once the client has acked and committed. */
  int bw = (p->floating || p->fullscreen) ? 0 : s->twm.border_w;
  int cw = p->rect.w - bw * 2;
  int ch = p->rect.h - bw * 2;
  if(cw < 1) cw = 1;
  if(ch < 1) ch = 1;

  wlr_xdg_toplevel_set_activated(v->xdg_toplevel, true);
  wlr_xdg_toplevel_set_size(v->xdg_toplevel, cw, ch);

  /* Position the scene node immediately so the window is visible even
   * before the client acks the configure. wlr_scene uses the node
   * position independently of the surface size. */
  int win_x = p->rect.x + bw;
  int win_y = p->rect.y + bw;
  wlr_scene_node_set_position(&v->scene_tree->node, win_x, win_y);
  wlr_scene_node_set_enabled(&v->scene_tree->node, true);

  bool is_scratch = false;
  for(int i = 0; i < s->twm.scratch_count; i++)
    if(s->twm.scratchpads[i].has_pane &&
       s->twm.scratchpads[i].pane_id == v->pane_id) {
      is_scratch = true;
      break;
    }

  if(is_scratch)
    anim_scratch_open(&s->anim, v->pane_id, p->rect);
  else if(p->floating)
    anim_float_open(&s->anim, v->pane_id, p->rect);
  else
    anim_open(&s->anim, v->pane_id, p->rect);

  server_focus_pane(s, v->pane_id);
  /* sync_windows will reposition correctly on the next frame tick;
   * request a redraw to kick the frame loop. */
  server_request_redraw(s);
}

static void view_handle_unmap(struct wl_listener *listener, void *data) {
  TrixieView   *v = CONTAINER_OF(listener, TrixieView, unmap);
  TrixieServer *s = v->server;
  v->mapped       = false;

  Pane *p = twm_pane_by_id(&s->twm, v->pane_id);
  if(p) {
    bool is_scratch = false;
    for(int i = 0; i < s->twm.scratch_count; i++)
      if(s->twm.scratchpads[i].has_pane &&
         s->twm.scratchpads[i].pane_id == v->pane_id) {
        is_scratch = true;
        break;
      }
    if(is_scratch)
      anim_scratch_close(&s->anim, v->pane_id, p->rect);
    else if(p->floating)
      anim_float_close(&s->anim, v->pane_id, p->rect);
    else
      anim_close(&s->anim, v->pane_id, p->rect);
  }

  if(s->seat->keyboard_state.focused_surface) {
    TrixieView *next;
    wl_list_for_each(next, &s->views, link) {
      if(next != v && next->mapped) {
        server_focus_pane(s, next->pane_id);
        break;
      }
    }
  }
}

static void view_handle_destroy(struct wl_listener *listener, void *data) {
  TrixieView   *v = CONTAINER_OF(listener, TrixieView, destroy);
  TrixieServer *s = v->server;

  twm_close(&s->twm, v->pane_id);

  wl_list_remove(&v->map.link);
  wl_list_remove(&v->unmap.link);
  wl_list_remove(&v->destroy.link);
  wl_list_remove(&v->commit.link);
  wl_list_remove(&v->request_fullscreen.link);
  wl_list_remove(&v->set_app_id.link);
  wl_list_remove(&v->set_title.link);
  wl_list_remove(&v->link);
  free(v);

  server_request_redraw(s);
}

static void view_handle_commit(struct wl_listener *listener, void *data) {
  TrixieView *v = CONTAINER_OF(listener, TrixieView, commit);
  server_request_redraw(v->server);
}

static void view_handle_request_fullscreen(struct wl_listener *listener,
                                           void               *data) {
  TrixieView *v = CONTAINER_OF(listener, TrixieView, request_fullscreen);
  Action      a = { .kind = ACTION_FULLSCREEN };
  server_dispatch_action(v->server, &a);
}

static void view_handle_set_title(struct wl_listener *listener, void *data) {
  TrixieView *v = CONTAINER_OF(listener, TrixieView, set_title);
  if(v->xdg_toplevel->title)
    twm_set_title(&v->server->twm, v->pane_id, v->xdg_toplevel->title);
}

static void view_handle_set_app_id(struct wl_listener *listener, void *data) {
  TrixieView *v = CONTAINER_OF(listener, TrixieView, set_app_id);
  if(v->xdg_toplevel->app_id)
    twm_try_assign_scratch(&v->server->twm, v->pane_id, v->xdg_toplevel->app_id);
}

static void handle_new_xdg_surface(struct wl_listener *listener, void *data) {
  TrixieServer            *s = CONTAINER_OF(listener, TrixieServer, new_xdg_surface);
  struct wlr_xdg_toplevel *toplevel = data;
  struct wlr_xdg_surface  *surface  = toplevel->base;

  const char *app_id = toplevel->app_id ? toplevel->app_id : "";
  wlr_log(WLR_INFO, "XDG_TOPLEVEL: new toplevel app_id='%s'", app_id);

  PaneId pane_id = twm_open(&s->twm, app_id);
  wlr_log(WLR_INFO, "XDG_TOPLEVEL: assigned pane_id=%u", pane_id);

  wlr_xdg_toplevel_set_activated(toplevel, false);
  wlr_xdg_toplevel_set_tiled(
      toplevel, WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);

  TrixieView *v   = calloc(1, sizeof(*v));
  v->server       = s;
  v->xdg_toplevel = toplevel;
  v->pane_id      = pane_id;
  v->mapped       = false;

  v->scene_tree = wlr_scene_xdg_surface_create(s->layer_windows, surface);
  wlr_scene_node_set_enabled(&v->scene_tree->node, true);

  v->map.notify                = view_handle_map;
  v->unmap.notify              = view_handle_unmap;
  v->destroy.notify            = view_handle_destroy;
  v->commit.notify             = view_handle_commit;
  v->request_fullscreen.notify = view_handle_request_fullscreen;
  v->set_title.notify          = view_handle_set_title;
  v->set_app_id.notify         = view_handle_set_app_id;

  wl_signal_add(&toplevel->base->surface->events.map, &v->map);
  wl_signal_add(&toplevel->base->surface->events.unmap, &v->unmap);
  wl_signal_add(&surface->events.destroy, &v->destroy);
  wl_signal_add(&surface->surface->events.commit, &v->commit);
  wl_signal_add(&toplevel->events.request_fullscreen, &v->request_fullscreen);
  wl_signal_add(&toplevel->events.set_title, &v->set_title);
  wl_signal_add(&toplevel->events.set_app_id, &v->set_app_id);

  wl_list_insert(&s->views, &v->link);
}

static void handle_new_layer_surface(struct wl_listener *listener, void *data) {
  TrixieServer *s = CONTAINER_OF(listener, TrixieServer, new_layer_surface);
  struct wlr_layer_surface_v1 *layer = data;
  wlr_log(WLR_DEBUG, "New layer surface: namespace=%s", layer->namespace);
  wlr_scene_layer_surface_v1_create(s->layer_overlay, layer);
}

static void handle_new_deco(struct wl_listener *listener, void *data) {
  struct wlr_xdg_toplevel_decoration_v1 *deco = data;
  wlr_xdg_toplevel_decoration_v1_set_mode(
      deco, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

/* ── IPC ──────────────────────────────────────────────────────────────────── */
#include <sys/socket.h>
#include <sys/un.h>

static const char *ipc_socket_path(void) {
  static char buf[256];
  if(buf[0]) return buf;
  const char *xdg = getenv("XDG_RUNTIME_DIR");
  if(xdg)
    snprintf(buf, sizeof(buf), "%s/trixie.sock", xdg);
  else
    snprintf(buf, sizeof(buf), "/tmp/trixie-%d.sock", (int)getuid());
  return buf;
}

static int ipc_read_cb(int fd, uint32_t mask, void *data) {
  TrixieServer *s      = data;
  int           client = accept(fd, NULL, NULL);
  if(client < 0) return 0;
  fcntl(client, F_SETFL, O_NONBLOCK);

  char    buf[1024] = { 0 };
  ssize_t n         = read(client, buf, sizeof(buf) - 1);
  if(n > 0) {
    char *nl = strchr(buf, '\n');
    if(nl) *nl = '\0';
    char reply[4096] = { 0 };
    ipc_dispatch(s, buf, reply, sizeof(reply));
    strncat(reply, "\n", sizeof(reply) - strlen(reply) - 1);
    write(client, reply, strlen(reply));
  }
  close(client);
  return 0;
}

void server_init_ipc(TrixieServer *s) {
  unlink(ipc_socket_path());
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if(fd < 0) return;
  struct sockaddr_un addr = { 0 };
  addr.sun_family         = AF_UNIX;
  strncpy(addr.sun_path, ipc_socket_path(), sizeof(addr.sun_path) - 1);
  if(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    return;
  }
  listen(fd, 8);
  fcntl(fd, F_SETFL, O_NONBLOCK);
  s->ipc_fd  = fd;
  s->ipc_src = wl_event_loop_add_fd(
      wl_display_get_event_loop(s->display), fd, WL_EVENT_READABLE, ipc_read_cb, s);
  wlr_log(WLR_INFO, "IPC: %s", ipc_socket_path());
}

/* ── Config file watcher ──────────────────────────────────────────────────── */
#include <sys/inotify.h>

static int inotify_cb(int fd, uint32_t mask, void *data) {
  TrixieServer *s = data;
  char    buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
  ssize_t n = read(fd, buf, sizeof(buf));
  if(n <= 0) return 0;
  server_apply_config_reload(s);
  return 0;
}

void server_init_config_watch(TrixieServer *s) {
  const char *xdg = getenv("XDG_CONFIG_HOME");
  char        dir[256];
  if(xdg)
    snprintf(dir, sizeof(dir), "%s/trixie", xdg);
  else {
    const char *home = getenv("HOME");
    if(!home) home = "/root";
    snprintf(dir, sizeof(dir), "%s/.config/trixie", home);
  }
  s->inotify_fd = inotify_init1(IN_NONBLOCK);
  if(s->inotify_fd < 0) return;
  if(inotify_add_watch(s->inotify_fd, dir, IN_MODIFY | IN_CREATE) < 0) {
    close(s->inotify_fd);
    s->inotify_fd = -1;
    return;
  }
  s->inotify_src = wl_event_loop_add_fd(wl_display_get_event_loop(s->display),
                                        s->inotify_fd,
                                        WL_EVENT_READABLE,
                                        inotify_cb,
                                        s);
  wlr_log(WLR_INFO, "Watching %s for config changes", dir);
}

/* ── Main ─────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
  wlr_log_init(WLR_DEBUG, NULL);

  TrixieServer *s = calloc(1, sizeof(*s));
  wl_list_init(&s->views);
  wl_list_init(&s->outputs);
  wl_list_init(&s->keyboards);
  s->running = true;

  /* Load config */
  {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    char        path[256];
    if(xdg)
      snprintf(path, sizeof(path), "%s/trixie/trixie.conf", xdg);
    else {
      const char *home = getenv("HOME");
      if(!home) home = "/root";
      snprintf(path, sizeof(path), "%s/.config/trixie/trixie.conf", home);
    }
    config_load(&s->cfg, path);
  }

  /* TWM */
  twm_init(&s->twm,
           1920,
           1080,
           s->cfg.bar.height,
           s->cfg.bar.position == BAR_BOTTOM,
           s->cfg.gap,
           s->cfg.border_width,
           0,
           s->cfg.workspaces,
           s->cfg.smart_gaps);
  for(int i = 0; i < s->cfg.scratchpad_count; i++) {
    ScratchpadCfg *sp = &s->cfg.scratchpads[i];
    twm_register_scratch(
        &s->twm, sp->name, sp->app_id, sp->width_pct, sp->height_pct);
  }
  anim_set_resize(&s->anim, 1920, 1080);

  /* Wayland display */
  s->display = wl_display_create();
  s->backend = wlr_backend_autocreate(wl_display_get_event_loop(s->display), NULL);
  if(!s->backend) {
    wlr_log(WLR_ERROR, "Failed to create backend");
    return 1;
  }

  s->renderer = wlr_renderer_autocreate(s->backend);
  wlr_renderer_init_wl_display(s->renderer, s->display);
  s->allocator = wlr_allocator_autocreate(s->backend, s->renderer);

  wlr_compositor_create(s->display, 5, s->renderer);
  wlr_subcompositor_create(s->display);
  wlr_data_device_manager_create(s->display);

  /* Output layout + scene */
  s->output_layout = wlr_output_layout_create(s->display);
  s->scene         = wlr_scene_create();
  s->scene_layout  = wlr_scene_attach_output_layout(s->scene, s->output_layout);

  /* Z-ordered scene layers — order of creation = bottom to top */
  s->layer_background = wlr_scene_tree_create(&s->scene->tree);
  s->layer_windows    = wlr_scene_tree_create(&s->scene->tree);
  s->layer_floating   = wlr_scene_tree_create(&s->scene->tree);
  s->layer_overlay    = wlr_scene_tree_create(&s->scene->tree);

  /* Shells */
  s->xdg_shell              = wlr_xdg_shell_create(s->display, 6);
  s->new_xdg_surface.notify = handle_new_xdg_surface;
  wl_signal_add(&s->xdg_shell->events.new_toplevel, &s->new_xdg_surface);

  s->layer_shell              = wlr_layer_shell_v1_create(s->display, 4);
  s->new_layer_surface.notify = handle_new_layer_surface;
  wl_signal_add(&s->layer_shell->events.new_surface, &s->new_layer_surface);

  s->output_mgr = wlr_xdg_output_manager_v1_create(s->display, s->output_layout);

  /* Decorations */
  s->deco_mgr        = wlr_xdg_decoration_manager_v1_create(s->display);
  s->new_deco.notify = handle_new_deco;
  wl_signal_add(&s->deco_mgr->events.new_toplevel_decoration, &s->new_deco);
  s->srv_deco = wlr_server_decoration_manager_create(s->display);
  wlr_server_decoration_manager_set_default_mode(
      s->srv_deco, WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);

  /* Cursor */
  s->cursor = wlr_cursor_create();
  wlr_cursor_attach_output_layout(s->cursor, s->output_layout);
  s->xcursor_mgr = wlr_xcursor_manager_create(
      s->cfg.cursor_theme[0] ? s->cfg.cursor_theme : NULL, s->cfg.cursor_size);
  wlr_xcursor_manager_load(s->xcursor_mgr, s->cfg.cursor_size);

  s->cursor_motion.notify     = handle_cursor_motion;
  s->cursor_motion_abs.notify = handle_cursor_motion_abs;
  s->cursor_button.notify     = handle_cursor_button;
  s->cursor_axis.notify       = handle_cursor_axis;
  s->cursor_frame.notify      = handle_cursor_frame;
  wl_signal_add(&s->cursor->events.motion, &s->cursor_motion);
  wl_signal_add(&s->cursor->events.motion_absolute, &s->cursor_motion_abs);
  wl_signal_add(&s->cursor->events.button, &s->cursor_button);
  wl_signal_add(&s->cursor->events.axis, &s->cursor_axis);
  wl_signal_add(&s->cursor->events.frame, &s->cursor_frame);

  /* Seat */
  s->seat                       = wlr_seat_create(s->display, s->cfg.seat_name);
  s->seat_request_cursor.notify = handle_request_cursor;
  s->seat_request_set_selection.notify = handle_request_set_selection;
  s->seat_request_set_primary_selection.notify =
      handle_request_set_primary_selection;
  wl_signal_add(&s->seat->events.request_set_cursor, &s->seat_request_cursor);
  wl_signal_add(&s->seat->events.request_set_selection,
                &s->seat_request_set_selection);
  wl_signal_add(&s->seat->events.request_set_primary_selection,
                &s->seat_request_set_primary_selection);

  /* Backend listeners */
  s->new_output.notify = handle_new_output;
  wl_signal_add(&s->backend->events.new_output, &s->new_output);
  s->new_input.notify = handle_new_input;
  wl_signal_add(&s->backend->events.new_input, &s->new_input);

  /* Wayland socket */
  const char *socket = wl_display_add_socket_auto(s->display);
  if(!socket) {
    wlr_log(WLR_ERROR, "Failed to create Wayland socket");
    return 1;
  }

  if(!wlr_backend_start(s->backend)) {
    wlr_log(WLR_ERROR, "Failed to start backend");
    return 1;
  }

  setenv("WAYLAND_DISPLAY", socket, true);
  setenv("XDG_SESSION_TYPE", "wayland", true);
  wlr_log(WLR_INFO, "WAYLAND_DISPLAY=%s", socket);

  server_init_ipc(s);
  server_init_config_watch(s);

  if(!s->exec_once_done) {
    s->exec_once_done = true;
    for(int i = 0; i < s->cfg.exec_once_count; i++)
      server_spawn(s, s->cfg.exec_once[i]);
  }
  for(int i = 0; i < s->cfg.exec_count; i++)
    server_spawn(s, s->cfg.exec[i]);

  wl_display_run(s->display);

  /* Cleanup */
  wlr_log(WLR_INFO, "Trixie shutting down");
  if(s->ipc_src) wl_event_source_remove(s->ipc_src);
  if(s->inotify_src) wl_event_source_remove(s->inotify_src);
  if(s->ipc_fd >= 0) {
    close(s->ipc_fd);
    unlink(ipc_socket_path());
  }
  if(s->inotify_fd >= 0) close(s->inotify_fd);

  wl_display_destroy_clients(s->display);
  wlr_backend_destroy(s->backend);
  wlr_xcursor_manager_destroy(s->xcursor_mgr);
  wlr_cursor_destroy(s->cursor);
  wlr_output_layout_destroy(s->output_layout);
  wl_display_destroy(s->display);
  free(s);
  return 0;
}
