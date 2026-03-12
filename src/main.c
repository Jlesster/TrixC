/* main.c — Trixie compositor entry point, wlroots backend & event loop
 */
#include "build/wlr-layer-shell-unstable-v1-protocol.h"
#include "nvim_panel.h"
#include "trixie.h"
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <libinput.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wlr/backend/libinput.h>
#include <wlr/render/pass.h>
#include <EGL/egl.h>
#include <wlr/types/wlr_pointer_gestures_v1.h>
#include <wlr/types/wlr_xdg_shell.h>


#include <execinfo.h>
#include <signal.h>


#include <wlr/types/wlr_content_type_v1.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_tearing_control_v1.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_activation_v1.h>

/* ── Forward declarations ─────────────────────────────────────────────────── */

static void handle_new_output(struct wl_listener *l, void *data);
static void handle_new_input(struct wl_listener *l, void *data);
static void handle_new_xdg_surface(struct wl_listener *l, void *data);
static void handle_new_layer_surface(struct wl_listener *l, void *data);
static void handle_new_deco(struct wl_listener *l, void *data);
static void handle_new_idle_inhibitor(struct wl_listener *l, void *data);
static void handle_cursor_motion(struct wl_listener *l, void *data);
static void handle_cursor_motion_abs(struct wl_listener *l, void *data);
static void handle_cursor_button(struct wl_listener *l, void *data);
static void handle_cursor_axis(struct wl_listener *l, void *data);
static void handle_cursor_frame(struct wl_listener *l, void *data);
static void handle_request_cursor(struct wl_listener *l, void *data);
static void handle_request_set_selection(struct wl_listener *l, void *data);
static void handle_request_set_primary_selection(struct wl_listener *l, void *data);
static void handle_request_start_drag(struct wl_listener *l, void *data);
static void handle_start_drag(struct wl_listener *l, void *data);
void server_binary_reload(TrixieServer *s);

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

/* ── xdg-activation ───────────────────────────────────────────────────────── */

static void handle_swipe_begin(struct wl_listener *l, void *data) {
  TrixieServer *s = CONTAINER_OF(l, TrixieServer, swipe_begin);
  struct wlr_pointer_swipe_begin_event *ev = data;
  gesture_swipe_begin(&s->gesture, &s->cfg.gestures, s, (int)ev->fingers);
  if(s->pointer_gestures)
    wlr_pointer_gestures_v1_send_swipe_begin(
        s->pointer_gestures, s->seat, ev->time_msec, ev->fingers);
}

static void handle_swipe_update(struct wl_listener *l, void *data) {
  TrixieServer *s = CONTAINER_OF(l, TrixieServer, swipe_update);
  struct wlr_pointer_swipe_update_event *ev = data;
  gesture_swipe_update(&s->gesture, &s->cfg.gestures, s, ev->dx, ev->dy);
  if(s->pointer_gestures)
    wlr_pointer_gestures_v1_send_swipe_update(/* 5 args — no fingers */
                                              s->pointer_gestures,
                                              s->seat,
                                              ev->time_msec,
                                              ev->dx,
                                              ev->dy);
}

static void handle_swipe_end(struct wl_listener *l, void *data) {
  TrixieServer                       *s  = CONTAINER_OF(l, TrixieServer, swipe_end);
  struct wlr_pointer_swipe_end_event *ev = data;
  gesture_swipe_end(&s->gesture, &s->cfg.gestures, s, ev->cancelled);
  if(s->pointer_gestures)
    wlr_pointer_gestures_v1_send_swipe_end(
        s->pointer_gestures, s->seat, ev->time_msec, ev->cancelled);
}

static void handle_pinch_begin(struct wl_listener *l, void *data) {
  TrixieServer *s = CONTAINER_OF(l, TrixieServer, pinch_begin);
  struct wlr_pointer_pinch_begin_event *ev = data;
  gesture_pinch_begin(&s->gesture, &s->cfg.gestures, s, (int)ev->fingers);
  if(s->pointer_gestures)
    wlr_pointer_gestures_v1_send_pinch_begin(
        s->pointer_gestures, s->seat, ev->time_msec, ev->fingers);
}

static void handle_pinch_update(struct wl_listener *l, void *data) {
  TrixieServer *s = CONTAINER_OF(l, TrixieServer, pinch_update);
  struct wlr_pointer_pinch_update_event *ev = data;
  gesture_pinch_update(&s->gesture, &s->cfg.gestures, s, ev->scale);
  if(s->pointer_gestures)
    wlr_pointer_gestures_v1_send_pinch_update(/* 7 args — no fingers */
                                              s->pointer_gestures,
                                              s->seat,
                                              ev->time_msec,
                                              ev->dx,
                                              ev->dy,
                                              ev->scale,
                                              ev->rotation);
}

static void handle_pinch_end(struct wl_listener *l, void *data) {
  TrixieServer                       *s  = CONTAINER_OF(l, TrixieServer, pinch_end);
  struct wlr_pointer_pinch_end_event *ev = data;
  gesture_pinch_end(&s->gesture, &s->cfg.gestures, s, ev->cancelled);
  if(s->pointer_gestures)
    wlr_pointer_gestures_v1_send_pinch_end(
        s->pointer_gestures, s->seat, ev->time_msec, ev->cancelled);
}

static void handle_xdg_activation(struct wl_listener *listener, void *data) {
  TrixieServer *s = wl_container_of(listener, s, xdg_activation_request);
  struct wlr_xdg_activation_v1_request_activate_event *ev = data;
  bool allow = (ev->token == NULL) || (ev->token->seat == s->seat);
  if(!allow) return;
  TrixieView *v;
  wl_list_for_each(v, &s->views, link) {
    if(!v->mapped) continue;
    struct wlr_surface *surf = NULL;
#ifdef HAS_XWAYLAND
    if(v->is_xwayland && v->xwayland_surface)
      surf = v->xwayland_surface->surface;
    else
#endif
        if(v->xdg_toplevel)
      surf = v->xdg_toplevel->base->surface;
    if(surf == ev->surface) {
      twm_set_focused(&s->twm, v->pane_id);
      server_focus_pane(s, v->pane_id);
      server_request_redraw(s);
      return;
    }
  }
}

/* ── pointer constraints ──────────────────────────────────────────────────── */

static void handle_pointer_constraint(struct wl_listener *listener, void *data) {
  TrixieServer *s = wl_container_of(listener, s, new_pointer_constraint);
  struct wlr_pointer_constraint_v1 *constraint = data;
  wlr_pointer_constraint_v1_send_activated(constraint);
}

/* ── cursor shape (wp-cursor-shape-v1) ───────────────────────────────────── */

static void handle_cursor_shape_request(struct wl_listener *listener, void *data) {
  TrixieServer *s = CONTAINER_OF(listener, TrixieServer, cursor_shape_request);
  struct wlr_cursor_shape_manager_v1_request_set_shape_event *ev = data;
  /* Only honour requests from the currently focused client. */
  if(ev->seat_client != s->seat->pointer_state.focused_client) return;
  const char *shape_name =
      wlr_cursor_shape_v1_name(ev->shape); /* e.g. "text", "grab" */
  if(!shape_name) shape_name = "default";
  wlr_cursor_set_xcursor(s->cursor, s->xcursor_mgr, shape_name);
}

/* ── foreign toplevel ─────────────────────────────────────────────────────── */

static void view_foreign_toplevel_update(TrixieServer *s, TrixieView *v) {
  if(!s->foreign_toplevel_mgr || !v->foreign_handle) return;
  Pane *p = twm_pane_by_id(&s->twm, v->pane_id);
  if(!p) return;
  wlr_foreign_toplevel_handle_v1_set_title(v->foreign_handle, p->title);
  wlr_foreign_toplevel_handle_v1_set_app_id(v->foreign_handle, p->app_id);
  wlr_foreign_toplevel_handle_v1_set_activated(
      v->foreign_handle, twm_focused_id(&s->twm) == v->pane_id);
  wlr_foreign_toplevel_handle_v1_set_fullscreen(v->foreign_handle, p->fullscreen);
  wlr_foreign_toplevel_handle_v1_set_maximized(v->foreign_handle, false);
  wlr_foreign_toplevel_handle_v1_set_minimized(v->foreign_handle, false);
}

static void handle_foreign_toplevel_request_activate(struct wl_listener *listener,
                                                     void               *data) {
  (void)data;
  TrixieView   *v = wl_container_of(listener, v, foreign_request_activate);
  TrixieServer *s = v->server;
  twm_set_focused(&s->twm, v->pane_id);
  server_focus_pane(s, v->pane_id);
  server_request_redraw(s);
}

static void handle_foreign_toplevel_request_close(struct wl_listener *listener,
                                                  void               *data) {
  (void)data;
  TrixieView *v = wl_container_of(listener, v, foreign_request_close);
#ifdef HAS_XWAYLAND
  if(v->is_xwayland && v->xwayland_surface) {
    wlr_xwayland_surface_close(v->xwayland_surface);
    return;
  }
#endif
  if(v->xdg_toplevel) wlr_xdg_toplevel_send_close(v->xdg_toplevel);
}

/* ── Idle / DPMS ──────────────────────────────────────────────────────────── */

static int idle_timer_cb(void *data) {
  TrixieServer *s = data;
  /* Do not blank if any inhibitor is active (e.g. video player). */
  if(s->idle_inhibit_count > 0) return 0;
  wlr_log(WLR_INFO, "idle: blanking outputs");
  TrixieOutput *o;
  wl_list_for_each(o, &s->outputs, link) {
    struct wlr_output_state st;
    wlr_output_state_init(&st);
    wlr_output_state_set_enabled(&st, false);
    wlr_output_commit_state(o->wlr_output, &st);
    wlr_output_state_finish(&st);
  }
  return 0;
}

/* ── Idle-inhibit handlers ────────────────────────────────────────────────── */

typedef struct {
  struct wl_listener destroy;
  TrixieServer      *server;
} IdleInhibitorCtx;

static void handle_idle_inhibitor_destroy(struct wl_listener *listener, void *data) {
  (void)data;
  IdleInhibitorCtx *ctx = wl_container_of(listener, ctx, destroy);
  TrixieServer     *s   = ctx->server;
  if(s->idle_inhibit_count > 0) s->idle_inhibit_count--;
  if(s->idle_inhibit_count == 0) {
    /* Resume the idle timer now that all inhibitors are gone. */
    wlr_log(WLR_INFO, "idle-inhibit: all inhibitors released, resuming idle timer");
    if(s->idle_timer && s->idle_timeout_ms > 0)
      wl_event_source_timer_update(s->idle_timer, s->idle_timeout_ms);
  }
  wl_list_remove(&ctx->destroy.link);
  free(ctx);
}

static void handle_new_idle_inhibitor(struct wl_listener *listener, void *data) {
  TrixieServer                     *s   = CONTAINER_OF(listener, TrixieServer, idle_inhibit_new);
  struct wlr_idle_inhibitor_v1     *inh = data;
  s->idle_inhibit_count++;
  wlr_log(WLR_INFO, "idle-inhibit: inhibitor added (count=%d)", s->idle_inhibit_count);
  /* While any inhibitor is active, pause the idle timer. */
  if(s->idle_timer) wl_event_source_timer_update(s->idle_timer, 0);
  IdleInhibitorCtx *ctx = calloc(1, sizeof(*ctx));
  if(ctx) {
    ctx->server          = s;
    ctx->destroy.notify  = handle_idle_inhibitor_destroy;
    wl_signal_add(&inh->events.destroy, &ctx->destroy);
  }
}

void server_reset_idle(TrixieServer *s) {
  if(!s->idle_timer) return;
  TrixieOutput *o;
  wl_list_for_each(o, &s->outputs, link) {
    struct wlr_output_state st;
    wlr_output_state_init(&st);
    wlr_output_state_set_enabled(&st, true);
    wlr_output_commit_state(o->wlr_output, &st);
    wlr_output_state_finish(&st);
  }
  wl_event_source_timer_update(s->idle_timer, s->idle_timeout_ms);
}

/* ── Pane geometry helpers ────────────────────────────────────────────────── */

static int pane_title_extra(TrixieServer *s, Pane *p) {
  if(p->floating || p->fullscreen || s->twm.border_w < 4) return 0;
  for(int i = 0; i < s->cfg.win_rule_count; i++) {
    WinRule *r = &s->cfg.win_rules[i];
    if(r->notitle && r->app_id[0] && strstr(p->app_id, r->app_id)) return 0;
  }
  int bw  = s->twm.border_w;
  int tbh = bw > TITLE_BAR_H ? bw : TITLE_BAR_H;
  return tbh - bw;
}

// near the other static callbacks at the top of main.c
static int  s_nvim_retry_count         = 0;
static bool s_nvim_socket_existed_last = false;

/* DnD drag-icon scene node — updated every cursor motion so the icon follows
 * the cursor.  NULL when no drag is active.  File-scope so it requires no
 * change to TrixieServer's struct layout in trixie.h. */
static struct wlr_scene_tree *g_drag_icon_tree = NULL;

void nvim_retry_reset(void) {
  s_nvim_retry_count         = 0;
  s_nvim_socket_existed_last = false;
}

static int nvim_connect_timer_cb(void *data) {
  TrixieServer *s       = data;
  int           next_ms = 2000;

  if(!nvim_is_connected() && s->cfg.overlay.nvim_socket[0]) {
    /* Check if socket file exists */
    struct stat st;
    bool        exists = (stat(s->cfg.overlay.nvim_socket, &st) == 0);

    if(exists && !s_nvim_socket_existed_last) {
      /* Socket just appeared — wait one full cycle before connecting
       * so nvim has time to finish setting up its RPC listener */
      s_nvim_socket_existed_last = true;
      s_nvim_retry_count         = 0;
      next_ms                    = 1000; /* one extra second grace period */
      wl_event_source_timer_update(s->nvim_timer, next_ms);
      return 0;
    }
    s_nvim_socket_existed_last = exists;

    if(exists) {
      bool ok = nvim_connect(s->cfg.overlay.nvim_socket);
      if(ok) {
        s_nvim_retry_count = 0;
        next_ms            = 5000;
      } else {
        /* Exponential backoff: 2s, 4s, 8s, cap at 16s */
        s_nvim_retry_count++;
        next_ms = 2000 * (1 << (s_nvim_retry_count < 3 ? s_nvim_retry_count : 3));
      }
    } else {
      s_nvim_socket_existed_last = false;
      next_ms                    = 2000;
    }
  } else {
    next_ms = 5000;
  }

  wl_event_source_timer_update(s->nvim_timer, next_ms);
  return 0;
}

static void view_apply_geom(TrixieServer *s, TrixieView *v, Pane *p) {
  int bw = (p->floating || p->fullscreen) ? 0 : s->twm.border_w;
  int th = pane_title_extra(s, p);
  int cw = p->rect.w - bw * 2;
  int ch = p->rect.h - bw * 2 - th;
  if(cw < 1) cw = 1;
  if(ch < 1) ch = 1;
#ifdef HAS_XWAYLAND
  if(v->is_xwayland && v->xwayland_surface) {
    wlr_xwayland_surface_configure(v->xwayland_surface,
                                   (int16_t)(p->rect.x + bw),
                                   (int16_t)(p->rect.y + bw + th),
                                   (uint16_t)cw,
                                   (uint16_t)ch);
    return;
  }
#endif
  wlr_xdg_toplevel_set_size(v->xdg_toplevel, cw, ch);
}

/* ── Spawn ────────────────────────────────────────────────────────────────── */

void server_spawn(TrixieServer *s, const char *cmd) {
  (void)s;
  if(!cmd || !cmd[0]) return;
  wlr_log(WLR_INFO, "spawn: %s", cmd);
  pid_t pid = fork();
  if(pid < 0) return;
  if(pid == 0) {
    pid_t pid2 = fork();
    if(pid2 < 0) _exit(1);
    if(pid2 == 0) {
      setsid();
      int maxfd = (int)sysconf(_SC_OPEN_MAX);
      for(int fd = 3; fd < maxfd; fd++)
        close(fd);
      if(!getenv("PATH")) setenv("PATH", "/usr/local/bin:/usr/bin:/bin", 1);
      execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
      _exit(127);
    }
    _exit(0);
  }
  waitpid(pid, NULL, 0);
}

/* ── Focus ────────────────────────────────────────────────────────────────── */

TrixieView *view_from_pane(TrixieServer *s, PaneId id) {
  TrixieView *v;
  wl_list_for_each(v, &s->views, link) if(v->pane_id == id && v->mapped) return v;
  return NULL;
}

void server_focus_pane(TrixieServer *s, PaneId id) {
  TrixieView *v = view_from_pane(s, id);
  if(!v) return;

  struct wlr_surface *surf = NULL;
#ifdef HAS_XWAYLAND
  if(v->is_xwayland && v->xwayland_surface) {
    surf = v->xwayland_surface->surface;
    wlr_xwayland_surface_activate(v->xwayland_surface, true);
  } else
#endif
      if(v->xdg_toplevel) {
    surf = v->xdg_toplevel->base->surface;
  }
  if(!surf) return;

  struct wlr_keyboard *kb = wlr_seat_get_keyboard(s->seat);
  wlr_scene_node_raise_to_top(&v->scene_tree->node);
  if(kb)
    wlr_seat_keyboard_notify_enter(
        s->seat, surf, kb->keycodes, kb->num_keycodes, &kb->modifiers);

  TrixieView *fv;
  wl_list_for_each(fv, &s->views, link) {
    if(fv->foreign_handle)
      wlr_foreign_toplevel_handle_v1_set_activated(fv->foreign_handle, fv == v);
#ifdef HAS_XWAYLAND
    if(fv->is_xwayland && fv->xwayland_surface && fv != v)
      wlr_xwayland_surface_activate(fv->xwayland_surface, false);
#endif
    /* Broadcast activation to every XDG toplevel so toolkits repaint. */
    if(!fv->is_xwayland && fv->xdg_toplevel && fv->mapped)
      wlr_xdg_toplevel_set_activated(fv->xdg_toplevel, fv == v);
  }
}

static void server_mark_deco_dirty(TrixieServer *s) {
  TrixieOutput *o;
  wl_list_for_each(o, &s->outputs, link) o->deco_dirty = true;
  server_request_redraw(s);
}

void server_sync_focus(TrixieServer *s) {
  PaneId id = twm_focused_id(&s->twm);
  if(id) server_focus_pane(s, id);
  ipc_push_focus_changed(s);
  lua_on_focus_changed(s);
  server_mark_deco_dirty(s);
}

/* ── Window sync ──────────────────────────────────────────────────────────── */

void server_sync_windows(TrixieServer *s) {
  Workspace *ws         = &s->twm.workspaces[s->twm.active_ws];
  int        incoming_x = anim_ws_incoming_x(&s->anim);
  PaneId     focused_id = twm_focused_id(&s->twm);

  TrixieView *v;
  wl_list_for_each(v, &s->views, link) {
    if(!v->mapped) continue;
    Pane *p = twm_pane_by_id(&s->twm, v->pane_id);
    if(!p) continue;

    bool on_ws = false;
    for(int i = 0; i < ws->pane_count; i++)
      if(ws->panes[i] == v->pane_id) { on_ws = true; break; }

    bool is_closing = anim_is_closing(&s->anim, v->pane_id);

    if(!on_ws && !is_closing) {
      wlr_scene_node_set_enabled(&v->scene_tree->node, false);
#ifdef HAS_XWAYLAND
      if(v->is_xwayland && v->xwayland_surface)
        wlr_xwayland_surface_activate(v->xwayland_surface, false);
#endif
      continue;
    }
    wlr_scene_node_set_enabled(&v->scene_tree->node, true);

    Rect r  = anim_get_rect(&s->anim, v->pane_id, p->rect);
    int  bw = (p->floating || p->fullscreen) ? 0 : s->twm.border_w;
    int  th = pane_title_extra(s, p);

    int win_x = r.x + bw + incoming_x;
    int win_y = r.y + bw + th;
    int win_w = r.w - bw * 2;
    int win_h = r.h - bw * 2 - th;
    if(win_w < 1) win_w = 1;
    if(win_h < 1) win_h = 1;

    wlr_scene_node_set_position(&v->scene_tree->node, win_x, win_y);

    bool  is_focused = (v->pane_id == focused_id);

    /* ── Opacity resolution (fixed sentinel ordering) ─────────────────── */
    float opacity;
    if(p->rule_opacity > 0.0f) {
      /* Cached rule matched. */
      opacity = p->rule_opacity;
    } else if(p->rule_opacity < 0.0f) {
      /* Previously scanned, no rule matched. */
      opacity = 1.0f;
    } else {
      /* Cache miss (== 0.0f sentinel) — scan once and store. */
      opacity = 1.0f;
      for(int ri = 0; ri < s->cfg.win_rule_count; ri++) {
        WinRule *wr = &s->cfg.win_rules[ri];
        if(wr->opacity > 0.0f && wr->app_id[0] && strstr(p->app_id, wr->app_id)) {
          opacity = wr->opacity;
          break;
        }
      }
      p->rule_opacity = opacity > 0.0f ? opacity : -1.0f;
    }
    /* Dim unfocused tiled windows only — floating/fullscreen always full. */
    if(opacity >= 1.0f && !is_focused && !p->floating && !p->fullscreen)
      opacity = 0.88f;

    /* For floating panes mid-open/close animation, blend in the fade opacity. */
    if(p->floating || is_closing) {
      float fade_op = anim_get_opacity(&s->anim, v->pane_id, -1.0f);
      if(fade_op >= 0.0f) opacity *= fade_op;
    }
#ifdef WLR_SCENE_HAS_OPACITY
    wlr_scene_node_set_opacity(&v->scene_tree->node, opacity);
#else
    (void)opacity;
#endif

    if(!is_closing) {
      int cfg_w = p->rect.w - bw * 2;
      int cfg_h = p->rect.h - bw * 2 - th;
      if(cfg_w < 1) cfg_w = 1;
      if(cfg_h < 1) cfg_h = 1;

      bool at_frame0 = false;
      {
        Rect got = anim_get_rect(&s->anim, v->pane_id, p->rect);
        if(got.x + got.w <= 0 || got.y + got.h <= 0 || got.x >= s->twm.screen_w ||
           got.y >= s->twm.screen_h)
          at_frame0 = true;
      }
      if(!at_frame0) {
#ifdef HAS_XWAYLAND
        if(v->is_xwayland && v->xwayland_surface) {
          struct wlr_xwayland_surface *xs = v->xwayland_surface;
          if(xs->width != cfg_w || xs->height != cfg_h)
            wlr_xwayland_surface_configure(xs,
                                           (int16_t)(p->rect.x + bw),
                                           (int16_t)(p->rect.y + bw + th),
                                           (uint16_t)cfg_w,
                                           (uint16_t)cfg_h);
        } else
#endif
            if(v->xdg_toplevel) {
          struct wlr_box cur;
          wlr_xdg_surface_get_geometry(v->xdg_toplevel->base, &cur);
          if(cur.width != cfg_w || cur.height != cfg_h)
            wlr_xdg_toplevel_set_size(v->xdg_toplevel, cfg_w, cfg_h);
        }
      }
    }
  }
}

/* ── Redraw ───────────────────────────────────────────────────────────────── */

void server_request_redraw(TrixieServer *s) {
  TrixieOutput *o;
  wl_list_for_each(o, &s->outputs, link) wlr_output_schedule_frame(o->wlr_output);
}

/* ── Animated layout reflow ───────────────────────────────────────────────── *
 * Call this instead of bare twm_reflow() whenever a layout change should be  *
 * animated.  It snapshots every tiled pane's current rect, performs the      *
 * reflow, then fires anim_morph(old→new) for each pane that moved.           *
 *                                                                             *
 * Floating and fullscreen panes are skipped — they don't participate in the  *
 * tiling layout.  Panes with an active open/close animation are also skipped *
 * (anim_morph guards those internally).                                       *
 *                                                                             *
 * TWM_MORPH_SWAP(s, call) wraps any twm call that internally calls           *
 * twm_reflow itself (twm_swap, twm_swap_main): snapshot BEFORE, morph AFTER. */
#define TWM_MORPH_SWAP(s_, call_) do { \
  Workspace *_ws = &(s_)->twm.workspaces[(s_)->twm.active_ws]; \
  typedef struct { PaneId id; Rect r; } _Snap; \
  _Snap _snaps[64]; int _nsn = 0; \
  for(int _i = 0; _i < _ws->pane_count && _nsn < 64; _i++) { \
    Pane *_p = twm_pane_by_id(&(s_)->twm, _ws->panes[_i]); \
    if(_p && !_p->floating && !_p->fullscreen) _snaps[_nsn++] = (_Snap){ _p->id, _p->rect }; \
  } \
  (call_); \
  for(int _i = 0; _i < _nsn; _i++) { \
    Pane *_p = twm_pane_by_id(&(s_)->twm, _snaps[_i].id); \
    if(_p) { \
      anim_morph(&(s_)->anim, _snaps[_i].id, _snaps[_i].r, _p->rect); \
      TrixieView *_v = view_from_pane((s_), _snaps[_i].id); \
      if(_v) view_apply_geom((s_), _v, _p); \
    } \
  } \
  server_sync_windows(s_); server_mark_deco_dirty(s_); server_request_redraw(s_); \
} while(0)
static void server_reflow_with_morph(TrixieServer *s) {
  Workspace *ws = &s->twm.workspaces[s->twm.active_ws];

  /* Snapshot current rects keyed by pane id. */
  typedef struct { PaneId id; Rect r; } Snap;
  Snap snap[64];
  int  snap_n = 0;
  for(int i = 0; i < ws->pane_count && snap_n < 64; i++) {
    Pane *p = twm_pane_by_id(&s->twm, ws->panes[i]);
    if(!p || p->floating || p->fullscreen) continue;
    snap[snap_n++] = (Snap){ p->id, p->rect };
  }

  twm_reflow(&s->twm);

  /* Fire morph animations for every pane whose rect changed. */
  for(int i = 0; i < snap_n; i++) {
    Pane *p = twm_pane_by_id(&s->twm, snap[i].id);
    if(!p) continue;
    anim_morph(&s->anim, snap[i].id, snap[i].r, p->rect);
    /* Also send a configure so the client knows its new size immediately.
     * Without this the client only hears about the size change once the
     * animation completes (via server_sync_windows size check). */
    TrixieView *v = view_from_pane(s, snap[i].id);
    if(v) view_apply_geom(s, v, p);
  }

  server_sync_windows(s);
  server_mark_deco_dirty(s);
  server_request_redraw(s);
}

/* ── Action dispatch ──────────────────────────────────────────────────────── */

void server_dispatch_action(TrixieServer *s, Action *a) {
  switch(a->kind) {
    case ACTION_SCREENSHOT_FULL: screenshot_full(s); break;
    case ACTION_SCREENSHOT_WINDOW: screenshot_window(s); break;
    case ACTION_SCREENSHOT_REGION: {
      ScreenshotRegion r = { 0 };
      screenshot_region(s, r); /* r.w==0 → slurp interactive picker */
      break;
    }
    case ACTION_QUIT:
      s->running = false;
      wl_display_terminate(s->display);
      break;
    case ACTION_RELOAD: server_binary_reload(s); break;
    case ACTION_EXEC: server_spawn(s, a->exec_cmd); break;
    case ACTION_CLOSE: {
      PaneId id = twm_focused_id(&s->twm);
      if(id) {
        TrixieView *v = view_from_pane(s, id);
        if(v) {
#ifdef HAS_XWAYLAND
          if(v->is_xwayland && v->xwayland_surface)
            wlr_xwayland_surface_close(v->xwayland_surface);
          else
#endif
              if(v->xdg_toplevel)
            wlr_xdg_toplevel_send_close(v->xdg_toplevel);
        }
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
          struct wlr_scene_tree *tgt =
              p->fullscreen ? s->layer_floating : s->layer_windows;
          wlr_scene_node_reparent(&v->scene_tree->node, tgt);
          wlr_scene_node_raise_to_top(&v->scene_tree->node);
          if(v->foreign_handle)
            wlr_foreign_toplevel_handle_v1_set_fullscreen(v->foreign_handle,
                                                          p->fullscreen);
#ifndef HAS_XWAYLAND
          wlr_xdg_toplevel_set_fullscreen(v->xdg_toplevel, p->fullscreen);
#else
          if(!v->is_xwayland && v->xdg_toplevel)
            wlr_xdg_toplevel_set_fullscreen(v->xdg_toplevel, p->fullscreen);
#endif
        }
        server_reflow_with_morph(s);
      }
      break;
    }
    case ACTION_TOGGLE_FLOAT: server_float_toggle(s); break;
    case ACTION_TOGGLE_BAR:
      s->twm.bar_visible = !s->twm.bar_visible;
      twm_set_bar_height(&s->twm,
                         s->twm.bar_visible ? s->cfg.bar.height : 0,
                         s->cfg.bar.position == BAR_BOTTOM);
      {
        TrixieOutput *o;
        wl_list_for_each(o, &s->outputs, link)
            bar_set_visible(o->bar, s->twm.bar_visible);
      }
      server_reflow_with_morph(s);
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
      TWM_MORPH_SWAP(s, twm_swap_dir(&s->twm, -1,  0));
      break;
    case ACTION_MOVE_RIGHT:
      TWM_MORPH_SWAP(s, twm_swap_dir(&s->twm,  1,  0));
      break;
    case ACTION_MOVE_UP:
      TWM_MORPH_SWAP(s, twm_swap_dir(&s->twm,  0, -1));
      break;
    case ACTION_MOVE_DOWN:
      TWM_MORPH_SWAP(s, twm_swap_dir(&s->twm,  0,  1));
      break;
    case ACTION_SWAP_MAIN:
      TWM_MORPH_SWAP(s, twm_swap_main(&s->twm));
      break;
    case ACTION_WORKSPACE: {
      int old = s->twm.active_ws;
      twm_switch_ws(&s->twm, a->n - 1);
      int nw = s->twm.active_ws;
      if(nw != old) {
        anim_workspace_transition(&s->anim, nw > old ? WS_DIR_RIGHT : WS_DIR_LEFT);
        ipc_push_workspace_changed(s);
        lua_on_workspace_changed(s);
      }
      server_sync_focus(s);
      server_sync_windows(s);
      server_mark_deco_dirty(s);
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
      server_reflow_with_morph(s);
      break;
    }
    case ACTION_PREV_LAYOUT: {
      Workspace *ws = &s->twm.workspaces[s->twm.active_ws];
      ws->layout    = layout_prev(ws->layout);
      server_reflow_with_morph(s);
      break;
    }
    case ACTION_GROW_MAIN: {
      Workspace *ws = &s->twm.workspaces[s->twm.active_ws];
      ws->main_ratio += 0.05f;
      if(ws->main_ratio > 0.9f) ws->main_ratio = 0.9f;
      server_reflow_with_morph(s);
      break;
    }
    case ACTION_SHRINK_MAIN: {
      Workspace *ws = &s->twm.workspaces[s->twm.active_ws];
      ws->main_ratio -= 0.05f;
      if(ws->main_ratio < 0.1f) ws->main_ratio = 0.1f;
      server_reflow_with_morph(s);
      break;
    }
    case ACTION_SCRATCHPAD: server_scratch_toggle(s, a->name); break;
    case ACTION_SWITCH_VT:
      if(s->session) wlr_session_change_vt(s->session, (unsigned)a->n);
      break;
    case ACTION_EMERGENCY_QUIT:
      s->running = false;
      wl_display_terminate(s->display);
      break;
    case ACTION_TOGGLE_OVERLAY:
      /* Overlay is per-output; iterate and toggle all. */
      {
        TrixieOutput *o;
        wl_list_for_each(o, &s->outputs, link) if(o->overlay)
            overlay_toggle(o->overlay);
      }
      server_request_redraw(s);
      break;
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
  server_mark_deco_dirty(s);
}

void server_scratch_toggle(TrixieServer *s, const char *name) {
  Scratchpad *sp = NULL;
  for(int i = 0; i < s->twm.scratch_count; i++)
    if(!strcmp(s->twm.scratchpads[i].name, name)) {
      sp = &s->twm.scratchpads[i];
      break;
    }
  if(!sp) return;

  /* If the scratchpad has no live pane, try to spawn its bound app.
   * The exec command lives in cfg.scratchpads (parsed from config).
   * Once spawned, the app will map, twm_try_assign_scratch will claim it,
   * and the next toggle keypress will show it. */
  if(!sp->has_pane) {
    for(int i = 0; i < s->cfg.scratchpad_count; i++) {
      ScratchpadCfg *sc = &s->cfg.scratchpads[i];
      if(strcmp(sc->name, name) != 0) continue;
      if(sc->exec[0]) {
        wlr_log(WLR_INFO, "scratch: spawning '%s' for scratchpad '%s'",
                sc->exec, name);
        server_spawn(s, sc->exec);
      } else {
        wlr_log(WLR_INFO, "scratch: no exec set for scratchpad '%s' — "
                "launch the app manually once to register it", name);
      }
      break;
    }
    return;
  }

  bool   was_visible = sp->visible;
  PaneId pid         = sp->pane_id;
  twm_toggle_scratch(&s->twm, name);
  Pane *p = twm_pane_by_id(&s->twm, pid);
  if(p) {
    if(!was_visible && sp->visible)
      anim_scratch_open(&s->anim, pid, p->rect);
    else if(was_visible && !sp->visible)
      anim_scratch_close(&s->anim, pid, p->rect);
  }
  server_sync_windows(s);
  server_sync_focus(s);
  server_mark_deco_dirty(s);
}

/* ── Config reload ────────────────────────────────────────────────────────── */

void server_apply_config_reload(TrixieServer *s) {
  Layout layout_snap[MAX_WORKSPACES];
  float  ratio_snap[MAX_WORKSPACES];
  int    n = s->twm.ws_count;
  for(int i = 0; i < n; i++) {
    layout_snap[i] = s->twm.workspaces[i].layout;
    ratio_snap[i]  = s->twm.workspaces[i].main_ratio;
  }
  config_reload(&s->cfg);
  bar_sync_exec_modules(&s->cfg);
  s->twm.gap        = s->cfg.gap;
  s->twm.border_w   = s->cfg.border_width;
  s->twm.smart_gaps = s->cfg.smart_gaps;
  s->twm.padding    = s->cfg.outer_gap;

  /* Re-color the root background rect from the (possibly new) theme. */
  if(s->bg_rect) {
    Color bc    = s->cfg.colors.background;
    float fc[4] = { bc.r / 255.0f, bc.g / 255.0f, bc.b / 255.0f, bc.a / 255.0f };
    wlr_scene_rect_set_color(s->bg_rect, fc);
  }
  twm_set_bar_height(&s->twm, s->cfg.bar.height, s->cfg.bar.position == BAR_BOTTOM);
  wlr_xcursor_manager_load(s->xcursor_mgr, s->cfg.cursor_size);
  int new_n = s->twm.ws_count;
  for(int i = 0; i < new_n && i < n; i++) {
    s->twm.workspaces[i].layout     = layout_snap[i];
    s->twm.workspaces[i].main_ratio = ratio_snap[i];
  }
  TrixieKeyboard *kb;
  wl_list_for_each(kb, &s->keyboards, link) wlr_keyboard_set_repeat_info(
      kb->wlr_keyboard, s->cfg.keyboard.repeat_rate, s->cfg.keyboard.repeat_delay);
  if(s->idle_timer) {
    s->idle_timeout_ms = s->cfg.idle_timeout * 1000;
    wl_event_source_timer_update(s->idle_timer,
                                 s->idle_timeout_ms > 0 ? s->idle_timeout_ms : 0);
  }
  TrixieOutput *o;
  wl_list_for_each(o, &s->outputs, link) {
      /* re-resolve saturation + shader_enabled per output */
      float sat   = s->cfg.saturation;
      bool  sh_on = s->cfg.shader_enabled;
      for(int i = 0; i < s->cfg.monitor_count; i++) {
          MonitorCfg *mcfg = &s->cfg.monitors[i];
          if(!strcmp(mcfg->name, o->wlr_output->name) && mcfg->shader_set) {
              if(mcfg->saturation > 0.0f) sat = mcfg->saturation;
              sh_on = mcfg->shader_enabled;
              break;
          }
      }
      o->saturation     = sat;
      o->shader_enabled = sh_on;

      if(o->bar) bar_mark_dirty(o->bar);
      if(o->overlay) overlay_resize(o->overlay, o->logical_w, o->logical_h);
  }
  twm_reflow(&s->twm);
  server_sync_windows(s);
  server_request_redraw(s);
  wlr_log(WLR_INFO, "config reloaded");
  /* Re-exec init.lua so Lua config overrides survive the reload. */
  lua_reload(s);
}

/* ── Binary hot-reload ────────────────────────────────────────────────────── *
 *
 * Flow:
 *   1. server_binary_reload() — forks ninja, pipes its stdout+stderr into the
 *      wl event loop, pushes "building…" to the overlay.
 *   2. reload_pipe_cb() — streams ninja output lines to the overlay log so
 *      you can watch errors without leaving the compositor.
 *   3. On ninja EOF, we waitpid().  Exit 0 → execv the new binary in-place.
 *      Exit non-zero → push "build failed" to overlay, compositor keeps running.
 *
 * The execv call replaces the compositor process in-place — same PID, same
 * Wayland socket fd (we clear CLOEXEC on it), same DISPLAY env.  All running
 * clients reconnect automatically within 1-2 frames.
 * ─────────────────────────────────────────────────────────────────────────── */

/* Push a message to the overlay on every output — convenience wrapper. */
static void reload_notify(TrixieServer *s, const char *title, const char *msg) {
  TrixieOutput *o;
  wl_list_for_each(o, &s->outputs, link) {
    if(o->overlay) overlay_notify(o->overlay, title, msg);
  }
}

/* Called when the ninja pipe has data (or closes). */
static int reload_pipe_cb(int fd, uint32_t mask, void *data) {
  TrixieServer *s = data;
  char          buf[2048];

  if(mask & WL_EVENT_READABLE) {
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if(n > 0) {
      buf[n] = '\0';
      /* Strip trailing newline for the overlay notification. */
      while(n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = '\0';
      if(n > 0) reload_notify(s, "rebuild", buf);
    }
  }

  if(mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
    /* Ninja finished — collect exit status. */
    int   status   = 0;
    pid_t finished = waitpid(s->reload_pid, &status, WNOHANG);

    /* Clean up the event source and pipe fd. */
    if(s->reload_pipe_src) {
      wl_event_source_remove(s->reload_pipe_src);
      s->reload_pipe_src = NULL;
    }
    close(fd);
    s->reload_pipe_fd = -1;
    s->reload_pid     = -1;

    int exit_code = (finished > 0 && WIFEXITED(status)) ? WEXITSTATUS(status) : 1;

    if(exit_code != 0) {
      wlr_log(WLR_ERROR, "hot-reload: ninja failed (exit %d)", exit_code);
      reload_notify(s, "rebuild", "build FAILED — compositor unchanged");
      return 0;
    }

    wlr_log(WLR_INFO, "hot-reload: build succeeded, exec-replacing…");
    reload_notify(s, "rebuild", "build OK — reloading…");

    /* Give the overlay one frame to render the message before we vanish. */
    server_request_redraw(s);

    /* Clear CLOEXEC on the Wayland display socket fd so it survives execv.
     * wlroots sets CLOEXEC when it creates the socket; without clearing it
     * the fd is closed by the kernel on execv and the new binary can't
     * re-use the same socket name (clients would lose their connection).
     *
     * wl_display_get_fd() returns the epoll/event-loop fd, not the listening
     * socket. The listening socket fd is stored internally by libwayland but
     * we can find it by scanning /proc/self/fd for the socket matching the
     * WAYLAND_DISPLAY path under XDG_RUNTIME_DIR. */
    {
      const char *xrd  = getenv("XDG_RUNTIME_DIR");
      const char *wdpy = getenv("WAYLAND_DISPLAY");
      if(xrd && wdpy) {
        char sock_path[256];
        snprintf(sock_path, sizeof(sock_path), "%s/%s", xrd, wdpy);

        /* Walk /proc/self/fd looking for the fd whose symlink matches
         * the Wayland socket path. */
        DIR *proc_fd = opendir("/proc/self/fd");
        if(proc_fd) {
          struct dirent *ent;
          while((ent = readdir(proc_fd)) != NULL) {
            if(ent->d_name[0] == '.') continue;
            char fd_link[64], target[256];
            snprintf(fd_link, sizeof(fd_link), "/proc/self/fd/%s", ent->d_name);
            ssize_t tlen = readlink(fd_link, target, sizeof(target) - 1);
            if(tlen > 0) {
              target[tlen] = '\0';
              if(strcmp(target, sock_path) == 0) {
                int wl_fd = atoi(ent->d_name);
                int flags = fcntl(wl_fd, F_GETFD);
                if(flags >= 0)
                  fcntl(wl_fd, F_SETFD, flags & ~FD_CLOEXEC);
                wlr_log(WLR_INFO,
                        "hot-reload: cleared CLOEXEC on Wayland socket fd %d (%s)",
                        wl_fd, sock_path);
                break;
              }
            }
          }
          closedir(proc_fd);
        }
      }
    }

    /* Flush the display so clients get a clean EOF on their connections
     * before the socket is unlinked and re-created by the new binary.      */
    wl_display_flush_clients(s->display);

    /* Unset WAYLAND_DISPLAY so the new process uses DRM, not nested Wayland. */
    unsetenv("WAYLAND_DISPLAY");
    unsetenv("DISPLAY");

    /* Release DRM master / seatd lease before exec-replacing.
     * Without this seatd makes the new process wait ~1.2s for the old
     * process's lease to time out before granting the seat. */
    wlr_backend_destroy(s->backend);
    s->backend = NULL;

    wlr_log(WLR_INFO, "hot-reload: execv → %s", s->reload_new_bin);
    execv(s->reload_new_bin, s->saved_argv);
    /* If we reach here execv failed — compositor still running. */
    wlr_log(WLR_ERROR, "hot-reload: execv failed: %s", strerror(errno));
    reload_notify(s, "rebuild", "execv failed — compositor unchanged");
  }
  return 0;
}

void server_binary_reload(TrixieServer *s) {
  /* Guard: don't start a second build if one is already running. */
  if(s->reload_pid > 0) {
    reload_notify(s, "rebuild", "build already in progress…");
    return;
  }

  /* Resolve the current binary path NOW, before ninja overwrites it.
   * /proc/self/exe after a successful build still resolves to the old
   * inode (the running process), so we must capture it here and store
   * it for the execv call in reload_pipe_cb. */
  {
    ssize_t len = readlink("/proc/self/exe",
                           s->reload_new_bin, sizeof(s->reload_new_bin) - 1);
    if(len <= 0) {
      wlr_log(WLR_ERROR, "hot-reload: readlink /proc/self/exe failed: %s",
              strerror(errno));
      reload_notify(s, "rebuild", "reload aborted — can't resolve binary path");
      return;
    }
    s->reload_new_bin[len] = '\0';
  }

  /* Resolve build directory: config value > binary directory > cwd. */
  char build_dir[512] = { 0 };
  if(s->cfg.build_dir[0]) {
    strncpy(build_dir, s->cfg.build_dir, sizeof(build_dir) - 1);
  } else {
    /* Fall back to the directory the running binary lives in. */
    char self[512] = { 0 };
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if(n > 0) {
      self[n] = '\0';
      char *slash = strrchr(self, '/');
      if(slash) {
        *slash = '\0';
        strncpy(build_dir, self, sizeof(build_dir) - 1);
      }
    }
    if(!build_dir[0]) strncpy(build_dir, ".", sizeof(build_dir) - 1);
  }

  /* Create a pipe for ninja's combined stdout+stderr. */
  int pipefd[2];
  if(pipe(pipefd) < 0) {
    wlr_log(WLR_ERROR, "hot-reload: pipe() failed: %s", strerror(errno));
    return;
  }
  /* Read end non-blocking so the wl event loop doesn't stall. */
  fcntl(pipefd[0], F_SETFL, O_NONBLOCK);

  pid_t pid = fork();
  if(pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    wlr_log(WLR_ERROR, "hot-reload: fork() failed: %s", strerror(errno));
    return;
  }

  if(pid == 0) {
    /* Child: run ninja in the build dir, stdout+stderr → pipe. */
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);
    if(chdir(build_dir) < 0) {
      fprintf(stderr, "hot-reload: chdir(%s) failed: %s\n",
              build_dir, strerror(errno));
      _exit(1);
    }
    execlp("ninja", "ninja", (char *)NULL);
    fprintf(stderr, "hot-reload: exec ninja failed: %s\n", strerror(errno));
    _exit(127);
  }

  /* Parent: store state, register pipe with the wl event loop. */
  close(pipefd[1]);
  s->reload_pid     = pid;
  s->reload_pipe_fd = pipefd[0];
  s->reload_pipe_src =
      wl_event_loop_add_fd(wl_display_get_event_loop(s->display),
                           pipefd[0],
                           WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR,
                           reload_pipe_cb,
                           s);

  char msg[640];
  snprintf(msg, sizeof(msg), "building in %s …", build_dir);
  wlr_log(WLR_INFO, "hot-reload: %s", msg);
  reload_notify(s, "rebuild", msg);
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
  server_reset_idle(s);

  uint32_t keycode = event->keycode + 8;

  /* Collect syms at the SHIFTED level (what xkb reports with modifiers). */
  const xkb_keysym_t *syms;
  int nsyms = xkb_state_key_get_syms(kb->wlr_keyboard->xkb_state, keycode, &syms);

  /* Also collect BASE-level syms (no modifiers applied) so that
   * "bind = SUPER SHIFT, t, ..." matches regardless of whether xkb
   * maps SHIFT+t → T or something else. */
  const xkb_keysym_t *base_syms;
  int                 nbase = xkb_keymap_key_get_syms_by_level(
      kb->wlr_keyboard->keymap, keycode, 0, 0, &base_syms);

  bool handled = false;

  /* Mask to only the four modifiers we track — ignore Num Lock, Caps Lock, etc. */
  uint32_t raw_mods = wlr_keyboard_get_modifiers(kb->wlr_keyboard);
  uint32_t mods     = wlr_mods_to_trixie(raw_mods);

  if(event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
    /* Emergency exit — always processed first, overlay or not. */
    for(int i = 0; i < nsyms; i++) {
      if(syms[i] == XKB_KEY_Print && (mods & MOD_SUPER) && (mods & MOD_SHIFT)) {
        s->running = false;
        wl_display_terminate(s->display);
        return;
      }
    }

    /* If any overlay is visible, feed the key there first. */
    {
      TrixieOutput *o;
      wl_list_for_each(o, &s->outputs, link) {
        if(o->overlay && overlay_visible(o->overlay)) {
          /* Try backtick / Escape to dismiss */
          bool dismiss = false;
          for(int i = 0; i < nsyms; i++) {
            if(syms[i] == XKB_KEY_grave || syms[i] == XKB_KEY_Escape) dismiss = true;
          }
          if(dismiss) {
            overlay_toggle(o->overlay);
            server_request_redraw(s);
            handled = true;
          } else {
            for(int i = 0; i < nsyms && !handled; i++)
              if(overlay_key(o->overlay, syms[i], mods)) handled = true;
            for(int i = 0; i < nbase && !handled; i++)
              if(overlay_key(o->overlay, base_syms[i], mods)) handled = true;
          }
          server_request_redraw(s);
          if(handled) goto key_done;
          break;
        }
      }
    }

    /* Lua binds first */
    for(int i = 0; i < nsyms && !handled; i++)
      if(lua_dispatch_key(s, mods, syms[i])) handled = true;
    for(int i = 0; i < nbase && !handled; i++)
      if(lua_dispatch_key(s, mods, base_syms[i])) handled = true;

    /* C keybinds — try shifted syms first, then base syms */
    for(int i = 0; i < nsyms && !handled; i++) {
      for(int ki = 0; ki < s->cfg.keybind_count; ki++) {
        Keybind *bind = &s->cfg.keybinds[ki];
        if(bind->mods == mods && bind->sym == syms[i]) {
          server_dispatch_action(s, &bind->action);
          handled = true;
          break;
        }
      }
    }
    for(int i = 0; i < nbase && !handled; i++) {
      for(int ki = 0; ki < s->cfg.keybind_count; ki++) {
        Keybind *bind = &s->cfg.keybinds[ki];
        if(bind->mods == mods && bind->sym == base_syms[i]) {
          server_dispatch_action(s, &bind->action);
          handled = true;
          break;
        }
      }
    }
  }

key_done:

  if(!handled) {
    wlr_seat_set_keyboard(s->seat, kb->wlr_keyboard);
    wlr_seat_keyboard_notify_key(
        s->seat, event->time_msec, event->keycode, event->state);
  }
}

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data) {
  (void)data;
  TrixieKeyboard *kb = CONTAINER_OF(listener, TrixieKeyboard, modifiers);
  wlr_seat_set_keyboard(kb->server->seat, kb->wlr_keyboard);
  wlr_seat_keyboard_notify_modifiers(kb->server->seat, &kb->wlr_keyboard->modifiers);
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
  (void)data;
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

  if(wlr_input_device_is_libinput(dev)) {
    struct libinput_device *li = wlr_libinput_get_device_handle(dev);
    libinput_device_config_tap_set_enabled(li, LIBINPUT_CONFIG_TAP_ENABLED);
    libinput_device_config_scroll_set_natural_scroll_enabled(li, 1);
    libinput_device_config_tap_set_drag_enabled(li, LIBINPUT_CONFIG_DRAG_ENABLED);
    libinput_device_config_tap_set_button_map(
        li, LIBINPUT_CONFIG_TAP_MAP_LMR); /* 1-tap=left, 2-tap=right, 3-tap=middle */
  }

  /* Attach gesture signal listeners directly to this pointer device. */
  struct wlr_pointer *ptr = wlr_pointer_from_input_device(dev);

  s->swipe_begin.notify  = handle_swipe_begin;
  s->swipe_update.notify = handle_swipe_update;
  s->swipe_end.notify    = handle_swipe_end;
  s->pinch_begin.notify  = handle_pinch_begin;
  s->pinch_update.notify = handle_pinch_update;
  s->pinch_end.notify    = handle_pinch_end;

  wl_signal_add(&ptr->events.swipe_begin, &s->swipe_begin);
  wl_signal_add(&ptr->events.swipe_update, &s->swipe_update);
  wl_signal_add(&ptr->events.swipe_end, &s->swipe_end);
  wl_signal_add(&ptr->events.pinch_begin, &s->pinch_begin);
  wl_signal_add(&ptr->events.pinch_update, &s->pinch_update);
  wl_signal_add(&ptr->events.pinch_end, &s->pinch_end);
}

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

static bool super_is_held(TrixieServer *s) {
  TrixieKeyboard *kb;
  wl_list_for_each(
      kb, &s->keyboards, link) if(wlr_keyboard_get_modifiers(kb->wlr_keyboard) &
                                  WLR_MODIFIER_LOGO) return true;
  return false;
}

static PaneId pane_at_cursor(TrixieServer *s, double lx, double ly) {
  int        px = (int)lx, py = (int)ly;
  Workspace *ws = &s->twm.workspaces[s->twm.active_ws];
  int        bw = s->twm.border_w;
  /* Pass 1: floating panes (checked front-to-back). */
  for(int i = ws->pane_count - 1; i >= 0; i--) {
    Pane *p = twm_pane_by_id(&s->twm, ws->panes[i]);
    if(!p || !p->floating) continue;
    if(rect_contains(p->rect, px, py)) return p->id;
  }
  /* Pass 2: tiled panes. */
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

#ifdef HAS_XWAYLAND
  /* Check unmanaged (override_redirect) XWayland surfaces first — they sit
   * above managed windows and must receive pointer events for menus/tooltips. */
  {
    TrixieView *uv;
    wl_list_for_each(uv, &s->views, link) {
      if(!uv->is_xwayland || !uv->xwayland_surface) continue;
      if(!uv->xwayland_surface->override_redirect) continue;
      if(!uv->mapped) continue;
      struct wlr_xwayland_surface *xs = uv->xwayland_surface;
      int x = xs->x, y = xs->y, w = xs->width, h = xs->height;
      if((int)cx >= x && (int)cx < x + w && (int)cy >= y && (int)cy < y + h) {
        struct wlr_scene_node *node =
            wlr_scene_node_at(&s->scene->tree.node, cx, cy, &sx, &sy);
        if(node && node->type == WLR_SCENE_NODE_BUFFER) {
          struct wlr_scene_buffer  *sb = wlr_scene_buffer_from_node(node);
          struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(sb);
          if(ss) surface = ss->surface;
        }
        if(surface) break;
      }
    }
  }
#endif

  PaneId pid = pane_at_cursor(s, cx, cy);
  if(!surface && pid) {
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

  if(surface && s->pointer_constraints) {
    struct wlr_pointer_constraint_v1 *constraint =
        wlr_pointer_constraints_v1_constraint_for_surface(
            s->pointer_constraints, surface, s->seat);
    if(constraint) wlr_pointer_constraint_v1_send_activated(constraint);
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
  server_reset_idle(s);
  /* Track DnD drag icon with cursor position. */
  if(g_drag_icon_tree)
    wlr_scene_node_set_position(&g_drag_icon_tree->node,
                                (int)s->cursor->x, (int)s->cursor->y);
  if(s->relative_pointer_mgr)
    wlr_relative_pointer_manager_v1_send_relative_motion(
        s->relative_pointer_mgr,
        s->seat,
        (uint64_t)ev->time_msec * 1000,
        ev->delta_x,
        ev->delta_y,
        ev->unaccel_dx,
        ev->unaccel_dy);
  if(s->drag_mode == DRAG_MOVE) {
    twm_float_move(&s->twm, s->drag_pane, (int)ev->delta_x, (int)ev->delta_y);
    server_sync_windows(s);
    server_request_redraw(s);
    return;
  }
  if(s->drag_mode == DRAG_RESIZE) {
    Pane *p = twm_pane_by_id(&s->twm, s->drag_pane);
    if(p && p->floating) {
      twm_float_resize(&s->twm, s->drag_pane, (int)ev->delta_x, (int)ev->delta_y);
      /* Send a configure so the client knows the new size. */
      TrixieView *v = view_from_pane(s, s->drag_pane);
      if(v) view_apply_geom(s, v, p);
    } else if(p && !p->floating) {
      Workspace *ws = &s->twm.workspaces[s->twm.active_ws];
      float      delta =
          (float)ev->delta_x / (float)(s->twm.screen_w > 0 ? s->twm.screen_w : 1);
      ws->main_ratio += delta;
      if(ws->main_ratio < 0.1f) ws->main_ratio = 0.1f;
      if(ws->main_ratio > 0.9f) ws->main_ratio = 0.9f;
      twm_reflow(&s->twm);
    }
    server_sync_windows(s);
    server_request_redraw(s);
    return;
  }
  update_cursor_focus(s, ev->time_msec);
}

static void handle_cursor_motion_abs(struct wl_listener *listener, void *data) {
  TrixieServer *s = CONTAINER_OF(listener, TrixieServer, cursor_motion_abs);
  struct wlr_pointer_motion_absolute_event *ev = data;

  /* Compute delta from previous position before warping. */
  double prev_x = s->cursor->x, prev_y = s->cursor->y;
  wlr_cursor_warp_absolute(s->cursor, &ev->pointer->base, ev->x, ev->y);
  double dx = s->cursor->x - prev_x, dy = s->cursor->y - prev_y;

  server_reset_idle(s);
  /* Track DnD drag icon with cursor position. */
  if(g_drag_icon_tree)
    wlr_scene_node_set_position(&g_drag_icon_tree->node,
                                (int)s->cursor->x, (int)s->cursor->y);

  if(s->drag_mode == DRAG_MOVE) {
    twm_float_move(&s->twm, s->drag_pane, (int)dx, (int)dy);
    server_sync_windows(s);
    server_request_redraw(s);
    return;
  }
  if(s->drag_mode == DRAG_RESIZE) {
    Pane *p = twm_pane_by_id(&s->twm, s->drag_pane);
    if(p && p->floating) {
      twm_float_resize(&s->twm, s->drag_pane, (int)dx, (int)dy);
      TrixieView *v = view_from_pane(s, s->drag_pane);
      if(v) view_apply_geom(s, v, p);
    } else if(p && !p->floating) {
      Workspace *ws = &s->twm.workspaces[s->twm.active_ws];
      float      delta = (float)dx / (float)(s->twm.screen_w > 0 ? s->twm.screen_w : 1);
      ws->main_ratio += delta;
      if(ws->main_ratio < 0.1f) ws->main_ratio = 0.1f;
      if(ws->main_ratio > 0.9f) ws->main_ratio = 0.9f;
      twm_reflow(&s->twm);
    }
    server_sync_windows(s);
    server_request_redraw(s);
    return;
  }
  update_cursor_focus(s, ev->time_msec);
}

static void handle_cursor_button(struct wl_listener *listener, void *data) {
  TrixieServer *s = CONTAINER_OF(listener, TrixieServer, cursor_button);
  struct wlr_pointer_button_event *ev = data;
  wlr_seat_pointer_notify_button(s->seat, ev->time_msec, ev->button, ev->state);
  server_reset_idle(s);
  if(ev->state == WL_POINTER_BUTTON_STATE_RELEASED) {
    s->drag_mode = DRAG_NONE;
    return;
  }
  double cx = s->cursor->x, cy = s->cursor->y;
  PaneId pid = pane_at_cursor(s, cx, cy);
  if(pid) {
    twm_set_focused(&s->twm, pid);
    server_focus_pane(s, pid);
    bool  held     = super_is_held(s);
    Pane *p        = twm_pane_by_id(&s->twm, pid);
    bool  is_float = p && p->floating;
    if(held && ev->button == BTN_LEFT) {
      s->drag_mode = is_float ? DRAG_MOVE : DRAG_RESIZE;
      s->drag_pane = pid;
      /* Cancel any open/close animation so anim_get_rect stops overriding
       * p->rect.  Without this the window ignores cursor movement for the
       * duration of the animation (typically ~250ms after window open). */
      anim_cancel(&s->anim, pid);
      return;
    }
    if(held && ev->button == BTN_RIGHT) {
      s->drag_mode = DRAG_RESIZE;
      s->drag_pane = pid;
      anim_cancel(&s->anim, pid);
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
  (void)data;
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

/* ── XDG Popups (context menus, tooltips, dropdowns) ─────────────────────── */

static void handle_new_xdg_popup(struct wl_listener *listener, void *data) {
  TrixieServer         *s     = CONTAINER_OF(listener, TrixieServer, new_xdg_popup);
  struct wlr_xdg_popup *popup = data;

  struct wlr_scene_tree *parent_tree = s->layer_floating;

  struct wlr_surface *parent_wlr = popup->parent;
  if(parent_wlr) {
    struct wlr_xdg_surface *parent_xdg =
        wlr_xdg_surface_try_from_wlr_surface(parent_wlr);
    if(parent_xdg && parent_xdg->data)
      parent_tree = parent_xdg->data;
  }

  struct wlr_scene_tree *scene_tree =
      wlr_scene_xdg_surface_create(parent_tree, popup->base);
  if(!scene_tree) return;

  popup->base->data = scene_tree;

  /* Constrain popup to output geometry so the XDG positioner repositions it
   * if it would clip a screen edge, rather than rendering off-screen. */
  struct wlr_output *output =
      wlr_output_layout_output_at(s->output_layout, s->cursor->x, s->cursor->y);
  if(output) {
    struct wlr_box output_box;
    wlr_output_layout_get_box(s->output_layout, output, &output_box);
    wlr_xdg_popup_unconstrain_from_box(popup, &output_box);
  }

  wlr_scene_node_raise_to_top(&scene_tree->node);
}

/* ── Drag and drop ────────────────────────────────────────────────────────── */

/* Per-drag heap allocation so concurrent drags don't share a static listener. */
typedef struct {
  struct wl_listener destroy;
} DragIconCtx;

static void drag_icon_handle_destroy(struct wl_listener *listener, void *data) {
  (void)data;
  DragIconCtx *ctx = wl_container_of(listener, ctx, destroy);
  /* Clear global so cursor_motion stops updating the dead node. */
  g_drag_icon_tree = NULL;
  wl_list_remove(&ctx->destroy.link);
  free(ctx);
}

static void handle_request_start_drag(struct wl_listener *listener, void *data) {
  /* Client requests a DnD session — validate the pointer serial first.
   * An invalid serial means the button-press that started the drag was not
   * delivered by us, so we must not honour it (security requirement). */
  TrixieServer *s = CONTAINER_OF(listener, TrixieServer, seat_request_start_drag);
  struct wlr_seat_request_start_drag_event *ev = data;

  if(wlr_seat_validate_pointer_grab_serial(s->seat, ev->origin, ev->serial))
    wlr_seat_start_pointer_drag(s->seat, ev->drag, ev->serial);
  else
    wlr_data_source_destroy(ev->drag->source);
}

static void handle_start_drag(struct wl_listener *listener, void *data) {
  /* Seat confirmed the drag.  If it carries an icon surface (e.g. a file
   * thumbnail) create a scene node in the topmost layer so it rides the
   * cursor for the duration of the drag. */
  TrixieServer    *s    = CONTAINER_OF(listener, TrixieServer, seat_start_drag);
  struct wlr_drag *drag = data;

  if(!drag->icon) return;

  struct wlr_scene_tree *icon_tree =
      wlr_scene_drag_icon_create(s->layer_overlay, drag->icon);
  if(!icon_tree) return;

  /* Snap to cursor so there's no one-frame offset on drag start. */
  wlr_scene_node_set_position(&icon_tree->node,
                              (int)s->cursor->x, (int)s->cursor->y);

  /* Store globally so cursor_motion handlers can update position every event. */
  g_drag_icon_tree = icon_tree;

  DragIconCtx *ctx = calloc(1, sizeof(*ctx));
  if(ctx) {
    ctx->destroy.notify = drag_icon_handle_destroy;
    wl_signal_add(&drag->icon->events.destroy, &ctx->destroy);
  }
}

/* ── Output ───────────────────────────────────────────────────────────────── */

static void output_handle_frame(struct wl_listener *listener, void *data) {
    (void)data;
    TrixieOutput *o = CONTAINER_OF(listener, TrixieOutput, frame);
    TrixieServer *s = o->server;

    bool still_animating = anim_tick(&s->anim);
    bool need_sync       = still_animating || o->was_animating;
    o->was_animating     = still_animating;
    if (need_sync) server_sync_windows(s);

    bool bar_dirty     = bar_update(o->bar, &s->twm, &s->cfg);
    bool overlay_dirty = overlay_update(o->overlay, &s->twm, &s->cfg, &s->bar_workers);
    /* Bridge the TrixieOutput.deco_dirty flag (set by server_mark_deco_dirty)
     * into the TrixieDeco dirty flag so deco_update skips the O(n×m)
     * noborder scan on frames where nothing layout-relevant changed. */
    if(o->deco_dirty) { deco_mark_dirty(o->deco); o->deco_dirty = false; }
    deco_update(o->deco, &s->twm, &s->anim, &s->cfg);

    /* ── Render ──────────────────────────────────────────────────────────── *
     * If shader is enabled (not permanently failed), try shader_render_frame.*
     * It handles lazy GL init internally on the first call inside the render  *
     * pass where the context is guaranteed current by wlroots.               *
     * On any failure fall back to a plain wlr_scene_output_commit.           */
  if(o->shader_enabled && !o->shader_init_tried) {
    bool ok = shader_render_frame(&o->shader, s->renderer,
                                  o->scene_output, o->wlr_output,
                                  o->saturation);
    if(!ok && !o->shader.gl_init_done) {
      o->shader_init_tried = true;
      wlr_log(WLR_ERROR, "shader: permanently disabled for %s",
              o->wlr_output->name);
      wlr_scene_output_commit(o->scene_output, NULL);
    } else if(!ok) {
      wlr_scene_output_commit(o->scene_output, NULL);
    }
  } else {
    wlr_scene_output_commit(o->scene_output, NULL);
  }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(o->scene_output, &now);

    if (s->running && (still_animating || bar_dirty || overlay_dirty))
        wlr_output_schedule_frame(o->wlr_output);
}

static void output_handle_request_state(struct wl_listener *listener, void *data) {
  TrixieOutput *o = CONTAINER_OF(listener, TrixieOutput, request_state);
  struct wlr_output_event_request_state *ev = data;
  wlr_output_commit_state(o->wlr_output, ev->state);
}

static void output_handle_destroy(struct wl_listener *listener, void *data) {
  (void)data;
  TrixieOutput *o = CONTAINER_OF(listener, TrixieOutput, destroy);
  TrixieServer *s = o->server;
  wl_list_remove(&o->frame.link);
  wl_list_remove(&o->request_state.link);
  wl_list_remove(&o->destroy.link);
  wl_list_remove(&o->link);
  shader_output_finish(&o->shader);
  if(o->bar) bar_destroy(o->bar);
  if(o->deco) deco_destroy(o->deco);
  if(o->overlay) overlay_destroy(o->overlay);
  free(o);
  if(!wl_list_empty(&s->outputs)) {
    TrixieOutput *next = wl_container_of(s->outputs.next, next, link);
    twm_resize(&s->twm, next->width, next->height);
    anim_set_resize(&s->anim, next->width, next->height);
    server_sync_windows(s);
  }
}

static void handle_new_output(struct wl_listener *listener, void *data) {
  TrixieServer      *s          = CONTAINER_OF(listener, TrixieServer, new_output);
  struct wlr_output *wlr_output = data;

  wlr_output_init_render(wlr_output, s->allocator, s->renderer);

  struct wlr_output_state state;
  wlr_output_state_init(&state);
  wlr_output_state_set_enabled(&state, true);
  wlr_output_state_set_adaptive_sync_enabled(&state, true);

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
         abs(m->refresh - (int)(mcfg->refresh * 1000)) < 50) {
        mode = m;
        break;
      }
    }
    float scale = mcfg->scale > 0.1f ? mcfg->scale : 1.0f;
    wlr_output_state_set_scale(&state, scale);
  }
  if(!mode) mode = wlr_output_preferred_mode(wlr_output);
  if(mode) wlr_output_state_set_mode(&state, mode);
  wlr_output_commit_state(wlr_output, &state);
  wlr_output_state_finish(&state);

  if(mode)
    wlr_log(WLR_INFO,
            "output mode: %dx%d@%dmHz",
            mode->width,
            mode->height,
            mode->refresh);

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

  int ow    = mode ? mode->width : wlr_output->width;
  int oh    = mode ? mode->height : wlr_output->height;
  o->width  = ow;
  o->height = oh;

  {
    float sc              = (mcfg && mcfg->scale > 0.1f) ? mcfg->scale : 1.0f;
    o->logical_w          = (int)((float)ow / sc);
    o->logical_h          = (int)((float)oh / sc);
    TrixieOutput *primary = wl_container_of(s->outputs.next, primary, link);
    if(primary == o) {
      twm_resize(&s->twm, o->logical_w, o->logical_h);
      anim_set_resize(&s->anim, o->logical_w, o->logical_h);
    }
  }

  o->bar     = bar_create(s->layer_overlay, ow, oh, &s->cfg, &s->bar_workers);
  o->deco    = deco_create(s->layer_chrome, s->layer_chrome_floating);
  o->overlay = overlay_create(s->layer_overlay, o->logical_w, o->logical_h, &s->cfg);

  /* Resize the background rect to cover the full output. */
  if(s->bg_rect) wlr_scene_rect_set_size(s->bg_rect, ow, oh);

  {
    /* Resolve saturation + shader_enabled:
     * per-monitor config wins over global default. */
    float sat     = s->cfg.saturation;
    bool  sh_on   = s->cfg.shader_enabled;
    if(mcfg && mcfg->shader_set) {
      if(mcfg->saturation > 0.0f) sat = mcfg->saturation;
      sh_on = mcfg->shader_enabled;
    }
    o->saturation     = sat;
    o->shader_enabled = sh_on;
  }

  memset(&o->shader, 0, sizeof(o->shader));
  if(!o->shader_enabled || !shader_output_init(&o->shader, s->renderer, ow, oh)) {
    o->shader_init_tried = true; /* disabled or not GLES2 */
  }

  wlr_log(WLR_INFO, "new output: %s %dx%d", wlr_output->name, ow, oh);
}

/* ── Layer shell ──────────────────────────────────────────────────────────── */

static void layer_surface_handle_map(struct wl_listener *listener, void *data) {
  (void)data;
  TrixieLayerSurface          *ls  = CONTAINER_OF(listener, TrixieLayerSurface, map);
  TrixieServer                *s   = ls->server;
  struct wlr_layer_surface_v1 *wls = ls->wlr_surface;
  int                          exclusive = wls->current.exclusive_zone;
  uint32_t                     anchor    = wls->current.anchor;
  if(exclusive <= 0) return;
  bool at_top = (anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) &&
                !(anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM);
  bool at_bottom = (anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) &&
                   !(anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP);
  if(at_top || at_bottom) {
    twm_set_bar_height(&s->twm, exclusive, at_bottom);
    twm_reflow(&s->twm);
    server_sync_windows(s);
  }
}

static void layer_surface_handle_unmap(struct wl_listener *listener, void *data) {
  (void)data;
  TrixieLayerSurface *ls = CONTAINER_OF(listener, TrixieLayerSurface, unmap);
  TrixieServer       *s  = ls->server;
  twm_set_bar_height(&s->twm, s->cfg.bar.height, s->cfg.bar.position == BAR_BOTTOM);
  twm_reflow(&s->twm);
  server_sync_windows(s);
}

static void layer_surface_handle_destroy(struct wl_listener *listener, void *data) {
  (void)data;
  TrixieLayerSurface *ls = CONTAINER_OF(listener, TrixieLayerSurface, destroy);
  wl_list_remove(&ls->map.link);
  wl_list_remove(&ls->unmap.link);
  wl_list_remove(&ls->destroy.link);
  wl_list_remove(&ls->link);
  free(ls);
}

static void handle_new_layer_surface(struct wl_listener *listener, void *data) {
  TrixieServer *s = CONTAINER_OF(listener, TrixieServer, new_layer_surface);
  struct wlr_layer_surface_v1 *wls = data;
  struct wlr_scene_tree       *layer_tree;
  switch(wls->pending.layer) {
    case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
      layer_tree = s->layer_background;
      break;
    case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM: layer_tree = s->layer_windows; break;
    default: layer_tree = s->layer_overlay; break;
  }
  TrixieLayerSurface *ls = calloc(1, sizeof(*ls));
  ls->server             = s;
  ls->wlr_surface        = wls;
  ls->scene_surface      = wlr_scene_layer_surface_v1_create(layer_tree, wls);
  struct wlr_output *output =
      wlr_output_layout_output_at(s->output_layout, s->cursor->x, s->cursor->y);
  if(!output && !wl_list_empty(&s->outputs)) {
    TrixieOutput *o = wl_container_of(s->outputs.next, o, link);
    output          = o->wlr_output;
  }
  if(output) {
    struct wlr_box obox;
    wlr_output_layout_get_box(s->output_layout, output, &obox);
    wlr_layer_surface_v1_configure(wls, (uint32_t)obox.width, (uint32_t)obox.height);
  }
  ls->map.notify     = layer_surface_handle_map;
  ls->unmap.notify   = layer_surface_handle_unmap;
  ls->destroy.notify = layer_surface_handle_destroy;
  wl_signal_add(&wls->surface->events.map, &ls->map);
  wl_signal_add(&wls->surface->events.unmap, &ls->unmap);
  wl_signal_add(&wls->events.destroy, &ls->destroy);
  wl_list_insert(&s->layer_surfaces, &ls->link);
}

/* ── XDG shell — shared view map/unmap helpers ───────────────────────────── */

static void view_apply_rules(TrixieServer *s, TrixieView *v, Pane *p,
                              const char *app_id, const char *title) {
  (void)v;
  for(int i = 0; i < s->cfg.win_rule_count; i++) {
    WinRule    *r     = &s->cfg.win_rules[i];
    bool        match = false;
    const char *pat   = r->app_id;
    if(!strncmp(pat, "title:", 6))
      match = strstr(title, pat + 6) != NULL;
    else
      match = pat[0] && strstr(app_id, pat) != NULL;
    if(!match) continue;
    if(r->float_rule) {
      p->floating = true;
      if(rect_empty(p->float_rect)) p->float_rect = p->rect;
    }
    if(r->fullscreen_rule) p->fullscreen = true;
    if(r->forced_ws >= 1 && r->forced_ws <= s->twm.ws_count)
      twm_move_to_ws(&s->twm, r->forced_ws - 1);
    if(r->forced_w > 0 && r->forced_h > 0) {
      p->float_rect.w = r->forced_w;
      p->float_rect.h = r->forced_h;
      if(r->forced_x == 0 && r->forced_y == 0) {
        p->float_rect.x = (s->twm.screen_w - r->forced_w) / 2;
        p->float_rect.y = (s->twm.screen_h - r->forced_h) / 2;
      }
      p->floating = true;
    }
    if(r->forced_x != 0 || r->forced_y != 0) {
      p->float_rect.x = r->forced_x;
      p->float_rect.y = r->forced_y;
    }
  }
}

/* Apply window rules and open animations for a newly mapped view.
 * Works for both XDG and XWayland views; caller provides app_id and title. */
static void view_do_map(
    TrixieServer *s, TrixieView *v, Pane *p, const char *app_id, const char *title) {
  view_apply_rules(s, v, p, app_id, title);
  twm_reflow(&s->twm);

  struct wlr_scene_tree *tgt =
      (p->floating || p->fullscreen) ? s->layer_floating : s->layer_windows;
  wlr_scene_node_reparent(&v->scene_tree->node, tgt);
  wlr_scene_node_raise_to_top(&v->scene_tree->node);

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

  view_apply_geom(s, v, p);

  if(s->foreign_toplevel_mgr) {
    v->foreign_handle =
        wlr_foreign_toplevel_handle_v1_create(s->foreign_toplevel_mgr);
    v->foreign_request_activate.notify = handle_foreign_toplevel_request_activate;
    v->foreign_request_close.notify    = handle_foreign_toplevel_request_close;
    wl_signal_add(&v->foreign_handle->events.request_activate,
                  &v->foreign_request_activate);
    wl_signal_add(&v->foreign_handle->events.request_close,
                  &v->foreign_request_close);
    view_foreign_toplevel_update(s, v);
  }

  server_focus_pane(s, v->pane_id);
  lua_apply_window_rules(s, p, app_id, title);
  lua_on_window_open(s, v->pane_id);
  server_request_redraw(s);
}

static void view_handle_map(struct wl_listener *listener, void *data) {
  (void)data;
  TrixieView   *v = CONTAINER_OF(listener, TrixieView, map);
  TrixieServer *s = v->server;
  v->mapped       = true;
  Pane *p         = twm_pane_by_id(&s->twm, v->pane_id);
  if(!p) return;
  const char *app_id = v->xdg_toplevel->app_id ? v->xdg_toplevel->app_id : "";
  const char *title  = v->xdg_toplevel->title ? v->xdg_toplevel->title : "";
  view_do_map(s, v, p, app_id, title);
}

static void view_handle_unmap(struct wl_listener *listener, void *data) {
  (void)data;
  TrixieView   *v = CONTAINER_OF(listener, TrixieView, unmap);
  TrixieServer *s = v->server;
  v->mapped       = false;
  lua_on_window_close(s, v->pane_id);
  if(v->foreign_handle) {
    wlr_foreign_toplevel_handle_v1_destroy(v->foreign_handle);
    v->foreign_handle = NULL;
  }

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
    wl_list_for_each(next, &s->views, link) if(next != v && next->mapped) {
      server_focus_pane(s, next->pane_id);
      break;
    }
  }
}

static void view_handle_destroy(struct wl_listener *listener, void *data) {
  (void)data;
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
  (void)data;
  TrixieView   *v = CONTAINER_OF(listener, TrixieView, commit);
  TrixieServer *s = v->server;
  if(s->fractional_scale_mgr && v->xdg_toplevel && v->xdg_toplevel->base->surface) {
    TrixieOutput *o;
    wl_list_for_each(o, &s->outputs, link) wlr_fractional_scale_v1_notify_scale(
        v->xdg_toplevel->base->surface, o->wlr_output->scale);
  }
  /* Do NOT call server_request_redraw here.  wlroots schedules a frame
   * automatically when wlr_surface damage is committed.  Calling
   * schedule_frame redundantly from every surface commit doubles the
   * callback rate and is a primary cause of perceived sluggishness. */
}

static void view_handle_request_fullscreen(struct wl_listener *listener,
                                           void               *data) {
  (void)data;
  TrixieView *v = CONTAINER_OF(listener, TrixieView, request_fullscreen);
  Action      a = { .kind = ACTION_FULLSCREEN };
  server_dispatch_action(v->server, &a);
}

static void view_rescue_to_scratch(TrixieServer *s, TrixieView *v) {
  /* Called after twm_try_assign_scratch succeeds post-map.
   * The pane is already tiled — move its scene node to floating layer
   * and hide it, matching the hidden scratchpad state. */
  if(!v->mapped) return;
  Pane *p = twm_pane_by_id(&s->twm, v->pane_id);
  if(!p) return;

  /* twm_try_assign_scratch already removed it from the workspace pane list
   * and set floating + rect, so just fix up the scene node. */
  wlr_scene_node_reparent(&v->scene_tree->node, s->layer_floating);
  wlr_scene_node_set_enabled(&v->scene_tree->node, false);
  server_sync_focus(s);
  server_sync_windows(s);
  server_request_redraw(s);
}

static void view_handle_set_title(struct wl_listener *listener, void *data) {
  (void)data;
  TrixieView *v = CONTAINER_OF(listener, TrixieView, set_title);
  if(v->xdg_toplevel && v->xdg_toplevel->title) {
    const char *new_title = v->xdg_toplevel->title;
    Pane       *p         = twm_pane_by_id(&v->server->twm, v->pane_id);

    bool first_title = p && (p->title[0] == '\0' || strcmp(p->title, p->app_id) == 0);
    twm_set_title(&v->server->twm, v->pane_id, new_title);

    if(first_title && p && v->mapped) {
      const char *app_id = v->xdg_toplevel->app_id ? v->xdg_toplevel->app_id : "";
      bool was_float = p->floating;
      view_apply_rules(v->server, v, p, app_id, new_title);
      if(p->floating != was_float) {
        struct wlr_scene_tree *tgt =
            p->floating ? v->server->layer_floating : v->server->layer_windows;
        wlr_scene_node_reparent(&v->scene_tree->node, tgt);
        wlr_scene_node_raise_to_top(&v->scene_tree->node);
        view_apply_geom(v->server, v, p);
        server_reflow_with_morph(v->server);
      }
      lua_apply_window_rules(v->server, p, app_id, new_title);
    }

    bool claimed = twm_try_assign_scratch(&v->server->twm, v->pane_id, new_title);
    if(claimed && v->mapped)
      view_rescue_to_scratch(v->server, v);

    if(v->foreign_handle)
      wlr_foreign_toplevel_handle_v1_set_title(v->foreign_handle, new_title);
    ipc_push_title_changed(v->server, v->pane_id);
    lua_on_title_changed(v->server, v->pane_id);
    TrixieOutput *o;
    wl_list_for_each(o, &v->server->outputs, link) if(o->bar) bar_mark_dirty(o->bar);
  }
}

static void view_handle_set_app_id(struct wl_listener *listener, void *data) {
  (void)data;
  TrixieView *v = CONTAINER_OF(listener, TrixieView, set_app_id);
  if(v->xdg_toplevel && v->xdg_toplevel->app_id) {
    Pane *p = twm_pane_by_id(&v->server->twm, v->pane_id);
    if(p) {
      const char *new_id  = v->xdg_toplevel->app_id;
      bool        changed = strncmp(p->app_id, new_id, sizeof(p->app_id) - 1) != 0;
      strncpy(p->app_id, new_id, sizeof(p->app_id) - 1);
      p->app_id[sizeof(p->app_id) - 1] = '\0';

      if(changed && v->mapped) {
        const char *title = v->xdg_toplevel->title ? v->xdg_toplevel->title : "";
        bool was_float = p->floating;
        view_apply_rules(v->server, v, p, new_id, title);
        if(p->floating != was_float) {
          struct wlr_scene_tree *tgt =
              p->floating ? v->server->layer_floating : v->server->layer_windows;
          wlr_scene_node_reparent(&v->scene_tree->node, tgt);
          wlr_scene_node_raise_to_top(&v->scene_tree->node);
          view_apply_geom(v->server, v, p);
          server_reflow_with_morph(v->server);
        }
        lua_apply_window_rules(v->server, p, new_id, title);
      }
    }
    bool claimed = twm_try_assign_scratch(&v->server->twm, v->pane_id,
                                          v->xdg_toplevel->app_id);
    if(claimed && v->mapped)
      view_rescue_to_scratch(v->server, v);
    if(v->foreign_handle)
      wlr_foreign_toplevel_handle_v1_set_app_id(v->foreign_handle,
                                                v->xdg_toplevel->app_id);
  }
}

static void handle_new_xdg_surface(struct wl_listener *listener, void *data) {
  TrixieServer            *s        = CONTAINER_OF(listener, TrixieServer, new_xdg_surface);
  struct wlr_xdg_toplevel *toplevel = data;
  struct wlr_xdg_surface  *surface  = toplevel->base;

  const char *app_id = toplevel->app_id ? toplevel->app_id : "";

  bool init_float      = false;
  bool init_fullscreen = false;
  for(int i = 0; i < s->cfg.win_rule_count; i++) {
    WinRule    *r     = &s->cfg.win_rules[i];
    const char *pat   = r->app_id;
    bool        match = false;
    if(!strncmp(pat, "title:", 6)) {
      /* Title not available yet — skip; view_do_map re-applies at map time. */
    } else {
      match = pat[0] && strstr(app_id, pat) != NULL;
    }
    if(!match) continue;
    if(r->float_rule || (r->forced_w > 0 && r->forced_h > 0)) init_float = true;
    if(r->fullscreen_rule) init_fullscreen = true;
  }

  PaneId pane_id = twm_open_ex(&s->twm, app_id, init_float, init_fullscreen);
  if(pane_id == 0) {
    wlr_log(WLR_ERROR, "MAX_PANES hit — closing incoming surface");
    wlr_xdg_toplevel_send_close(toplevel);
    return;
  }

  /* Advertise compositor capabilities before the first configure.
   * GTK4/libadwaita reads this to decide chrome layout and button visibility. */
  wlr_xdg_toplevel_set_wm_capabilities(toplevel,
      WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE |
      WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN);
  wlr_xdg_toplevel_set_activated(toplevel, false);
  wlr_xdg_toplevel_set_tiled(
      toplevel, WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);

  TrixieView *v     = calloc(1, sizeof(*v));
  v->server         = s;
  v->xdg_toplevel   = toplevel;
  v->pane_id        = pane_id;
  v->mapped         = false;
  v->foreign_handle = NULL;
#ifdef HAS_XWAYLAND
  v->is_xwayland      = false;
  v->xwayland_surface = NULL;
#endif

  v->scene_tree = wlr_scene_xdg_surface_create(s->layer_windows, surface);
  wlr_scene_node_set_enabled(&v->scene_tree->node, false);

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

static void handle_new_deco(struct wl_listener *listener, void *data) {
  (void)listener;
  struct wlr_xdg_toplevel_decoration_v1 *deco = data;
  wlr_xdg_toplevel_decoration_v1_set_mode(
      deco, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * XWayland — full view lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifdef HAS_XWAYLAND
#include <wlr/xwayland.h>

/* Called when an XWayland surface is first drawn (equivalent to xdg map). */
static void xwayland_handle_map(struct wl_listener *listener, void *data) {
  (void)data;
  TrixieView   *v = CONTAINER_OF(listener, TrixieView, map);
  TrixieServer *s = v->server;
  v->mapped       = true;

  struct wlr_xwayland_surface *xs = v->xwayland_surface;
  Pane                        *p  = twm_pane_by_id(&s->twm, v->pane_id);
  if(!p) return;

  const char *app_id = xs->class ? xs->class : "";
  const char *title  = xs->title ? xs->title : "";

  twm_set_title(&s->twm, v->pane_id, title);
  strncpy(p->app_id, app_id, sizeof(p->app_id) - 1);

  /* Attempt scratchpad claim now that we have app_id */
  twm_try_assign_scratch(&s->twm, v->pane_id, app_id);

  /* XWayland override-redirect surfaces are unmanaged popups — just place. */
  if(xs->override_redirect) {
    wlr_scene_node_set_position(&v->scene_tree->node, xs->x, xs->y);
    wlr_scene_node_set_enabled(&v->scene_tree->node, true);
    return;
  }

  view_do_map(s, v, p, app_id, title);
}

static void xwayland_handle_unmap(struct wl_listener *listener, void *data) {
  (void)data;
  TrixieView   *v = CONTAINER_OF(listener, TrixieView, unmap);
  TrixieServer *s = v->server;
  v->mapped       = false;

  if(v->foreign_handle) {
    wlr_foreign_toplevel_handle_v1_destroy(v->foreign_handle);
    v->foreign_handle = NULL;
  }

  struct wlr_xwayland_surface *xs = v->xwayland_surface;
  if(xs->override_redirect) {
    wlr_scene_node_set_enabled(&v->scene_tree->node, false);
    return;
  }

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
    wl_list_for_each(next, &s->views, link) if(next != v && next->mapped) {
      server_focus_pane(s, next->pane_id);
      break;
    }
  }
}

static void xwayland_handle_destroy(struct wl_listener *listener, void *data) {
  (void)data;
  TrixieView   *v = CONTAINER_OF(listener, TrixieView, destroy);
  TrixieServer *s = v->server;
  twm_close(&s->twm, v->pane_id);
  wl_list_remove(&v->map.link);
  wl_list_remove(&v->unmap.link);
  wl_list_remove(&v->destroy.link);
  wl_list_remove(&v->commit.link);
  wl_list_remove(&v->request_fullscreen.link);
  wl_list_remove(&v->set_title.link);
  wl_list_remove(&v->set_app_id.link);
  wl_list_remove(&v->link);
  free(v);
  server_request_redraw(s);
}

static void xwayland_handle_commit(struct wl_listener *listener, void *data) {
  (void)listener;
  (void)data;
  /* wlroots schedules the frame automatically on surface damage — no-op. */
}

static void xwayland_handle_request_fullscreen(struct wl_listener *listener,
                                               void               *data) {
  (void)data;
  TrixieView *v = CONTAINER_OF(listener, TrixieView, request_fullscreen);
  Action      a = { .kind = ACTION_FULLSCREEN };
  server_dispatch_action(v->server, &a);
}

static void xwayland_handle_set_title(struct wl_listener *listener, void *data) {
  (void)data;
  TrixieView   *v = CONTAINER_OF(listener, TrixieView, set_title);
  TrixieServer *s = v->server;
  const char   *t = v->xwayland_surface->title ? v->xwayland_surface->title : "";
  twm_set_title(&s->twm, v->pane_id, t);
  if(v->foreign_handle)
    wlr_foreign_toplevel_handle_v1_set_title(v->foreign_handle, t);

  /* Add this: retry scratch assignment on title change */
  twm_try_assign_scratch(&s->twm, v->pane_id, t);

  TrixieOutput *o;
  wl_list_for_each(o, &s->outputs, link) if(o->bar) bar_mark_dirty(o->bar);
}

static void xwayland_handle_set_class(struct wl_listener *listener, void *data) {
  (void)data;
  TrixieView   *v = CONTAINER_OF(listener, TrixieView, set_app_id);
  TrixieServer *s = v->server;
  const char   *c = v->xwayland_surface->class ? v->xwayland_surface->class : "";
  twm_try_assign_scratch(&s->twm, v->pane_id, c);
  if(v->foreign_handle)
    wlr_foreign_toplevel_handle_v1_set_app_id(v->foreign_handle, c);
}

static void handle_new_xwayland_surface(struct wl_listener *listener, void *data) {
  TrixieServer *s = CONTAINER_OF(listener, TrixieServer, new_xwayland_surface);
  struct wlr_xwayland_surface *xs = data;

  const char *class = xs->class ? xs->class : "";

  /* Override-redirect: unmanaged popup — wrap in a scene tree but no TWM pane. */
  if(xs->override_redirect) {
    /* We still need to track it so it's visible.  Use a lightweight view with
     * pane_id=0 — it won't be managed but will be rendered. */
    TrixieView *v       = calloc(1, sizeof(*v));
    v->server           = s;
    v->is_xwayland      = true;
    v->xwayland_surface = xs;
    v->pane_id          = 0; /* unmanaged */
    v->mapped           = false;
    v->foreign_handle   = NULL;
    v->scene_tree       = wlr_scene_xwayland_surface_create(s->layer_floating, xs);
    wlr_scene_node_set_enabled(&v->scene_tree->node, false);
    /* Minimal listeners for unmanaged surfaces */
    v->map.notify                = xwayland_handle_map;
    v->unmap.notify              = xwayland_handle_unmap;
    v->destroy.notify            = xwayland_handle_destroy;
    v->commit.notify             = xwayland_handle_commit;
    /* Stub out the rest so remove is safe */
    v->request_fullscreen.notify = xwayland_handle_request_fullscreen;
    v->set_title.notify          = xwayland_handle_set_title;
    v->set_app_id.notify         = xwayland_handle_set_class;
    wl_signal_add(&xs->surface->events.map, &v->map);
    wl_signal_add(&xs->surface->events.unmap, &v->unmap);
    wl_signal_add(&xs->events.destroy, &v->destroy);
    wl_signal_add(&xs->surface->events.commit, &v->commit);
    wl_signal_add(&xs->events.request_fullscreen, &v->request_fullscreen);
    wl_signal_add(&xs->events.set_title, &v->set_title);
    wl_signal_add(&xs->events.set_class, &v->set_app_id);
    wl_list_insert(&s->views, &v->link);
    return;
  }

  /* Pre-scan float rules so the pane is born with the right classification. */
  bool xw_float = false, xw_full = false;
  for(int i = 0; i < s->cfg.win_rule_count; i++) {
    WinRule *r = &s->cfg.win_rules[i];
    if(!r->app_id[0] || strncmp(r->app_id, "title:", 6) == 0) continue;
    if(!strstr(class, r->app_id)) continue;
    if(r->float_rule || (r->forced_w > 0 && r->forced_h > 0)) xw_float = true;
    if(r->fullscreen_rule) xw_full = true;
  }
  PaneId pane_id = twm_open_ex(&s->twm, class, xw_float, xw_full);

  /* BUGFIX: pane pool overflow — refuse the surface. */
  if(pane_id == 0) {
    wlr_log(WLR_ERROR, "MAX_PANES hit — closing XWayland surface class=%s", class);
    wlr_xwayland_surface_close(xs);
    return;
  }

  TrixieView *v       = calloc(1, sizeof(*v));
  v->server           = s;
  v->is_xwayland      = true;
  v->xwayland_surface = xs;
  v->xdg_toplevel     = NULL;
  v->pane_id          = pane_id;
  v->mapped           = false;
  v->foreign_handle   = NULL;

  v->scene_tree = wlr_scene_xwayland_surface_create(s->layer_windows, xs);
  wlr_scene_node_set_enabled(&v->scene_tree->node, false);

  v->map.notify                = xwayland_handle_map;
  v->unmap.notify              = xwayland_handle_unmap;
  v->destroy.notify            = xwayland_handle_destroy;
  v->commit.notify             = xwayland_handle_commit;
  v->request_fullscreen.notify = xwayland_handle_request_fullscreen;
  v->set_title.notify          = xwayland_handle_set_title;
  v->set_app_id.notify         = xwayland_handle_set_class;

  wl_signal_add(&xs->surface->events.map, &v->map);
  wl_signal_add(&xs->surface->events.unmap, &v->unmap);
  wl_signal_add(&xs->events.destroy, &v->destroy);
  wl_signal_add(&xs->surface->events.commit, &v->commit);
  wl_signal_add(&xs->events.request_fullscreen, &v->request_fullscreen);
  wl_signal_add(&xs->events.set_title, &v->set_title);
  wl_signal_add(&xs->events.set_class, &v->set_app_id);
  wl_list_insert(&s->views, &v->link);

  wlr_log(WLR_INFO, "xwayland: new surface class=%s pane=%u", class, pane_id);
}

static void handle_xwayland_ready(struct wl_listener *listener, void *data) {
  (void)data;
  TrixieServer *s = CONTAINER_OF(listener, TrixieServer, xwayland_ready);
  wlr_log(WLR_INFO, "XWayland ready on display %s", s->xwayland->display_name);
  setenv("DISPLAY", s->xwayland->display_name, true);
  struct wlr_xcursor *xc =
      wlr_xcursor_manager_get_xcursor(s->xcursor_mgr, "default", 1);
  if(xc)
    wlr_xwayland_set_cursor(s->xwayland,
                            xc->images[0]->buffer,
                            xc->images[0]->width * 4,
                            xc->images[0]->width,
                            xc->images[0]->height,
                            xc->images[0]->hotspot_x,
                            xc->images[0]->hotspot_y);
}
#endif /* HAS_XWAYLAND */

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

static void write_all_fd(int fd, const char *buf, size_t len) {
  while(len > 0) {
    ssize_t n = write(fd, buf, len);
    if(n <= 0) {
      if(errno == EINTR) continue;
      break;
    }
    buf += n;
    len -= (size_t)n;
  }
}

static int ipc_read_cb(int fd, uint32_t mask, void *data) {
  (void)mask;
  TrixieServer *s      = data;
  int           client = accept(fd, NULL, NULL);
  if(client < 0) return 0;
  fcntl(client, F_SETFL, O_NONBLOCK);
  char    buf[65536] = { 0 };
  size_t  total      = 0;
  ssize_t n;
  while(total < sizeof(buf) - 1) {
    n = read(client, buf + total, sizeof(buf) - 1 - total);
    if(n > 0) {
      total += (size_t)n;
      continue;
    }
    if(n < 0 && errno == EAGAIN) break;
    break;
  }
  if(total > 0) {
    n        = (ssize_t)total;
    char *nl = strchr(buf, '\n');
    if(nl) *nl = '\0';
    /* Special case: subscribe command keeps the fd open for event pushing. */
    char *cmd_start = buf;
    while(*cmd_start == ' ' || *cmd_start == '\t')
      cmd_start++;
    if(strncmp(cmd_start, "subscribe", 9) == 0 &&
       (cmd_start[9] == '\0' || cmd_start[9] == ' ' || cmd_start[9] == '\t')) {
      if(ipc_subscribe(s, client)) {
        /* fd is now owned by the subscriber list — do NOT close it here */
        return 0;
      } else {
        const char *err = "err: subscriber table full\n";
        write_all_fd(client, err, strlen(err));
        close(client);
        return 0;
      }
    }
    /* Special case: binary_reload triggers a ninja rebuild + execv.
     * Handled here rather than in ipc_dispatch because it lives in main.c. */
    if(strncmp(cmd_start, "binary_reload", 13) == 0 &&
       (cmd_start[13] == '\0' || cmd_start[13] == '\n')) {
      server_binary_reload(s);
      write_all_fd(client, "ok: building\n", 13);
      close(client);
      return 0;
    }
    char reply[65536] = { 0 };
    ipc_dispatch(s, buf, reply, sizeof(reply));
    strncat(reply, "\n", sizeof(reply) - strlen(reply) - 1);
    write_all_fd(client, reply, strlen(reply));
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
  (void)mask;
  static int64_t last_reload_ms = 0;
  TrixieServer  *s              = data;
  char    buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
  ssize_t n = read(fd, buf, sizeof(buf));
  if(n <= 0) return 0;
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  int64_t now_ms = now.tv_sec * 1000 + now.tv_nsec / 1000000;
  if(now_ms - last_reload_ms < 200) return 0;
  last_reload_ms = now_ms;
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
  if(inotify_add_watch(s->inotify_fd, dir, IN_MODIFY | IN_CREATE | IN_MOVED_TO) <
     0) {
    close(s->inotify_fd);
    s->inotify_fd = -1;
    return;
  }
  s->inotify_src = wl_event_loop_add_fd(wl_display_get_event_loop(s->display),
                                        s->inotify_fd,
                                        WL_EVENT_READABLE,
                                        inotify_cb,
                                        s);
  wlr_log(WLR_INFO, "watching %s for config changes", dir);
}

/* ── Main ─────────────────────────────────────────────────────────────────── */

static void crash_handler(int sig) {
  void *bt[64];
  int   n = backtrace(bt, 64);
  fprintf(stderr, "\n=== CRASH sig=%d ===\n", sig);
  backtrace_symbols_fd(bt, n, STDERR_FILENO);
  fprintf(stderr, "===================\n");
  fflush(stderr);
  signal(sig, SIG_DFL);
  raise(sig);
}

int main(int argc, char *argv[]) {
  (void)argc;
  setenv("WLR_RENDERER", "gles2", false);
  wlr_log_init(WLR_INFO, NULL);
  signal(SIGSEGV, crash_handler);
  signal(SIGABRT, crash_handler);
  signal(SIGBUS, crash_handler);
  TrixieServer *s = calloc(1, sizeof(*s));
  wl_list_init(&s->views);
  wl_list_init(&s->outputs);
  wl_list_init(&s->keyboards);
  wl_list_init(&s->layer_surfaces);
  s->running        = true;
  s->ipc_fd         = -1;
  s->inotify_fd     = -1;
  s->reload_pid     = -1;
  s->reload_pipe_fd = -1;
  s->saved_argv     = argv;

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

  /* Step 2: start async bar worker pool before any output is created. */
  bar_workers_init(&s->bar_workers, &s->cfg);

  /* Step 2b: init Lua scripting layer — loads ~/.config/trixie/init.lua
   * after the C config so Lua can override any setting. */
  lua_init(s);

  twm_init(&s->twm,
           1920,
           1080,
           s->cfg.bar.height,
           s->cfg.bar.position == BAR_BOTTOM,
           s->cfg.gap,
           s->cfg.border_width,
           s->cfg.outer_gap,
           s->cfg.workspaces,
           s->cfg.smart_gaps);
  for(int i = 0; i < s->cfg.scratchpad_count; i++) {
    ScratchpadCfg *sp = &s->cfg.scratchpads[i];
    twm_register_scratch(
        &s->twm, sp->name, sp->app_id, sp->exec, sp->width_pct, sp->height_pct);
  }
  anim_set_resize(&s->anim, 1920, 1080);

  s->display = wl_display_create();
  s->backend =
      wlr_backend_autocreate(wl_display_get_event_loop(s->display), &s->session);
  if(!s->backend) {
    wlr_log(WLR_ERROR, "failed to create backend");
    return 1;
  }

  s->renderer = wlr_renderer_autocreate(s->backend);
  wlr_renderer_init_wl_display(s->renderer, s->display);
  s->allocator = wlr_allocator_autocreate(s->backend, s->renderer);

  wlr_compositor_create(s->display, 6, s->renderer);
  wlr_subcompositor_create(s->display);
  wlr_data_device_manager_create(s->display);

  /* Extended protocols */
  wlr_data_control_manager_v1_create(s->display);
  wlr_primary_selection_v1_device_manager_create(s->display);
  s->presentation = wlr_presentation_create(s->display, s->backend);
  wlr_viewporter_create(s->display);
  s->fractional_scale_mgr = wlr_fractional_scale_manager_v1_create(s->display, 1);
  wlr_single_pixel_buffer_manager_v1_create(s->display);

  /* wp-cursor-shape-v1 — apps request named cursors without a wl_surface */
  s->cursor_shape_mgr            = wlr_cursor_shape_manager_v1_create(s->display, 1);
  s->cursor_shape_request.notify = handle_cursor_shape_request;
  wl_signal_add(&s->cursor_shape_mgr->events.request_set_shape,
                &s->cursor_shape_request);

  s->xdg_activation                = wlr_xdg_activation_v1_create(s->display);
  s->xdg_activation_request.notify = handle_xdg_activation;
  wl_signal_add(&s->xdg_activation->events.request_activate,
                &s->xdg_activation_request);

  s->pointer_constraints           = wlr_pointer_constraints_v1_create(s->display);
  s->new_pointer_constraint.notify = handle_pointer_constraint;
  wl_signal_add(&s->pointer_constraints->events.new_constraint,
                &s->new_pointer_constraint);

  s->relative_pointer_mgr = wlr_relative_pointer_manager_v1_create(s->display);
  s->foreign_toplevel_mgr = wlr_foreign_toplevel_manager_v1_create(s->display);
  s->screencopy_mgr       = wlr_screencopy_manager_v1_create(s->display);
  wlr_export_dmabuf_manager_v1_create(s->display);
  wlr_gamma_control_manager_v1_create(s->display);

  /* ── Idle-inhibit protocol (zwp_idle_inhibit_manager_v1) ──────────────── *
   * Clients like video players call this to suppress DPMS blanking.         *
   * We respect it by pausing/resuming the idle timer.                       */
  s->idle_inhibit_mgr = wlr_idle_inhibit_v1_create(s->display);
  if(s->idle_inhibit_mgr) {
    s->idle_inhibit_new.notify = handle_new_idle_inhibitor;
    wl_signal_add(&s->idle_inhibit_mgr->events.new_inhibitor,
                  &s->idle_inhibit_new);
  }
  wlr_content_type_manager_v1_create(s->display, 1);
  wlr_tearing_control_manager_v1_create(s->display, 1);

  s->output_layout = wlr_output_layout_create(s->display);
  s->scene         = wlr_scene_create();
  s->scene_layout  = wlr_scene_attach_output_layout(s->scene, s->output_layout);

  s->layer_background      = wlr_scene_tree_create(&s->scene->tree);
  s->layer_windows         = wlr_scene_tree_create(&s->scene->tree);
  s->layer_chrome          = wlr_scene_tree_create(&s->scene->tree);
  s->layer_floating        = wlr_scene_tree_create(&s->scene->tree);
  s->layer_chrome_floating = wlr_scene_tree_create(&s->scene->tree);
  s->layer_overlay         = wlr_scene_tree_create(&s->scene->tree);

  /* Root background rect — sits at the bottom of layer_background.
   * Covers the full screen (resized in handle_new_output / config reload).
   * Color comes from cfg.colors.background — defaults to pane_bg if unset. */
  {
    Color bc    = s->cfg.colors.background;
    float fc[4] = { bc.r / 255.0f, bc.g / 255.0f, bc.b / 255.0f, bc.a / 255.0f };
    s->bg_rect  = wlr_scene_rect_create(s->layer_background, 1920, 1080, fc);
    wlr_scene_node_set_position(&s->bg_rect->node, 0, 0);
  }

  s->xdg_shell              = wlr_xdg_shell_create(s->display, 6);
  s->new_xdg_surface.notify = handle_new_xdg_surface;
  wl_signal_add(&s->xdg_shell->events.new_toplevel, &s->new_xdg_surface);
  s->new_xdg_popup.notify = handle_new_xdg_popup;
  wl_signal_add(&s->xdg_shell->events.new_popup, &s->new_xdg_popup);

  s->layer_shell              = wlr_layer_shell_v1_create(s->display, 4);
  s->new_layer_surface.notify = handle_new_layer_surface;
  wl_signal_add(&s->layer_shell->events.new_surface, &s->new_layer_surface);

  s->output_mgr = wlr_xdg_output_manager_v1_create(s->display, s->output_layout);

  s->deco_mgr        = wlr_xdg_decoration_manager_v1_create(s->display);
  s->new_deco.notify = handle_new_deco;
  wl_signal_add(&s->deco_mgr->events.new_toplevel_decoration, &s->new_deco);
  s->srv_deco = wlr_server_decoration_manager_create(s->display);
  wlr_server_decoration_manager_set_default_mode(
      s->srv_deco, WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);

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

  s->pointer_gestures = wlr_pointer_gestures_v1_create(s->display);
  gesture_tracker_init(&s->gesture);

  s->swipe_begin.notify  = handle_swipe_begin;
  s->swipe_update.notify = handle_swipe_update;
  s->swipe_end.notify    = handle_swipe_end;
  s->pinch_begin.notify  = handle_pinch_begin;
  s->pinch_update.notify = handle_pinch_update;
  s->pinch_end.notify    = handle_pinch_end;

  s->seat                       = wlr_seat_create(s->display, s->cfg.seat_name);
  s->seat_request_cursor.notify = handle_request_cursor;
  s->seat_request_set_selection.notify = handle_request_set_selection;
  s->seat_request_set_primary_selection.notify =
      handle_request_set_primary_selection;
  s->seat_request_start_drag.notify = handle_request_start_drag;
  s->seat_start_drag.notify         = handle_start_drag;
  wl_signal_add(&s->seat->events.request_set_cursor, &s->seat_request_cursor);
  wl_signal_add(&s->seat->events.request_set_selection,
                &s->seat_request_set_selection);
  wl_signal_add(&s->seat->events.request_set_primary_selection,
                &s->seat_request_set_primary_selection);
  wl_signal_add(&s->seat->events.request_start_drag, &s->seat_request_start_drag);
  wl_signal_add(&s->seat->events.start_drag,         &s->seat_start_drag);

  s->new_output.notify = handle_new_output;
  wl_signal_add(&s->backend->events.new_output, &s->new_output);
  s->new_input.notify = handle_new_input;
  wl_signal_add(&s->backend->events.new_input, &s->new_input);

  const char *socket = wl_display_add_socket_auto(s->display);
  if(!socket) {
    wlr_log(WLR_ERROR, "failed to create Wayland socket");
    return 1;
  }
  if(!wlr_backend_start(s->backend)) {
    wlr_log(WLR_ERROR, "failed to start backend");
    return 1;
  }

  setenv("WAYLAND_DISPLAY", socket, true);
  setenv("XDG_SESSION_TYPE", "wayland", true);
  setenv("XDG_CURRENT_DESKTOP", "trixie", true);
  /* Qt */
  setenv("QT_QPA_PLATFORM", "wayland", true);
  setenv("QT_WAYLAND_DISABLE_WINDOWDECORATION", "1", true);
  setenv("QT_AUTO_SCREEN_SCALE_FACTOR", "1", true);
  /* GTK */
  setenv("GDK_BACKEND", "wayland", true);
  /* SDL */
  setenv("SDL_VIDEODRIVER", "wayland", true);
  /* Java / LWJGL — required for AWT and GLFW on Wayland */
  setenv("_JAVA_AWT_WM_NONREPARENTING", "1", true);
  /* Clutter / GStreamer */
  setenv("CLUTTER_BACKEND", "wayland", true);
  /* Firefox */
  setenv("MOZ_ENABLE_WAYLAND", "1", true);
  setenv("MOZ_WEBRENDER", "1", true);
  setenv("MOZ_DBUS_REMOTE", "1", true);
  /* Electron / Chromium */
  setenv("ELECTRON_OZONE_PLATFORM_HINT", "wayland", true);
  setenv("NIXOS_OZONE_WL", "1", true);

  /* ── Nvidia proprietary driver EGL fix ───────────────────────────────────
   * The diagnostic confirms: RTX 4050, nvidia 590.48, DISPLAY=(not set).
   *
   * Nvidia's libEGL probes for an X11 display before trying Wayland. When
   * DISPLAY is unset it logs an error internally and fails to initialise the
   * Wayland EGL platform, so GLFW/LWJGL abort at eglGetDisplay() even though
   * every required Wayland protocol is present.
   *
   * Fixes:
   *  1. EGL_PLATFORM=wayland  — forces Mesa/Nvidia EGL to skip the X11 probe
   *     entirely and go straight to the Wayland platform.
   *  2. DISPLAY=:0 (dummy)    — satisfies any residual X11 probes in older
   *     nvidia EGL versions that check the var before honouring EGL_PLATFORM.
   *     Only set when XWayland is disabled; if XWayland is running it sets
   *     DISPLAY itself to its real socket in handle_xwayland_ready.
   *  3. GBM_BACKEND / __GLX_VENDOR_LIBRARY_NAME — ensure wlroots and Mesa
   *     use the nvidia GBM implementation rather than falling back to llvmpipe.
   *     These are no-ops on non-nvidia systems.
   * ─────────────────────────────────────────────────────────────────────── */
  setenv("EGL_PLATFORM", "wayland", true);
#ifndef HAS_XWAYLAND
  if(!getenv("DISPLAY") || getenv("DISPLAY")[0] == '\0')
    setenv("DISPLAY", ":0", false);
#endif
  setenv("GBM_BACKEND", "nvidia-drm", false);
  setenv("__GLX_VENDOR_LIBRARY_NAME", "nvidia", false);
  /* Nvidia GBM requires this to not assert on buffer import */
  setenv("WLR_NO_HARDWARE_CURSORS", "1", false);

  wlr_log(WLR_INFO, "WAYLAND_DISPLAY=%s", socket);

  server_init_ipc(s);
  server_init_config_watch(s);

  /* Expose server pointer for marvin_panel / overlay_exports */
  extern TrixieServer *g_server;
  g_server = s;

  if(s->cfg.idle_timeout > 0) {
    s->idle_timeout_ms = s->cfg.idle_timeout * 1000;
    s->idle_timer      = wl_event_loop_add_timer(
        wl_display_get_event_loop(s->display), idle_timer_cb, s);
    wl_event_source_timer_update(s->idle_timer, s->idle_timeout_ms);
  }

#ifdef HAS_XWAYLAND
  if(s->cfg.xwayland) {
    s->xwayland = wlr_xwayland_create(s->display, s->compositor, true);
    if(s->xwayland) {
      s->xwayland_ready.notify       = handle_xwayland_ready;
      s->new_xwayland_surface.notify = handle_new_xwayland_surface;
      wl_signal_add(&s->xwayland->events.ready, &s->xwayland_ready);
      wl_signal_add(&s->xwayland->events.new_surface, &s->new_xwayland_surface);
      setenv("DISPLAY", s->xwayland->display_name, true);
    }
  }
#endif
  s->nvim_timer = wl_event_loop_add_timer(
      wl_display_get_event_loop(s->display), nvim_connect_timer_cb, s);
  if(s->nvim_timer) wl_event_source_timer_update(s->nvim_timer, 2000);

  /* exec_once — unchanged */
  if(!s->exec_once_done) {
    s->exec_once_done = true;
    for(int i = 0; i < s->cfg.exec_once_count; i++)
      server_spawn(s, s->cfg.exec_once[i]);
  }
  for(int i = 0; i < s->cfg.exec_count; i++)
    server_spawn(s, s->cfg.exec[i]);

  wl_display_run(s->display);

  /* Cleanup — remove timer HERE, after the loop exits */
  if(s->nvim_timer) {
    wl_event_source_remove(s->nvim_timer);
    s->nvim_timer = NULL;
  }
  nvim_disconnect();

  /* Cleanup */
  wlr_log(WLR_INFO, "trixie shutting down");
  lua_destroy(s);                    /* close Lua before threads stop */
  bar_workers_stop(&s->bar_workers); /* Step 2: join worker threads cleanly */
  if(s->idle_timer) wl_event_source_remove(s->idle_timer);
  if(s->ipc_src) wl_event_source_remove(s->ipc_src);
  if(s->inotify_src) wl_event_source_remove(s->inotify_src);
  if(s->ipc_fd >= 0) {
    close(s->ipc_fd);
    unlink(ipc_socket_path());
  }
  if(s->inotify_fd >= 0) close(s->inotify_fd);
#ifdef HAS_XWAYLAND
  if(s->xwayland) wlr_xwayland_destroy(s->xwayland);
#endif
  wl_display_destroy_clients(s->display);
  wlr_backend_destroy(s->backend);
  wlr_xcursor_manager_destroy(s->xcursor_mgr);
  wlr_cursor_destroy(s->cursor);
  wlr_output_layout_destroy(s->output_layout);
  wl_display_destroy(s->display);
  free(s);
  return 0;
}
