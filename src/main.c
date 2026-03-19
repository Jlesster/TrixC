/* main.c — Trixie compositor */
#define _POSIX_C_SOURCE 200809L
#include "trixie.h"
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <libinput.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wlr/backend/libinput.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_content_type_v1.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_drm.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#if __has_include(<wlr/types/wlr_alpha_modifier_v1.h>)
#include <wlr/types/wlr_alpha_modifier_v1.h>
#define HAVE_ALPHA_MODIFIER 1
#endif
#if __has_include(<wlr/types/wlr_linux_drm_syncobj_v1.h>)
#include <wlr/backend/drm.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#define HAVE_DRM_SYNCOBJ 1
#endif
#if __has_include(<wlr/types/wlr_text_input_v3.h>)
#include <wlr/types/wlr_text_input_v3.h>
#define HAVE_TEXT_INPUT_V3 1
#endif
#if __has_include(<wlr/types/wlr_input_method_v2.h>)
#include <wlr/types/wlr_input_method_v2.h>
#define HAVE_INPUT_METHOD_V2 1
#endif
#if __has_include(<wlr/types/wlr_output_management_v1.h>)
#include <wlr/types/wlr_output_management_v1.h>
#define HAVE_OUTPUT_MANAGEMENT_V1 1
#endif
#if __has_include(<wlr/types/wlr_output_power_management_v1.h>)
#include <wlr/types/wlr_output_power_management_v1.h>
#define HAVE_OUTPUT_POWER_MGMT_V1 1
#endif
#if __has_include(<wlr/types/wlr_ext_idle_notify_v1.h>)
#include <wlr/types/wlr_ext_idle_notify_v1.h>
#define HAVE_EXT_IDLE_NOTIFY_V1 1
#endif
#if __has_include(<wlr/types/wlr_security_context_v1.h>)
#include <wlr/types/wlr_security_context_v1.h>
#define HAVE_SECURITY_CONTEXT_V1 1
#endif
#if __has_include(<wlr/types/wlr_virtual_keyboard_v1.h>)
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#define HAVE_VIRTUAL_KEYBOARD_V1 1
#endif
#if __has_include(<wlr/types/wlr_virtual_pointer_v1.h>)
#include <wlr/types/wlr_virtual_pointer_v1.h>
#define HAVE_VIRTUAL_POINTER_V1 1
#endif
#if __has_include(<wlr/types/wlr_session_lock_manager_v1.h>)
#include <wlr/types/wlr_session_lock_manager_v1.h>
#define HAVE_SESSION_LOCK_V1 1
#endif
#if __has_include(<wlr/types/wlr_xdg_foreign_registry.h>)
#include <wlr/types/wlr_xdg_foreign_registry.h>
#include <wlr/types/wlr_xdg_foreign_v1.h>
#include <wlr/types/wlr_xdg_foreign_v2.h>
#define HAVE_XDG_FOREIGN 1
#endif
#include "shader.h"
#include <wlr/render/gles2.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_pointer_gestures_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_tearing_control_v1.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_shell.h>

#define CONTAINER_OF(ptr, type, member)                                        \
  ((type *)((char *)(ptr) - offsetof(type, member)))

TrixieServer *g_server = NULL;

/* ── Forward declarations ───────────────────────────────────────────────────
 */
static void handle_new_output(struct wl_listener *, void *);
static void handle_new_input(struct wl_listener *, void *);
static void handle_new_xdg_surface(struct wl_listener *, void *);
static void handle_new_layer_surface(struct wl_listener *, void *);
static void handle_new_deco(struct wl_listener *, void *);
static void handle_new_idle_inhibitor(struct wl_listener *, void *);
static void handle_cursor_motion(struct wl_listener *, void *);
static void handle_cursor_motion_abs(struct wl_listener *, void *);
static void handle_cursor_button(struct wl_listener *, void *);
static void handle_cursor_axis(struct wl_listener *, void *);
static void handle_cursor_frame(struct wl_listener *, void *);
static void handle_request_cursor(struct wl_listener *, void *);
static void handle_request_set_selection(struct wl_listener *, void *);
static void handle_request_set_primary_selection(struct wl_listener *, void *);
static void handle_request_start_drag(struct wl_listener *, void *);
static void handle_start_drag(struct wl_listener *, void *);
static void view_handle_map(struct wl_listener *, void *);
static void view_handle_unmap(struct wl_listener *, void *);
static void view_handle_destroy(struct wl_listener *, void *);
static void view_handle_commit(struct wl_listener *, void *);
static void view_handle_request_fullscreen(struct wl_listener *, void *);
static void view_handle_set_title(struct wl_listener *, void *);
static void view_handle_set_app_id(struct wl_listener *, void *);
static void output_handle_frame(struct wl_listener *, void *);
static void output_handle_request_state(struct wl_listener *, void *);
static void output_handle_destroy(struct wl_listener *, void *);
static void keyboard_handle_key(struct wl_listener *, void *);
static void keyboard_handle_modifiers(struct wl_listener *, void *);
static void keyboard_handle_destroy(struct wl_listener *, void *);
static void server_reflow_with_morph(TrixieServer *s);
void server_mark_deco_dirty(TrixieServer *s);

/* ── Bar inset statics ───────────────────────────────────────────────────────
 */
static int s_bar_top_h = 0;
static int s_bar_bot_h = 0;

/* ── Gesture handlers ───────────────────────────────────────────────────────
 */
static double s_swipe_dx = 0, s_swipe_dy = 0;
static int s_swipe_fingers = 0;
static double s_pinch_scale = 1.0;
static int s_pinch_fingers = 0;

static void handle_swipe_begin(struct wl_listener *l, void *data) {
  TrixieServer *s = CONTAINER_OF(l, TrixieServer, swipe_begin);
  struct wlr_pointer_swipe_begin_event *ev = data;
  s_swipe_dx = 0;
  s_swipe_dy = 0;
  s_swipe_fingers = (int)ev->fingers;
  gesture_swipe_begin(&s->gesture, &s->cfg.gestures, s, (int)ev->fingers);
  if (s->pointer_gestures)
    wlr_pointer_gestures_v1_send_swipe_begin(s->pointer_gestures, s->seat,
                                             ev->time_msec, ev->fingers);
}
static void handle_swipe_update(struct wl_listener *l, void *data) {
  TrixieServer *s = CONTAINER_OF(l, TrixieServer, swipe_update);
  struct wlr_pointer_swipe_update_event *ev = data;
  s_swipe_dx += ev->dx;
  s_swipe_dy += ev->dy;
  gesture_swipe_update(&s->gesture, &s->cfg.gestures, s, ev->dx, ev->dy);
  if (s->pointer_gestures)
    wlr_pointer_gestures_v1_send_swipe_update(s->pointer_gestures, s->seat,
                                              ev->time_msec, ev->dx, ev->dy);
}
static void handle_swipe_end(struct wl_listener *l, void *data) {
  TrixieServer *s = CONTAINER_OF(l, TrixieServer, swipe_end);
  struct wlr_pointer_swipe_end_event *ev = data;
  gesture_swipe_end(&s->gesture, &s->cfg.gestures, s, ev->cancelled);
  if (!ev->cancelled && (s_swipe_dx != 0.0 || s_swipe_dy != 0.0)) {
    /* Determine dominant direction */
    const char *dir;
    if (fabs(s_swipe_dx) >= fabs(s_swipe_dy))
      dir = s_swipe_dx > 0 ? "right" : "left";
    else
      dir = s_swipe_dy > 0 ? "down" : "up";
    char spec[64];
    snprintf(spec, sizeof(spec), "swipe:%d:%s", s_swipe_fingers, dir);
    lua_dispatch_gesture(s, spec);
  }
  if (s->pointer_gestures)
    wlr_pointer_gestures_v1_send_swipe_end(s->pointer_gestures, s->seat,
                                           ev->time_msec, ev->cancelled);
}
static void handle_pinch_begin(struct wl_listener *l, void *data) {
  TrixieServer *s = CONTAINER_OF(l, TrixieServer, pinch_begin);
  struct wlr_pointer_pinch_begin_event *ev = data;
  s_pinch_scale = 1.0;
  s_pinch_fingers = (int)ev->fingers;
  gesture_pinch_begin(&s->gesture, &s->cfg.gestures, s, (int)ev->fingers);
  if (s->pointer_gestures)
    wlr_pointer_gestures_v1_send_pinch_begin(s->pointer_gestures, s->seat,
                                             ev->time_msec, ev->fingers);
}
static void handle_pinch_update(struct wl_listener *l, void *data) {
  TrixieServer *s = CONTAINER_OF(l, TrixieServer, pinch_update);
  struct wlr_pointer_pinch_update_event *ev = data;
  s_pinch_scale = ev->scale;
  gesture_pinch_update(&s->gesture, &s->cfg.gestures, s, ev->scale);
  if (s->pointer_gestures)
    wlr_pointer_gestures_v1_send_pinch_update(s->pointer_gestures, s->seat,
                                              ev->time_msec, ev->dx, ev->dy,
                                              ev->scale, ev->rotation);
}
static void handle_pinch_end(struct wl_listener *l, void *data) {
  TrixieServer *s = CONTAINER_OF(l, TrixieServer, pinch_end);
  struct wlr_pointer_pinch_end_event *ev = data;
  gesture_pinch_end(&s->gesture, &s->cfg.gestures, s, ev->cancelled);
  if (!ev->cancelled) {
    char spec[64];
    snprintf(spec, sizeof(spec), "pinch:%d:%s", s_pinch_fingers,
             s_pinch_scale < 0.9 ? "in" : "out");
    lua_dispatch_gesture(s, spec);
  }
  if (s->pointer_gestures)
    wlr_pointer_gestures_v1_send_pinch_end(s->pointer_gestures, s->seat,
                                           ev->time_msec, ev->cancelled);
}

/* ── Protocol handlers ──────────────────────────────────────────────────────
 */
static void handle_xdg_activation(struct wl_listener *listener, void *data) {
  TrixieServer *s = wl_container_of(listener, s, xdg_activation_request);
  struct wlr_xdg_activation_v1_request_activate_event *ev = data;
  if (ev->token && ev->token->seat != s->seat)
    return;
  TrixieView *v;
  wl_list_for_each(v, &s->views, link) {
    if (!v->mapped)
      continue;
    struct wlr_surface *surf = NULL;
#ifdef HAS_XWAYLAND
    if (v->is_xwayland && v->xwayland_surface)
      surf = v->xwayland_surface->surface;
    else
#endif
        if (v->xdg_toplevel)
      surf = v->xdg_toplevel->base->surface;
    if (surf == ev->surface) {
      twm_set_focused(&s->twm, v->pane_id);
      server_focus_pane(s, v->pane_id);
      server_request_redraw(s);
      return;
    }
  }
}

static void handle_pointer_constraint(struct wl_listener *listener,
                                      void *data) {
  (void)listener;
  wlr_pointer_constraint_v1_send_activated(data);
}

static void handle_cursor_shape_request(struct wl_listener *listener,
                                        void *data) {
  TrixieServer *s = CONTAINER_OF(listener, TrixieServer, cursor_shape_request);
  struct wlr_cursor_shape_manager_v1_request_set_shape_event *ev = data;
  if (ev->seat_client != s->seat->pointer_state.focused_client)
    return;
  const char *shape = wlr_cursor_shape_v1_name(ev->shape);
  wlr_cursor_set_xcursor(s->cursor, s->xcursor_mgr, shape ? shape : "default");
}

/* ── Foreign toplevel ───────────────────────────────────────────────────────
 */
static void view_foreign_toplevel_update(TrixieServer *s, TrixieView *v) {
  if (!s->foreign_toplevel_mgr || !v->foreign_handle)
    return;
  Pane *p = twm_pane_by_id(&s->twm, v->pane_id);
  if (!p)
    return;
  wlr_foreign_toplevel_handle_v1_set_title(v->foreign_handle, p->title);
  wlr_foreign_toplevel_handle_v1_set_app_id(v->foreign_handle, p->app_id);
  wlr_foreign_toplevel_handle_v1_set_activated(
      v->foreign_handle, twm_focused_id(&s->twm) == v->pane_id);
  wlr_foreign_toplevel_handle_v1_set_fullscreen(v->foreign_handle,
                                                p->fullscreen);
  wlr_foreign_toplevel_handle_v1_set_maximized(v->foreign_handle, false);
  wlr_foreign_toplevel_handle_v1_set_minimized(v->foreign_handle, false);
}
static void
handle_foreign_toplevel_request_activate(struct wl_listener *listener,
                                         void *data) {
  (void)data;
  TrixieView *v = wl_container_of(listener, v, foreign_request_activate);
  TrixieServer *s = v->server;
  twm_set_focused(&s->twm, v->pane_id);
  server_focus_pane(s, v->pane_id);
  server_request_redraw(s);
}
static void handle_foreign_toplevel_request_close(struct wl_listener *listener,
                                                  void *data) {
  (void)data;
  TrixieView *v = wl_container_of(listener, v, foreign_request_close);
#ifdef HAS_XWAYLAND
  if (v->is_xwayland && v->xwayland_surface) {
    wlr_xwayland_surface_close(v->xwayland_surface);
    return;
  }
#endif
  if (v->xdg_toplevel)
    wlr_xdg_toplevel_send_close(v->xdg_toplevel);
}

/* ── Idle inhibit ───────────────────────────────────────────────────────────
 */
typedef struct {
  TrixieServer *server;
  struct wl_listener destroy;
} IdleInhibitorCtx;
static void handle_idle_inhibitor_destroy(struct wl_listener *listener,
                                          void *data) {
  (void)data;
  IdleInhibitorCtx *ctx = wl_container_of(listener, ctx, destroy);
  ctx->server->idle_inhibit_count--;
  if (ctx->server->idle_inhibit_count <= 0) {
    ctx->server->idle_inhibit_count = 0;
    if (ctx->server->idle_timer)
      wl_event_source_timer_update(ctx->server->idle_timer,
                                   ctx->server->idle_timeout_ms);
  }
  wl_list_remove(&ctx->destroy.link);
  free(ctx);
}
static void handle_new_idle_inhibitor(struct wl_listener *listener,
                                      void *data) {
  TrixieServer *s = CONTAINER_OF(listener, TrixieServer, idle_inhibit_new);
  struct wlr_idle_inhibitor_v1 *inh = data;
  s->idle_inhibit_count++;
  if (s->idle_timer)
    wl_event_source_timer_update(s->idle_timer, 0);
  IdleInhibitorCtx *ctx = calloc(1, sizeof(*ctx));
  if (ctx) {
    ctx->server = s;
    ctx->destroy.notify = handle_idle_inhibitor_destroy;
    wl_signal_add(&inh->events.destroy, &ctx->destroy);
  }
}

void server_reset_idle(TrixieServer *s) {
  if (!s->idle_timer)
    return;
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

static int idle_timer_cb(void *data) {
  TrixieServer *s = data;
  if (s->idle_inhibit_count > 0)
    return 0;
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

/* ── Geometry helpers ───────────────────────────────────────────────────────
 */
static void view_apply_geom(TrixieServer *s, TrixieView *v, Pane *p) {
  int bw = (p->floating || p->fullscreen) ? 0 : s->twm.border_w;
  int cw = p->rect.w - bw * 2;
  if (cw < 1)
    cw = 1;
  int ch = p->rect.h - bw * 2;
  if (ch < 1)
    ch = 1;
#ifdef HAS_XWAYLAND
  if (v->is_xwayland && v->xwayland_surface) {
    wlr_xwayland_surface_configure(
        v->xwayland_surface, (int16_t)(p->rect.x + bw),
        (int16_t)(p->rect.y + bw), (uint16_t)cw, (uint16_t)ch);
    return;
  }
#endif
  /* Guard against scheduling a configure on an uninitialized xdg_surface.
   * Dolphin and some Qt apps emit map before their initial ack_configure,
   * causing wlroots to log "configure scheduled for uninitialized surface"
   * and crash. In wlroots 0.18 the correct check is initial_commit — true
   * means the surface hasn't completed its first commit yet. */
  if (!v->xdg_toplevel || !v->xdg_toplevel->base)
    return;
  if (v->xdg_toplevel->base->initial_commit)
    return;
  wlr_xdg_toplevel_set_size(v->xdg_toplevel, (uint32_t)cw, (uint32_t)ch);
}

/* Public wrapper used by lua.c client_geometry write */
void view_apply_geom_pub(TrixieServer *s, TrixieView *v, Pane *p) {
  view_apply_geom(s, v, p);
}

/* ── Spawn ──────────────────────────────────────────────────────────────────
 */
void server_spawn(TrixieServer *s, const char *cmd) {
  (void)s;
  if (!cmd || !cmd[0])
    return;
  wlr_log(WLR_INFO, "spawn: %s", cmd);
  pid_t pid = fork();
  if (pid < 0)
    return;
  if (pid == 0) {
    pid_t p2 = fork();
    if (p2 < 0)
      _exit(1);
    if (p2 == 0) {
      setsid();
      int maxfd = (int)sysconf(_SC_OPEN_MAX);
      for (int fd = 3; fd < maxfd; fd++)
        close(fd);
      if (!getenv("PATH"))
        setenv("PATH", "/usr/local/bin:/usr/bin:/bin", 1);
      /* Unset vars that are needed by the compositor/wlroots but break
       * client applications (especially Firefox and Electron):
       *
       * GBM_BACKEND=nvidia-drm    — correct for wlroots/GBM buffer alloc,
       *                             but breaks Firefox's own Mesa EGL path
       *                             which does not go through GBM.
       * __GLX_VENDOR_LIBRARY_NAME — forces libGLX_nvidia which is wrong for
       *                             Wayland clients that use EGL directly.
       * EGL_PLATFORM              — compositor needs this set before its own
       *                             EGL init; clients should auto-detect.
       *                             Firefox's glxtest forks without a
       * wl_display and crashes if forced to wayland platform. */
      unsetenv("GBM_BACKEND");
      unsetenv("__GLX_VENDOR_LIBRARY_NAME");
      unsetenv("EGL_PLATFORM");
      unsetenv("WLR_NO_HARDWARE_CURSORS");
      unsetenv("WLR_RENDERER");
      unsetenv("WLR_DRM_DEVICES");
      unsetenv("MOZ_WEBRENDER_FORCE");
      unsetenv("MOZ_X11_EGL"); /* conflicts with Wayland EGL on Nvidia */
      execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
      _exit(127);
    }
    _exit(0);
  }
  waitpid(pid, NULL, 0);
}

/* ── Focus ──────────────────────────────────────────────────────────────────
 */
TrixieView *view_from_pane(TrixieServer *s, PaneId id) {
  TrixieView *v;
  wl_list_for_each(v, &s->views,
                   link) if (v->pane_id == id && v->mapped) return v;
  return NULL;
}

void server_focus_pane(TrixieServer *s, PaneId id) {
  TrixieView *v = view_from_pane(s, id);
  if (!v)
    return;
  struct wlr_surface *surf = NULL;
#ifdef HAS_XWAYLAND
  if (v->is_xwayland && v->xwayland_surface) {
    surf = v->xwayland_surface->surface;
    wlr_xwayland_surface_activate(v->xwayland_surface, true);
  } else
#endif
      if (v->xdg_toplevel)
    surf = v->xdg_toplevel->base->surface;
  if (!surf)
    return;
  struct wlr_keyboard *kb = wlr_seat_get_keyboard(s->seat);
  wlr_scene_node_raise_to_top(&v->scene_tree->node);
  if (kb)
    wlr_seat_keyboard_notify_enter(s->seat, surf, kb->keycodes,
                                   kb->num_keycodes, &kb->modifiers);
  TrixieView *fv;
  wl_list_for_each(fv, &s->views, link) {
    if (fv->foreign_handle)
      wlr_foreign_toplevel_handle_v1_set_activated(fv->foreign_handle, fv == v);
#ifdef HAS_XWAYLAND
    if (fv->is_xwayland && fv->xwayland_surface && fv != v)
      wlr_xwayland_surface_activate(fv->xwayland_surface, false);
#endif
    if (!fv->is_xwayland && fv->xdg_toplevel && fv->mapped)
      wlr_xdg_toplevel_set_activated(fv->xdg_toplevel, fv == v);
  }
}

void server_mark_deco_dirty(TrixieServer *s) {
  TrixieOutput *o;
  wl_list_for_each(o, &s->outputs, link) o->deco_dirty = true;
  server_request_redraw(s);
}

/* ── Bar inset ──────────────────────────────────────────────────────────────
 */
static void server_apply_bar_insets(TrixieServer *s) {
  if (s_bar_bot_h > 0)
    twm_set_bar_height(&s->twm, s_bar_bot_h, true);
  else if (s_bar_top_h > 0)
    twm_set_bar_height(&s->twm, s_bar_top_h, false);
  else
    twm_set_bar_height(&s->twm, 0, false);
  server_sync_windows(s);
  server_mark_deco_dirty(s);
}

void server_set_bar_inset(TrixieServer *s, int h, bool at_bottom) {
  if (at_bottom)
    s_bar_bot_h = h;
  else
    s_bar_top_h = h;
  server_apply_bar_insets(s);
}

void server_sync_focus(TrixieServer *s) {
  PaneId id = twm_focused_id(&s->twm);
  if (id)
    server_focus_pane(s, id);
  ipc_push_focus_changed(s);
  lua_emit_focus(s);
  server_mark_deco_dirty(s);
}

/* ── Window sync ────────────────────────────────────────────────────────────
 */
void server_sync_windows(TrixieServer *s) {
  Workspace *ws = &s->twm.workspaces[s->twm.active_ws];
  int incoming_x = anim_ws_incoming_x(&s->anim);
  PaneId focused_id = twm_focused_id(&s->twm);
  TrixieView *v;
  wl_list_for_each(v, &s->views, link) {
    if (!v->mapped)
      continue;
    Pane *p = twm_pane_by_id(&s->twm, v->pane_id);
    if (!p)
      continue;

    /* Check if this pane is on the active workspace */
    bool on_ws = p->sticky;
    if (!on_ws)
      for (int i = 0; i < ws->pane_count; i++)
        if (ws->panes[i] == v->pane_id) {
          on_ws = true;
          break;
        }

    bool is_closing = anim_is_closing(&s->anim, v->pane_id);
    if (!on_ws && !is_closing) {
      wlr_scene_node_set_enabled(&v->scene_tree->node, false);
#ifdef HAS_XWAYLAND
      if (v->is_xwayland && v->xwayland_surface)
        wlr_xwayland_surface_activate(v->xwayland_surface, false);
#endif
      continue;
    }
    if (p->minimized) {
      wlr_scene_node_set_enabled(&v->scene_tree->node, false);
      continue;
    }
    wlr_scene_node_set_enabled(&v->scene_tree->node, true);
    Rect r = anim_get_rect(&s->anim, v->pane_id, p->rect);
    int bw = (p->floating || p->fullscreen) ? 0 : s->twm.border_w;
    int win_x = r.x + bw + incoming_x, win_y = r.y + bw;
    int win_w = r.w - bw * 2;
    if (win_w < 1)
      win_w = 1;
    int win_h = r.h - bw * 2;
    if (win_h < 1)
      win_h = 1;
    wlr_scene_node_set_position(&v->scene_tree->node, win_x, win_y);

    bool is_focused = (v->pane_id == focused_id);
    float opacity = (p->rule_opacity > 0.f) ? p->rule_opacity : 1.0f;
    if (opacity >= 1.0f && !is_focused && !p->floating && !p->fullscreen)
      opacity = 0.90f;
    if (p->floating || is_closing) {
      float fade = anim_get_opacity(&s->anim, v->pane_id, -1.f);
      if (fade >= 0.f)
        opacity *= fade;
    }
#ifdef WLR_SCENE_HAS_OPACITY
    wlr_scene_node_set_opacity(&v->scene_tree->node, opacity);
#else
    (void)opacity;
#endif
    if (!is_closing) {
      int cw = p->rect.w - bw * 2;
      if (cw < 1)
        cw = 1;
      int ch = p->rect.h - bw * 2;
      if (ch < 1)
        ch = 1;
#ifdef HAS_XWAYLAND
      if (v->is_xwayland && v->xwayland_surface) {
        struct wlr_xwayland_surface *xs = v->xwayland_surface;
        if (xs->width != cw || xs->height != ch)
          wlr_xwayland_surface_configure(xs, (int16_t)(p->rect.x + bw),
                                         (int16_t)(p->rect.y + bw),
                                         (uint16_t)cw, (uint16_t)ch);
      } else
#endif
          if (v->xdg_toplevel) {
        if (!v->xdg_toplevel->base->initial_commit) {
          struct wlr_box cur;
          wlr_xdg_surface_get_geometry(v->xdg_toplevel->base, &cur);
          if (cur.width != cw || cur.height != ch)
            wlr_xdg_toplevel_set_size(v->xdg_toplevel, cw, ch);
        }
      }
    }
  }
}

void server_request_redraw(TrixieServer *s) {
  TrixieOutput *o;
  wl_list_for_each(o, &s->outputs, link)
      wlr_output_schedule_frame(o->wlr_output);
}

/* ── Animated reflow ────────────────────────────────────────────────────────
 */
#define TWM_MORPH_SWAP(s_, call_)                                              \
  do {                                                                         \
    Workspace *_ws = &(s_)->twm.workspaces[(s_)->twm.active_ws];               \
    typedef struct {                                                           \
      PaneId id;                                                               \
      Rect r;                                                                  \
    } _Snap;                                                                   \
    _Snap _snaps[64];                                                          \
    int _nsn = 0;                                                              \
    for (int _i = 0; _i < _ws->pane_count && _nsn < 64; _i++) {                \
      Pane *_p = twm_pane_by_id(&(s_)->twm, _ws->panes[_i]);                   \
      if (_p && !_p->floating && !_p->fullscreen)                              \
        _snaps[_nsn++] = (_Snap){_p->id, _p->rect};                            \
    }                                                                          \
    (call_);                                                                   \
    for (int _i = 0; _i < _nsn; _i++) {                                        \
      Pane *_p = twm_pane_by_id(&(s_)->twm, _snaps[_i].id);                    \
      if (_p) {                                                                \
        anim_morph(&(s_)->anim, _snaps[_i].id, _snaps[_i].r, _p->rect);        \
        TrixieView *_v = view_from_pane((s_), _snaps[_i].id);                  \
        if (_v)                                                                \
          view_apply_geom((s_), _v, _p);                                       \
      }                                                                        \
    }                                                                          \
    server_sync_windows(s_);                                                   \
    server_mark_deco_dirty(s_);                                                \
    server_request_redraw(s_);                                                 \
  } while (0)

static void server_reflow_with_morph(TrixieServer *s) {
  Workspace *ws = &s->twm.workspaces[s->twm.active_ws];
  typedef struct {
    PaneId id;
    Rect r;
  } Snap;
  Snap snap[64];
  int snap_n = 0;
  for (int i = 0; i < ws->pane_count && snap_n < 64; i++) {
    Pane *p = twm_pane_by_id(&s->twm, ws->panes[i]);
    if (!p || p->floating || p->fullscreen)
      continue;
    snap[snap_n++] = (Snap){p->id, p->rect};
  }
  twm_reflow(&s->twm);
  for (int i = 0; i < snap_n; i++) {
    Pane *p = twm_pane_by_id(&s->twm, snap[i].id);
    if (!p)
      continue;
    anim_morph(&s->anim, snap[i].id, snap[i].r, p->rect);
    TrixieView *v = view_from_pane(s, snap[i].id);
    if (v)
      view_apply_geom(s, v, p);
  }
  server_sync_windows(s);
  server_mark_deco_dirty(s);
  server_request_redraw(s);
}

/* ── Action dispatch ────────────────────────────────────────────────────────
 */
void server_dispatch_action(TrixieServer *s, Action *a) {
  switch (a->kind) {
  case ACTION_QUIT:
    s->running = false;
    wl_display_terminate(s->display);
    break;
  case ACTION_RELOAD:
    server_apply_config_reload(s);
    break;
  case ACTION_EXEC:
    server_spawn(s, a->exec_cmd);
    break;
  case ACTION_CLOSE: {
    PaneId id = twm_focused_id(&s->twm);
    if (id) {
      TrixieView *v = view_from_pane(s, id);
      if (v) {
#ifdef HAS_XWAYLAND
        if (v->is_xwayland && v->xwayland_surface) {
          wlr_xwayland_surface_close(v->xwayland_surface);
          break;
        }
#endif
        if (v->xdg_toplevel)
          wlr_xdg_toplevel_send_close(v->xdg_toplevel);
      }
    }
    break;
  }
  case ACTION_FULLSCREEN: {
    PaneId id = twm_focused_id(&s->twm);
    Pane *p = id ? twm_pane_by_id(&s->twm, id) : NULL;
    if (p) {
      p->fullscreen = !p->fullscreen;
      TrixieView *v = view_from_pane(s, id);
      if (v) {
        struct wlr_scene_tree *tgt =
            p->fullscreen ? s->layer_floating : s->layer_windows;
        wlr_scene_node_reparent(&v->scene_tree->node, tgt);
        wlr_scene_node_raise_to_top(&v->scene_tree->node);
        if (v->foreign_handle)
          wlr_foreign_toplevel_handle_v1_set_fullscreen(v->foreign_handle,
                                                        p->fullscreen);
#ifndef HAS_XWAYLAND
        wlr_xdg_toplevel_set_fullscreen(v->xdg_toplevel, p->fullscreen);
#else
        if (!v->is_xwayland && v->xdg_toplevel)
          wlr_xdg_toplevel_set_fullscreen(v->xdg_toplevel, p->fullscreen);
#endif
      }
      server_reflow_with_morph(s);
    }
    break;
  }
  case ACTION_TOGGLE_FLOAT:
    server_float_toggle(s);
    break;
  case ACTION_TOGGLE_BAR:
    s->twm.bar_visible = !s->twm.bar_visible;
    {
      int h = (s_bar_bot_h > 0) ? s_bar_bot_h : s_bar_top_h;
      bool bot = (s_bar_bot_h > 0);
      server_set_bar_inset(s, s->twm.bar_visible ? h : 0, bot);
    }
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
    TWM_MORPH_SWAP(s, twm_swap_dir(&s->twm, -1, 0));
    break;
  case ACTION_MOVE_RIGHT:
    TWM_MORPH_SWAP(s, twm_swap_dir(&s->twm, 1, 0));
    break;
  case ACTION_MOVE_UP:
    TWM_MORPH_SWAP(s, twm_swap_dir(&s->twm, 0, -1));
    break;
  case ACTION_MOVE_DOWN:
    TWM_MORPH_SWAP(s, twm_swap_dir(&s->twm, 0, 1));
    break;
  case ACTION_SWAP_MAIN:
    TWM_MORPH_SWAP(s, twm_swap_main(&s->twm));
    break;
  case ACTION_WORKSPACE: {
    int old = s->twm.active_ws;
    twm_switch_ws(&s->twm, a->n - 1);
    int nw = s->twm.active_ws;
    if (nw != old) {
      anim_workspace_transition(&s->anim,
                                nw > old ? WS_DIR_RIGHT : WS_DIR_LEFT);
      ipc_push_workspace_changed(s);
      lua_emit_workspace_changed(s);
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
    if (s->twm.active_ws != old)
      anim_workspace_transition(&s->anim, WS_DIR_RIGHT);
    server_sync_focus(s);
    server_sync_windows(s);
    break;
  }
  case ACTION_PREV_WS: {
    int old = s->twm.active_ws;
    twm_switch_ws(&s->twm, (old + s->twm.ws_count - 1) % s->twm.ws_count);
    if (s->twm.active_ws != old)
      anim_workspace_transition(&s->anim, WS_DIR_LEFT);
    server_sync_focus(s);
    server_sync_windows(s);
    break;
  }
  case ACTION_NEXT_LAYOUT:
    s->twm.workspaces[s->twm.active_ws].layout =
        layout_next(s->twm.workspaces[s->twm.active_ws].layout);
    server_reflow_with_morph(s);
    break;
  case ACTION_PREV_LAYOUT:
    s->twm.workspaces[s->twm.active_ws].layout =
        layout_prev(s->twm.workspaces[s->twm.active_ws].layout);
    server_reflow_with_morph(s);
    break;
  case ACTION_GROW_MAIN: {
    float *r = &s->twm.workspaces[s->twm.active_ws].main_ratio;
    *r = (*r + 0.05f > 0.9f) ? 0.9f : *r + 0.05f;
    server_reflow_with_morph(s);
    break;
  }
  case ACTION_SHRINK_MAIN: {
    float *r = &s->twm.workspaces[s->twm.active_ws].main_ratio;
    *r = (*r - 0.05f < 0.1f) ? 0.1f : *r - 0.05f;
    server_reflow_with_morph(s);
    break;
  }
  case ACTION_SCRATCHPAD:
    server_scratch_toggle(s, a->name);
    break;
  case ACTION_SWITCH_VT:
    if (s->session)
      wlr_session_change_vt(s->session, (unsigned)a->n);
    break;
  case ACTION_EMERGENCY_QUIT:
    s->running = false;
    wl_display_terminate(s->display);
    break;
  default:
    wlr_log(WLR_DEBUG, "unhandled action %d", a->kind);
    break;
  }
}

/* ── Float / scratch ────────────────────────────────────────────────────────
 */
void server_float_toggle(TrixieServer *s) {
  PaneId id = twm_focused_id(&s->twm);
  if (!id)
    return;
  Pane *p = twm_pane_by_id(&s->twm, id);
  if (!p)
    return;
  bool was_float = p->floating;
  twm_toggle_float(&s->twm);
  TrixieView *v = view_from_pane(s, id);
  if (v) {
    struct wlr_scene_tree *tgt =
        p->floating ? s->layer_floating : s->layer_windows;
    wlr_scene_node_reparent(&v->scene_tree->node, tgt);
    wlr_scene_node_raise_to_top(&v->scene_tree->node);
  }
  if (p->floating && !was_float)
    anim_float_open(&s->anim, id, p->rect);
  else if (!p->floating && was_float)
    anim_open(&s->anim, id, p->rect);
  server_sync_windows(s);
  server_sync_focus(s);
  server_mark_deco_dirty(s);
}

void server_scratch_toggle(TrixieServer *s, const char *name) {
  Scratchpad *sp = NULL;
  for (int i = 0; i < s->twm.scratch_count; i++)
    if (!strcmp(s->twm.scratchpads[i].name, name)) {
      sp = &s->twm.scratchpads[i];
      break;
    }
  if (!sp)
    return;
  if (!sp->has_pane)
    return; /* Lua scratchpad.lua handles spawning */
  bool was_visible = sp->visible;
  PaneId pid = sp->pane_id;
  twm_toggle_scratch(&s->twm, name);
  Pane *p = twm_pane_by_id(&s->twm, pid);
  if (p) {
    if (!was_visible && sp->visible)
      anim_scratch_open(&s->anim, pid, p->rect);
    else if (was_visible && !sp->visible)
      anim_scratch_close(&s->anim, pid, p->rect);
  }
  server_sync_windows(s);
  server_sync_focus(s);
  server_mark_deco_dirty(s);
}

/* ── Config reload ──────────────────────────────────────────────────────────
 */
void server_apply_config_reload(TrixieServer *s) {
  static bool reloading = false;
  if (reloading)
    return;
  reloading = true;

  /* Snapshot interactive layout/ratio — Lua can still override on reload */
  Layout layout_snap[MAX_WORKSPACES];
  float ratio_snap[MAX_WORKSPACES];
  int n = s->twm.ws_count;
  for (int i = 0; i < n; i++) {
    layout_snap[i] = s->twm.workspaces[i].layout;
    ratio_snap[i] = s->twm.workspaces[i].main_ratio;
  }

  /* Reset to C defaults; Lua will override everything via trixie.set() */
  config_defaults(&s->cfg);
  s->twm.gap = s->cfg.gap;
  s->twm.border_w = s->cfg.border_width;
  s->twm.smart_gaps = s->cfg.smart_gaps;
  s->twm.padding = s->cfg.outer_gap;

  if (s->bg_rect) {
    Color bc = s->cfg.colors.background;
    float fc[4] = {bc.r / 255.f, bc.g / 255.f, bc.b / 255.f, bc.a / 255.f};
    wlr_scene_rect_set_color(s->bg_rect, fc);
  }
  if (s->xcursor_mgr)
    wlr_xcursor_manager_load(s->xcursor_mgr, s->cfg.cursor_size);

  /* Restore interactive layout/ratio before Lua runs */
  int new_n = s->twm.ws_count;
  for (int i = 0; i < new_n && i < n; i++) {
    s->twm.workspaces[i].layout = layout_snap[i];
    s->twm.workspaces[i].main_ratio = ratio_snap[i];
  }

  TrixieKeyboard *kb;
  wl_list_for_each(kb, &s->keyboards, link) wlr_keyboard_set_repeat_info(
      kb->wlr_keyboard, s->cfg.keyboard.repeat_rate,
      s->cfg.keyboard.repeat_delay);

  if (s->idle_timer) {
    s->idle_timeout_ms = s->cfg.idle_timeout * 1000;
    wl_event_source_timer_update(
        s->idle_timer, s->idle_timeout_ms > 0 ? s->idle_timeout_ms : 0);
  }

  /* Reset bar insets — l_wibox re-registers from scratch */
  s_bar_top_h = 0;
  s_bar_bot_h = 0;

  /* Run init.lua — all trixie.set() and trixie.wibox() calls happen here */
  lua_reload(s);

  /* Post-Lua: pick up any remaining trixie.set() changes */
  twm_reflow(&s->twm);
  server_sync_windows(s);

  TrixieOutput *o;
  wl_list_for_each(o, &s->outputs, link) if (o->bar) bar_mark_dirty(o->bar);
  server_request_redraw(s);
  wlr_log(WLR_INFO, "config reloaded");
  reloading = false;
}

/* ── Binary hot-reload ──────────────────────────────────────────────────────
 */
static int reload_pipe_cb(int fd, uint32_t mask, void *data) {
  TrixieServer *s = data;
  char buf[2048];
  if (mask & WL_EVENT_READABLE) {
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n > 0) {
      buf[n] = '\0';
      wlr_log(WLR_INFO, "hot-reload: %s", buf);
    }
  }
  if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
    int status = 0;
    pid_t done = waitpid(s->reload_pid, &status, WNOHANG);
    if (s->reload_pipe_src) {
      wl_event_source_remove(s->reload_pipe_src);
      s->reload_pipe_src = NULL;
    }
    close(fd);
    s->reload_pipe_fd = -1;
    s->reload_pid = -1;
    int exit_code = (done > 0 && WIFEXITED(status)) ? WEXITSTATUS(status) : 1;
    if (exit_code != 0) {
      wlr_log(WLR_ERROR, "hot-reload: ninja failed (%d)", exit_code);
      return 0;
    }
    wlr_log(WLR_INFO, "hot-reload: build succeeded, exec-replacing…");
    wl_display_flush_clients(s->display);
    unsetenv("WAYLAND_DISPLAY");
    unsetenv("DISPLAY");
    wlr_backend_destroy(s->backend);
    s->backend = NULL;
    execv(s->reload_new_bin, s->saved_argv);
    wlr_log(WLR_ERROR, "hot-reload: execv failed: %s", strerror(errno));
  }
  return 0;
}

void server_binary_reload(TrixieServer *s) {
  if (s->reload_pid > 0)
    return;
  ssize_t len = readlink("/proc/self/exe", s->reload_new_bin,
                         sizeof(s->reload_new_bin) - 1);
  if (len <= 0)
    return;
  s->reload_new_bin[len] = '\0';
  char build_dir[512] = {0};
  if (s->cfg.build_dir[0]) {
    strncpy(build_dir, s->cfg.build_dir, sizeof(build_dir) - 1);
  } else {
    char self[512] = {0};
    if (readlink("/proc/self/exe", self, sizeof(self) - 1) > 0) {
      char *slash = strrchr(self, '/');
      if (slash) {
        *slash = '\0';
        strncpy(build_dir, self, sizeof(build_dir) - 1);
      }
    }
    if (!build_dir[0])
      strncpy(build_dir, ".", sizeof(build_dir) - 1);
  }
  int pipefd[2];
  if (pipe(pipefd) < 0)
    return;
  fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return;
  }
  if (pid == 0) {
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);
    if (chdir(build_dir) < 0)
      _exit(1);
    execlp("ninja", "ninja", (char *)NULL);
    _exit(127);
  }
  close(pipefd[1]);
  s->reload_pid = pid;
  s->reload_pipe_fd = pipefd[0];
  s->reload_pipe_src = wl_event_loop_add_fd(
      wl_display_get_event_loop(s->display), pipefd[0],
      WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR, reload_pipe_cb, s);
  wlr_log(WLR_INFO, "hot-reload: building in %s…", build_dir);
}

/* ── Schedule reload ────────────────────────────────────────────────────────
 */
static void deferred_reload_cb(void *data) {
  TrixieServer *s = data;
  if (s->reload_idle) {
    wl_event_source_remove(s->reload_idle);
    s->reload_idle = NULL;
  }
  server_apply_config_reload(s);
}
void server_schedule_reload(TrixieServer *s) {
  if (s->reload_idle)
    return;
  s->reload_idle = wl_event_loop_add_idle(wl_display_get_event_loop(s->display),
                                          deferred_reload_cb, s);
}

/* ── Keyboard ───────────────────────────────────────────────────────────────
 */
static uint32_t wlr_mods_to_trixie(uint32_t m) {
  uint32_t out = 0;
  if (m & WLR_MODIFIER_LOGO)
    out |= MOD_SUPER;
  if (m & WLR_MODIFIER_CTRL)
    out |= MOD_CTRL;
  if (m & WLR_MODIFIER_ALT)
    out |= MOD_ALT;
  if (m & WLR_MODIFIER_SHIFT)
    out |= MOD_SHIFT;
  return out;
}

static void keyboard_handle_key(struct wl_listener *listener, void *data) {
  TrixieKeyboard *kb = CONTAINER_OF(listener, TrixieKeyboard, key);
  TrixieServer *s = kb->server;
  struct wlr_keyboard_key_event *event = data;
  server_reset_idle(s);

  uint32_t keycode = event->keycode + 8;
  const xkb_keysym_t *syms;
  int nsyms =
      xkb_state_key_get_syms(kb->wlr_keyboard->xkb_state, keycode, &syms);
  const xkb_keysym_t *base_syms;
  int nbase = xkb_keymap_key_get_syms_by_level(kb->wlr_keyboard->keymap,
                                               keycode, 0, 0, &base_syms);

  bool handled = false;
  uint32_t mods =
      wlr_mods_to_trixie(wlr_keyboard_get_modifiers(kb->wlr_keyboard));

  if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
    /* Hard emergency exit: Super+Shift+Print */
    for (int i = 0; i < nsyms; i++)
      if (syms[i] == XKB_KEY_Print && (mods & MOD_SUPER) &&
          (mods & MOD_SHIFT)) {
        s->running = false;
        wl_display_terminate(s->display);
        return;
      }
    /* Lua binds — dispatched by sym and base sym (handles non-ASCII layouts) */
    for (int i = 0; i < nsyms && !handled; i++)
      if (lua_dispatch_key(s, mods, syms[i]))
        handled = true;
    for (int i = 0; i < nbase && !handled; i++)
      if (lua_dispatch_key(s, mods, base_syms[i]))
        handled = true;
  }

  if (!handled) {
    wlr_seat_set_keyboard(s->seat, kb->wlr_keyboard);
    wlr_seat_keyboard_notify_key(s->seat, event->time_msec, event->keycode,
                                 event->state);
  }
}

static void keyboard_handle_modifiers(struct wl_listener *listener,
                                      void *data) {
  (void)data;
  TrixieKeyboard *kb = CONTAINER_OF(listener, TrixieKeyboard, modifiers);
  wlr_seat_set_keyboard(kb->server->seat, kb->wlr_keyboard);
  wlr_seat_keyboard_notify_modifiers(kb->server->seat,
                                     &kb->wlr_keyboard->modifiers);
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
  TrixieKeyboard *kb = calloc(1, sizeof(*kb));
  kb->server = s;
  kb->wlr_keyboard = wlr_kb;
  struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  struct xkb_rule_names rules = {
      .layout = s->cfg.keyboard.kb_layout[0] ? s->cfg.keyboard.kb_layout : NULL,
      .variant =
          s->cfg.keyboard.kb_variant[0] ? s->cfg.keyboard.kb_variant : NULL,
      .options =
          s->cfg.keyboard.kb_options[0] ? s->cfg.keyboard.kb_options : NULL,
  };
  struct xkb_keymap *keymap =
      xkb_keymap_new_from_names(ctx, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);
  wlr_keyboard_set_keymap(wlr_kb, keymap);
  xkb_keymap_unref(keymap);
  xkb_context_unref(ctx);
  wlr_keyboard_set_repeat_info(wlr_kb, s->cfg.keyboard.repeat_rate,
                               s->cfg.keyboard.repeat_delay);
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
  if (wlr_input_device_is_libinput(dev)) {
    struct libinput_device *li = wlr_libinput_get_device_handle(dev);
    libinput_device_config_tap_set_enabled(li, LIBINPUT_CONFIG_TAP_ENABLED);
    libinput_device_config_scroll_set_natural_scroll_enabled(li, 1);
    libinput_device_config_tap_set_drag_enabled(li,
                                                LIBINPUT_CONFIG_DRAG_ENABLED);
    libinput_device_config_tap_set_button_map(li, LIBINPUT_CONFIG_TAP_MAP_LMR);
  }
  struct wlr_pointer *ptr = wlr_pointer_from_input_device(dev);
  s->swipe_begin.notify = handle_swipe_begin;
  wl_signal_add(&ptr->events.swipe_begin, &s->swipe_begin);
  s->swipe_update.notify = handle_swipe_update;
  wl_signal_add(&ptr->events.swipe_update, &s->swipe_update);
  s->swipe_end.notify = handle_swipe_end;
  wl_signal_add(&ptr->events.swipe_end, &s->swipe_end);
  s->pinch_begin.notify = handle_pinch_begin;
  wl_signal_add(&ptr->events.pinch_begin, &s->pinch_begin);
  s->pinch_update.notify = handle_pinch_update;
  wl_signal_add(&ptr->events.pinch_update, &s->pinch_update);
  s->pinch_end.notify = handle_pinch_end;
  wl_signal_add(&ptr->events.pinch_end, &s->pinch_end);
}

static void handle_new_input(struct wl_listener *listener, void *data) {
  TrixieServer *s = CONTAINER_OF(listener, TrixieServer, new_input);
  struct wlr_input_device *dev = data;
  switch (dev->type) {
  case WLR_INPUT_DEVICE_KEYBOARD:
    server_new_keyboard(s, dev);
    break;
  case WLR_INPUT_DEVICE_POINTER:
    server_new_pointer(s, dev);
    break;
  default:
    break;
  }
  uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
  if (!wl_list_empty(&s->keyboards))
    caps |= WL_SEAT_CAPABILITY_KEYBOARD;
  wlr_seat_set_capabilities(s->seat, caps);
}

/* ── Cursor ─────────────────────────────────────────────────────────────────
 */
static bool super_is_held(TrixieServer *s) {
  TrixieKeyboard *kb;
  wl_list_for_each(kb, &s->keyboards,
                   link) if (wlr_keyboard_get_modifiers(kb->wlr_keyboard) &
                             WLR_MODIFIER_LOGO) return true;
  return false;
}

static PaneId pane_at_cursor(TrixieServer *s, double lx, double ly) {
  int px = (int)lx, py = (int)ly;
  Workspace *ws = &s->twm.workspaces[s->twm.active_ws];
  for (int i = ws->pane_count - 1; i >= 0; i--) {
    Pane *p = twm_pane_by_id(&s->twm, ws->panes[i]);
    if (!p || !p->floating)
      continue;
    if (rect_contains(p->rect, px, py))
      return p->id;
  }
  for (int i = ws->pane_count - 1; i >= 0; i--) {
    Pane *p = twm_pane_by_id(&s->twm, ws->panes[i]);
    if (!p)
      continue;
    if (rect_contains(p->rect, px, py))
      return p->id;
  }
  return 0;
}

static void update_cursor_focus(TrixieServer *s, uint32_t time_msec) {
  if (s->drag_mode != DRAG_NONE)
    return;
  double lx = s->cursor->x, ly = s->cursor->y;
  struct wlr_surface *under = NULL;
  double sx, sy;
  /* wlroots scene hit-test */
  struct wlr_scene_node *node =
      wlr_scene_node_at(&s->scene->tree.node, lx, ly, &sx, &sy);
  if (node && node->type == WLR_SCENE_NODE_BUFFER) {
    struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(node);
    struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(sb);
    if (ss)
      under = ss->surface;
  }
  if (under) {
    wlr_seat_pointer_notify_enter(s->seat, under, sx, sy);
    wlr_seat_pointer_notify_motion(s->seat, time_msec, sx, sy);
  } else {
    wlr_cursor_set_xcursor(s->cursor, s->xcursor_mgr, "default");
    wlr_seat_pointer_clear_focus(s->seat);
  }
}

static void handle_cursor_motion(struct wl_listener *listener, void *data) {
  TrixieServer *s = CONTAINER_OF(listener, TrixieServer, cursor_motion);
  struct wlr_pointer_motion_event *ev = data;
  server_reset_idle(s);
  wlr_cursor_move(s->cursor, &ev->pointer->base, ev->delta_x, ev->delta_y);
  if (s->drag_mode == DRAG_MOVE) {
    Pane *p = twm_pane_by_id(&s->twm, s->drag_pane);
    if (p) {
      twm_float_move(&s->twm, s->drag_pane, (int)ev->delta_x, (int)ev->delta_y);
      server_sync_windows(s);
      server_request_redraw(s);
      return;
    }
  }
  if (s->drag_mode == DRAG_RESIZE) {
    Pane *p = twm_pane_by_id(&s->twm, s->drag_pane);
    if (p) {
      twm_float_resize(&s->twm, s->drag_pane, (int)ev->delta_x,
                       (int)ev->delta_y);
      server_sync_windows(s);
      server_request_redraw(s);
      return;
    }
  }
  update_cursor_focus(s, ev->time_msec);
  PaneId under = pane_at_cursor(s, s->cursor->x, s->cursor->y);
  if (under && under != twm_focused_id(&s->twm)) {
    twm_set_focused(&s->twm, under);
    server_sync_focus(s);
  }
}

static void handle_cursor_motion_abs(struct wl_listener *listener, void *data) {
  TrixieServer *s = CONTAINER_OF(listener, TrixieServer, cursor_motion_abs);
  struct wlr_pointer_motion_absolute_event *ev = data;
  server_reset_idle(s);
  wlr_cursor_warp_absolute(s->cursor, &ev->pointer->base, ev->x, ev->y);
  update_cursor_focus(s, ev->time_msec);
}

static void handle_cursor_button(struct wl_listener *listener, void *data) {
  TrixieServer *s = CONTAINER_OF(listener, TrixieServer, cursor_button);
  struct wlr_pointer_button_event *ev = data;
  server_reset_idle(s);
  bool pressed = (ev->state == WL_POINTER_BUTTON_STATE_PRESSED);

  /* Lua button bindings get first crack (without modifier → global binds) */
  uint32_t mods = 0;
  TrixieKeyboard *kb;
  wl_list_for_each(kb, &s->keyboards, link) mods |=
      wlr_keyboard_get_modifiers(kb->wlr_keyboard);
  /* Convert wlr mods */
  uint32_t lmods = 0;
  if (mods & WLR_MODIFIER_LOGO)
    lmods |= MOD_SUPER;
  if (mods & WLR_MODIFIER_CTRL)
    lmods |= MOD_CTRL;
  if (mods & WLR_MODIFIER_ALT)
    lmods |= MOD_ALT;
  if (mods & WLR_MODIFIER_SHIFT)
    lmods |= MOD_SHIFT;
  /* Map wlr button codes to 1/2/3 */
  uint32_t btn = ev->button == BTN_LEFT     ? 1
                 : ev->button == BTN_MIDDLE ? 2
                 : ev->button == BTN_RIGHT  ? 3
                                            : ev->button;
  if (lua_dispatch_button(s, lmods, btn, pressed))
    return;

  wlr_seat_pointer_notify_button(s->seat, ev->time_msec, ev->button, ev->state);
  if (!pressed) {
    s->drag_mode = DRAG_NONE;
    s->drag_pane = 0;
    return;
  }

  bool held = super_is_held(s);
  PaneId pid = pane_at_cursor(s, s->cursor->x, s->cursor->y);
  if (pid) {
    twm_set_focused(&s->twm, pid);
    server_sync_focus(s);
    Pane *p = twm_pane_by_id(&s->twm, pid);
    bool is_float = p && p->floating;
    if (held && ev->button == BTN_LEFT) {
      s->drag_mode = is_float ? DRAG_MOVE : DRAG_RESIZE;
      s->drag_pane = pid;
      anim_cancel(&s->anim, pid);
      return;
    }
    if (held && ev->button == BTN_RIGHT) {
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
  wlr_seat_pointer_notify_axis(s->seat, ev->time_msec, ev->orientation,
                               ev->delta, ev->delta_discrete, ev->source,
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
  if (s->seat->pointer_state.focused_client == ev->seat_client)
    wlr_cursor_set_surface(s->cursor, ev->surface, ev->hotspot_x,
                           ev->hotspot_y);
}
static void handle_request_set_selection(struct wl_listener *listener,
                                         void *data) {
  TrixieServer *s =
      CONTAINER_OF(listener, TrixieServer, seat_request_set_selection);
  struct wlr_seat_request_set_selection_event *ev = data;
  wlr_seat_set_selection(s->seat, ev->source, ev->serial);
}
static void handle_request_set_primary_selection(struct wl_listener *listener,
                                                 void *data) {
  TrixieServer *s =
      CONTAINER_OF(listener, TrixieServer, seat_request_set_primary_selection);
  struct wlr_seat_request_set_primary_selection_event *ev = data;
  wlr_seat_set_primary_selection(s->seat, ev->source, ev->serial);
}

/* ── XDG Popups ─────────────────────────────────────────────────────────────
 */
static void handle_new_xdg_popup(struct wl_listener *listener, void *data) {
  TrixieServer *s = CONTAINER_OF(listener, TrixieServer, new_xdg_popup);
  struct wlr_xdg_popup *popup = data;
  struct wlr_scene_tree *parent_tree = s->layer_floating;
  if (popup->parent) {
    struct wlr_xdg_surface *px =
        wlr_xdg_surface_try_from_wlr_surface(popup->parent);
    if (px && px->data)
      parent_tree = px->data;
  }
  struct wlr_scene_tree *tree =
      wlr_scene_xdg_surface_create(parent_tree, popup->base);
  if (!tree)
    return;
  popup->base->data = tree;
  struct wlr_output *out =
      wlr_output_layout_output_at(s->output_layout, s->cursor->x, s->cursor->y);
  if (out) {
    struct wlr_box ob;
    wlr_output_layout_get_box(s->output_layout, out, &ob);
    wlr_xdg_popup_unconstrain_from_box(popup, &ob);
  }
  wlr_scene_node_raise_to_top(&tree->node);
}

/* ── Drag and drop ──────────────────────────────────────────────────────────
 */
static struct wlr_scene_tree *g_drag_icon_tree = NULL;
typedef struct {
  struct wl_listener destroy;
} DragIconCtx;
static void drag_icon_handle_destroy(struct wl_listener *l, void *data) {
  (void)data;
  DragIconCtx *ctx = wl_container_of(l, ctx, destroy);
  g_drag_icon_tree = NULL;
  wl_list_remove(&ctx->destroy.link);
  free(ctx);
}
static void handle_request_start_drag(struct wl_listener *listener,
                                      void *data) {
  TrixieServer *s =
      CONTAINER_OF(listener, TrixieServer, seat_request_start_drag);
  struct wlr_seat_request_start_drag_event *ev = data;
  if (wlr_seat_validate_pointer_grab_serial(s->seat, ev->origin, ev->serial))
    wlr_seat_start_pointer_drag(s->seat, ev->drag, ev->serial);
  else
    wlr_data_source_destroy(ev->drag->source);
}
static void handle_start_drag(struct wl_listener *listener, void *data) {
  TrixieServer *s = CONTAINER_OF(listener, TrixieServer, seat_start_drag);
  struct wlr_drag *drag = data;
  if (!drag->icon)
    return;
  struct wlr_scene_tree *t =
      wlr_scene_drag_icon_create(s->layer_overlay, drag->icon);
  if (!t)
    return;
  wlr_scene_node_set_position(&t->node, (int)s->cursor->x, (int)s->cursor->y);
  g_drag_icon_tree = t;
  DragIconCtx *ctx = calloc(1, sizeof(*ctx));
  if (ctx) {
    ctx->destroy.notify = drag_icon_handle_destroy;
    wl_signal_add(&drag->icon->events.destroy, &ctx->destroy);
  }
}

/* ── Output frame ───────────────────────────────────────────────────────────
 */
static void output_handle_frame(struct wl_listener *listener, void *data) {
  (void)data;
  TrixieOutput *o = CONTAINER_OF(listener, TrixieOutput, frame);
  TrixieServer *s = o->server;

  bool still_animating = anim_tick(&s->anim);
  bool need_sync = still_animating || o->was_animating;
  o->was_animating = still_animating;
  if (need_sync)
    server_sync_windows(s);

  bool bar_dirty = false;
  for (int wi = 0; wi < o->wibox_count; wi++) {
    TrixieWibox *wb = o->wiboxes[wi];
    if (!wb || !wb->visible || !wb->scene_buf)
      continue;
    if (!wb->dirty)
      continue;
    if (wb->lua_draw_ref != LUA_NOREF)
      wibox_lua_draw(wb, s->L); /* draw fn marks dirty=true then commits */
    else
      wibox_commit(wb); /* manual canvas path */
    bar_dirty = true;
  }

  if (o->deco_dirty) {
    deco_mark_dirty(o->deco);
    o->deco_dirty = false;
  }
  deco_update(o->deco, &s->twm, &s->anim, &s->cfg);

  float sat = s->cfg.saturation;
  bool use_shader = s->cfg.shader_enabled && sat != 1.0f &&
                    wlr_renderer_is_gles2(s->renderer) && o->shader.ready;
  if (use_shader) {
    shader_render_frame(&o->shader, s->renderer, o->scene_output, o->wlr_output,
                        sat);
  } else {
    wlr_scene_output_commit(o->scene_output, NULL);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(o->scene_output, &now);
  }
  if (s->running && (still_animating || bar_dirty))
    wlr_output_schedule_frame(o->wlr_output);
}

static void output_handle_request_state(struct wl_listener *listener,
                                        void *data) {
  TrixieOutput *o = CONTAINER_OF(listener, TrixieOutput, request_state);
  struct wlr_output_event_request_state *ev = data;
  wlr_output_commit_state(o->wlr_output, ev->state);
  int nw = o->wlr_output->width, nh = o->wlr_output->height;
  if (nw > 0 && nh > 0 && (nw != o->shader.width || nh != o->shader.height))
    shader_output_resize(&o->shader, nw, nh);
}

static void output_handle_destroy(struct wl_listener *listener, void *data) {
  (void)data;
  TrixieOutput *o = CONTAINER_OF(listener, TrixieOutput, destroy);
  TrixieServer *s = o->server;
  lua_emit_screen_removed(s, o);
  wl_list_remove(&o->frame.link);
  wl_list_remove(&o->request_state.link);
  wl_list_remove(&o->destroy.link);
  wl_list_remove(&o->link);
  if (o->bar)
    bar_destroy(o->bar);
  for (int wi = 0; wi < o->wibox_count; wi++)
    wibox_destroy(o->wiboxes[wi]);
  o->wibox_count = 0;
  if (o->deco)
    deco_destroy(o->deco);
  shader_output_finish(&o->shader);
  free(o);
  if (!wl_list_empty(&s->outputs)) {
    TrixieOutput *next = wl_container_of(s->outputs.next, next, link);
    twm_resize(&s->twm, next->width, next->height);
    anim_set_resize(&s->anim, next->width, next->height);
    server_sync_windows(s);
  }
}

static void handle_new_output(struct wl_listener *listener, void *data) {
  TrixieServer *s = CONTAINER_OF(listener, TrixieServer, new_output);
  struct wlr_output *wlr_output = data;
  wlr_output_init_render(wlr_output, s->allocator, s->renderer);

  struct wlr_output_state state;
  wlr_output_state_init(&state);
  wlr_output_state_set_enabled(&state, true);
  wlr_output_state_set_adaptive_sync_enabled(&state, true);
  struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
  if (mode)
    wlr_output_state_set_mode(&state, mode);
  wlr_output_commit_state(wlr_output, &state);
  wlr_output_state_finish(&state);

  TrixieOutput *o = calloc(1, sizeof(*o));
  o->server = s;
  o->wlr_output = wlr_output;
  o->frame.notify = output_handle_frame;
  wl_signal_add(&wlr_output->events.frame, &o->frame);
  o->request_state.notify = output_handle_request_state;
  wl_signal_add(&wlr_output->events.request_state, &o->request_state);
  o->destroy.notify = output_handle_destroy;
  wl_signal_add(&wlr_output->events.destroy, &o->destroy);
  wl_list_insert(&s->outputs, &o->link);

  struct wlr_output_layout_output *lo =
      wlr_output_layout_add_auto(s->output_layout, wlr_output);
  o->scene_output = wlr_scene_output_create(s->scene, wlr_output);
  wlr_scene_output_layout_add_output(s->scene_layout, lo, o->scene_output);

  int ow = mode ? mode->width : wlr_output->width;
  int oh = mode ? mode->height : wlr_output->height;
  o->width = ow;
  o->height = oh;
  o->logical_w = ow;
  o->logical_h = oh;

  TrixieOutput *primary = wl_container_of(s->outputs.next, primary, link);
  if (primary == o) {
    twm_resize(&s->twm, ow, oh);
    anim_set_resize(&s->anim, ow, oh);
  }

  o->bar = bar_create(s->layer_overlay, ow, oh, &s->cfg);
  o->deco = deco_create(s->layer_chrome, s->layer_chrome_floating);
  if (s->cfg.shader_enabled && wlr_renderer_is_gles2(s->renderer))
    shader_output_init(&o->shader, s->renderer, ow, oh);
  if (s->bg_rect)
    wlr_scene_rect_set_size(s->bg_rect, ow, oh);

  wlr_log(WLR_INFO, "new output: %s %dx%d", wlr_output->name, ow, oh);
  /* Apply any monitor config that was written via trixie.set("monitor", ...)
   * before wlr_backend_start enumerated outputs. */
  lua_apply_pending_monitor(s, o);
  lua_emit_screen_added(s, o);
}

/* ── Layer shell ────────────────────────────────────────────────────────────
 */
static void layer_surface_handle_map(struct wl_listener *listener, void *data) {
  (void)data;
  TrixieLayerSurface *ls = CONTAINER_OF(listener, TrixieLayerSurface, map);
  TrixieServer *s = ls->server;
  struct wlr_layer_surface_v1 *wls = ls->wlr_surface;
  int exclusive = wls->current.exclusive_zone;
  uint32_t anchor = wls->current.anchor;
  if (exclusive <= 0)
    return;
  bool at_top = (anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) &&
                !(anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM);
  bool at_bottom = (anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) &&
                   !(anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP);
  if (at_top || at_bottom)
    server_set_bar_inset(s, exclusive, at_bottom);
}
static void layer_surface_handle_unmap(struct wl_listener *listener,
                                       void *data) {
  (void)data;
  TrixieLayerSurface *ls = CONTAINER_OF(listener, TrixieLayerSurface, unmap);
  server_set_bar_inset(ls->server, 0, s_bar_bot_h > 0);
}
static void layer_surface_handle_destroy(struct wl_listener *listener,
                                         void *data) {
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
  struct wlr_scene_tree *layer_tree;
  switch (wls->pending.layer) {
  case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
    layer_tree = s->layer_background;
    break;
  case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
    layer_tree = s->layer_windows;
    break;
  default:
    layer_tree = s->layer_overlay;
    break;
  }
  TrixieLayerSurface *ls = calloc(1, sizeof(*ls));
  ls->server = s;
  ls->wlr_surface = wls;
  ls->scene_surface = wlr_scene_layer_surface_v1_create(layer_tree, wls);
  struct wlr_output *out =
      wlr_output_layout_output_at(s->output_layout, s->cursor->x, s->cursor->y);
  if (!out && !wl_list_empty(&s->outputs)) {
    TrixieOutput *o = wl_container_of(s->outputs.next, o, link);
    out = o->wlr_output;
  }
  if (out) {
    struct wlr_box ob;
    wlr_output_layout_get_box(s->output_layout, out, &ob);
    wlr_layer_surface_v1_configure(wls, (uint32_t)ob.width,
                                   (uint32_t)ob.height);
  }
  ls->map.notify = layer_surface_handle_map;
  ls->unmap.notify = layer_surface_handle_unmap;
  ls->destroy.notify = layer_surface_handle_destroy;
  wl_signal_add(&wls->surface->events.map, &ls->map);
  wl_signal_add(&wls->surface->events.unmap, &ls->unmap);
  wl_signal_add(&wls->events.destroy, &ls->destroy);
  wl_list_insert(&s->layer_surfaces, &ls->link);
}

/* ── XDG shell ──────────────────────────────────────────────────────────────
 */

static void view_do_map(TrixieServer *s, TrixieView *v, Pane *p,
                        const char *app_id, const char *title) {
  (void)v;
  lua_apply_window_rules(s, p, app_id, title);
  twm_reflow(&s->twm);
  struct wlr_scene_tree *tgt =
      (p->floating || p->fullscreen) ? s->layer_floating : s->layer_windows;
  wlr_scene_node_reparent(&v->scene_tree->node, tgt);
  wlr_scene_node_raise_to_top(&v->scene_tree->node);

  bool is_scratch = false;
  for (int i = 0; i < s->twm.scratch_count; i++)
    if (s->twm.scratchpads[i].has_pane &&
        s->twm.scratchpads[i].pane_id == v->pane_id) {
      is_scratch = true;
      break;
    }

  if (is_scratch)
    anim_scratch_open(&s->anim, v->pane_id, p->rect);
  else if (p->floating)
    anim_float_open(&s->anim, v->pane_id, p->rect);
  else
    anim_open(&s->anim, v->pane_id, p->rect);

  view_apply_geom(s, v, p);

  if (s->foreign_toplevel_mgr) {
    v->foreign_handle =
        wlr_foreign_toplevel_handle_v1_create(s->foreign_toplevel_mgr);
    v->foreign_request_activate.notify =
        handle_foreign_toplevel_request_activate;
    v->foreign_request_close.notify = handle_foreign_toplevel_request_close;
    wl_signal_add(&v->foreign_handle->events.request_activate,
                  &v->foreign_request_activate);
    wl_signal_add(&v->foreign_handle->events.request_close,
                  &v->foreign_request_close);
    view_foreign_toplevel_update(s, v);
  }
  server_focus_pane(s, v->pane_id);
  lua_emit_manage(s, v->pane_id);
  server_request_redraw(s);
}

static void view_handle_map(struct wl_listener *listener, void *data) {
  (void)data;
  TrixieView *v = CONTAINER_OF(listener, TrixieView, map);
  TrixieServer *s = v->server;
  v->mapped = true;
  Pane *p = twm_pane_by_id(&s->twm, v->pane_id);
  if (!p)
    return;
  const char *app_id = v->xdg_toplevel->app_id ? v->xdg_toplevel->app_id : "";
  const char *title = v->xdg_toplevel->title ? v->xdg_toplevel->title : "";
  view_do_map(s, v, p, app_id, title);
}

static void view_handle_unmap(struct wl_listener *listener, void *data) {
  (void)data;
  TrixieView *v = CONTAINER_OF(listener, TrixieView, unmap);
  TrixieServer *s = v->server;
  v->mapped = false;
  lua_emit_unmanage(s, v->pane_id);
  if (v->foreign_handle) {
    wlr_foreign_toplevel_handle_v1_destroy(v->foreign_handle);
    v->foreign_handle = NULL;
  }
  Pane *p = twm_pane_by_id(&s->twm, v->pane_id);
  if (p) {
    bool is_scratch = false;
    for (int i = 0; i < s->twm.scratch_count; i++)
      if (s->twm.scratchpads[i].has_pane &&
          s->twm.scratchpads[i].pane_id == v->pane_id) {
        is_scratch = true;
        break;
      }
    if (is_scratch)
      anim_scratch_close(&s->anim, v->pane_id, p->rect);
    else if (p->floating)
      anim_float_close(&s->anim, v->pane_id, p->rect);
    else
      anim_close(&s->anim, v->pane_id, p->rect);
  }
  TrixieView *next;
  wl_list_for_each(next, &s->views, link) if (next != v && next->mapped) {
    server_focus_pane(s, next->pane_id);
    break;
  }
}

static void view_handle_destroy(struct wl_listener *listener, void *data) {
  (void)data;
  TrixieView *v = CONTAINER_OF(listener, TrixieView, destroy);
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
  TrixieView *v = CONTAINER_OF(listener, TrixieView, commit);
  TrixieServer *s = v->server;
  if (s->fractional_scale_mgr && v->xdg_toplevel &&
      v->xdg_toplevel->base->surface) {
    TrixieOutput *o;
    wl_list_for_each(o, &s->outputs, link) wlr_fractional_scale_v1_notify_scale(
        v->xdg_toplevel->base->surface, o->wlr_output->scale);
  }
}

static void view_handle_request_fullscreen(struct wl_listener *listener,
                                           void *data) {
  (void)data;
  TrixieView *v = CONTAINER_OF(listener, TrixieView, request_fullscreen);
  Action a = {.kind = ACTION_FULLSCREEN};
  server_dispatch_action(v->server, &a);
}

static void view_handle_set_title(struct wl_listener *listener, void *data) {
  (void)data;
  TrixieView *v = CONTAINER_OF(listener, TrixieView, set_title);
  if (!v->xdg_toplevel || !v->xdg_toplevel->title)
    return;
  const char *new_title = v->xdg_toplevel->title;
  twm_set_title(&v->server->twm, v->pane_id, new_title);
  if (v->foreign_handle)
    wlr_foreign_toplevel_handle_v1_set_title(v->foreign_handle, new_title);
  twm_try_assign_scratch(&v->server->twm, v->pane_id, new_title);
  ipc_push_title_changed(v->server, v->pane_id);
  lua_emit_title_changed(v->server, v->pane_id);
  TrixieOutput *o;
  wl_list_for_each(o, &v->server->outputs, link) if (o->bar)
      bar_mark_dirty(o->bar);
}

static void view_handle_set_app_id(struct wl_listener *listener, void *data) {
  (void)data;
  TrixieView *v = CONTAINER_OF(listener, TrixieView, set_app_id);
  if (!v->xdg_toplevel || !v->xdg_toplevel->app_id)
    return;
  Pane *p = twm_pane_by_id(&v->server->twm, v->pane_id);
  if (p)
    strncpy(p->app_id, v->xdg_toplevel->app_id, sizeof(p->app_id) - 1);
  twm_try_assign_scratch(&v->server->twm, v->pane_id, v->xdg_toplevel->app_id);
  if (v->foreign_handle)
    wlr_foreign_toplevel_handle_v1_set_app_id(v->foreign_handle,
                                              v->xdg_toplevel->app_id);
}

static void handle_new_xdg_surface(struct wl_listener *listener, void *data) {
  TrixieServer *s = CONTAINER_OF(listener, TrixieServer, new_xdg_surface);
  struct wlr_xdg_toplevel *toplevel = data;
  struct wlr_xdg_surface *surface = toplevel->base;
  const char *app_id = toplevel->app_id ? toplevel->app_id : "";

  /* No C-level rule pre-pass: lua_apply_window_rules fires at map time */
  PaneId pane_id = twm_open(&s->twm, app_id);
  if (!pane_id) {
    wlr_xdg_toplevel_send_close(toplevel);
    return;
  }

  wlr_xdg_toplevel_set_wm_capabilities(
      toplevel, WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE |
                    WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN);
  wlr_xdg_toplevel_set_activated(toplevel, false);
  wlr_xdg_toplevel_set_tiled(toplevel, WLR_EDGE_TOP | WLR_EDGE_BOTTOM |
                                           WLR_EDGE_LEFT | WLR_EDGE_RIGHT);

  TrixieView *v = calloc(1, sizeof(*v));
  v->server = s;
  v->xdg_toplevel = toplevel;
  v->pane_id = pane_id;
#ifdef HAS_XWAYLAND
  v->is_xwayland = false;
  v->xwayland_surface = NULL;
#endif
  v->scene_tree = wlr_scene_xdg_surface_create(s->layer_windows, surface);
  wlr_scene_node_set_enabled(&v->scene_tree->node, false);

  v->map.notify = view_handle_map;
  v->unmap.notify = view_handle_unmap;
  v->destroy.notify = view_handle_destroy;
  v->commit.notify = view_handle_commit;
  v->request_fullscreen.notify = view_handle_request_fullscreen;
  v->set_title.notify = view_handle_set_title;
  v->set_app_id.notify = view_handle_set_app_id;

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

/* ── XWayland ───────────────────────────────────────────────────────────────
 */
#ifdef HAS_XWAYLAND
static void xwayland_handle_map(struct wl_listener *listener, void *data) {
  (void)data;
  TrixieView *v = CONTAINER_OF(listener, TrixieView, map);
  TrixieServer *s = v->server;
  v->mapped = true;
  struct wlr_xwayland_surface *xs = v->xwayland_surface;
  if (!xs->surface) {
    wlr_log(WLR_ERROR, "xwayland_handle_map: surface NULL for %s",
            xs->class ? xs->class : "(unknown)");
    v->mapped = false;
    return;
  }
  /* Ensure the wlr_surface is attached to our scene tree.  Only do this
   * once — if handle_new_xwayland_surface already created the node (when
   * xs->surface was non-NULL at that point) skip it.  Calling
   * wlr_scene_surface_create twice on the same tree creates a second
   * orphaned node and corrupts the scene graph, causing compositor crashes. */
  if (!v->xw_scene_attached) {
    wlr_scene_surface_create(v->scene_tree, xs->surface);
    v->xw_scene_attached = true;
  }
  /* Wire xs->surface->events.* here rather than in handle_new_xwayland_surface.
   * The surface is guaranteed live for the lifetime of this map. */
  if (!v->xw_commit_connected) {
    wl_signal_add(&xs->surface->events.map, &v->map);
    wl_signal_add(&xs->surface->events.unmap, &v->unmap);
    wl_signal_add(&xs->surface->events.commit, &v->commit);
    v->xw_commit_connected = true;
  }
  Pane *p = twm_pane_by_id(&s->twm, v->pane_id);
  if (!p)
    return;
  const char *app_id = xs->class ? xs->class : "";
  const char *title = xs->title ? xs->title : "";
  twm_set_title(&s->twm, v->pane_id, title);
  strncpy(p->app_id, app_id, sizeof(p->app_id) - 1);
  twm_try_assign_scratch(&s->twm, v->pane_id, app_id);
  if (xs->override_redirect) {
    wlr_scene_node_set_position(&v->scene_tree->node, xs->x, xs->y);
    wlr_scene_node_set_enabled(&v->scene_tree->node, true);
    return;
  }
  view_do_map(s, v, p, app_id, title);
}
static void xwayland_handle_unmap(struct wl_listener *listener, void *data) {
  (void)data;
  TrixieView *v = CONTAINER_OF(listener, TrixieView, unmap);
  TrixieServer *s = v->server;
  v->mapped = false;
  if (v->foreign_handle) {
    wlr_foreign_toplevel_handle_v1_destroy(v->foreign_handle);
    v->foreign_handle = NULL;
  }
  if (v->xwayland_surface->override_redirect) {
    wlr_scene_node_set_enabled(&v->scene_tree->node, false);
    return;
  }
  Pane *p = twm_pane_by_id(&s->twm, v->pane_id);
  if (p) {
    bool is_scratch = false;
    for (int i = 0; i < s->twm.scratch_count; i++)
      if (s->twm.scratchpads[i].has_pane &&
          s->twm.scratchpads[i].pane_id == v->pane_id) {
        is_scratch = true;
        break;
      }
    if (is_scratch)
      anim_scratch_close(&s->anim, v->pane_id, p->rect);
    else if (p->floating)
      anim_float_close(&s->anim, v->pane_id, p->rect);
    else
      anim_close(&s->anim, v->pane_id, p->rect);
  }
  TrixieView *next;
  wl_list_for_each(next, &s->views, link) if (next != v && next->mapped) {
    server_focus_pane(s, next->pane_id);
    break;
  }
}
static void xwayland_handle_destroy(struct wl_listener *listener, void *data) {
  (void)data;
  TrixieView *v = CONTAINER_OF(listener, TrixieView, destroy);
  TrixieServer *s = v->server;
  if (v->pane_id)
    twm_close(&s->twm, v->pane_id);
  wl_list_remove(&v->destroy.link);
  wl_list_remove(&v->request_fullscreen.link);
  wl_list_remove(&v->set_title.link);
  wl_list_remove(&v->set_app_id.link);
  if (v->xw_commit_connected) {
    wl_list_remove(&v->map.link);
    wl_list_remove(&v->unmap.link);
    wl_list_remove(&v->commit.link);
  }
  wl_list_remove(&v->link);
  free(v);
  server_request_redraw(s);
}
static void xwayland_handle_commit(struct wl_listener *listener, void *data) {
  (void)data;
  TrixieView *v = CONTAINER_OF(listener, TrixieView, commit);
  TrixieServer *s = v->server;
  if (s->fractional_scale_mgr && v->xwayland_surface &&
      v->xwayland_surface->surface) {
    TrixieOutput *o;
    wl_list_for_each(o, &s->outputs, link) wlr_fractional_scale_v1_notify_scale(
        v->xwayland_surface->surface, o->wlr_output->scale);
  }
}
static void xwayland_handle_request_fullscreen(struct wl_listener *listener,
                                               void *data) {
  (void)data;
  TrixieView *v = CONTAINER_OF(listener, TrixieView, request_fullscreen);
  Action a = {.kind = ACTION_FULLSCREEN};
  server_dispatch_action(v->server, &a);
}
static void xwayland_handle_set_title(struct wl_listener *listener,
                                      void *data) {
  (void)data;
  TrixieView *v = CONTAINER_OF(listener, TrixieView, set_title);
  const char *t = v->xwayland_surface->title ? v->xwayland_surface->title : "";
  twm_set_title(&v->server->twm, v->pane_id, t);
  if (v->foreign_handle)
    wlr_foreign_toplevel_handle_v1_set_title(v->foreign_handle, t);
  twm_try_assign_scratch(&v->server->twm, v->pane_id, t);
  TrixieOutput *o;
  wl_list_for_each(o, &v->server->outputs, link) if (o->bar)
      bar_mark_dirty(o->bar);
}
static void xwayland_handle_set_class(struct wl_listener *listener,
                                      void *data) {
  (void)data;
  TrixieView *v = CONTAINER_OF(listener, TrixieView, set_app_id);
  const char *c = v->xwayland_surface->class ? v->xwayland_surface->class : "";
  twm_try_assign_scratch(&v->server->twm, v->pane_id, c);
  if (v->foreign_handle)
    wlr_foreign_toplevel_handle_v1_set_app_id(v->foreign_handle, c);
}
static void handle_new_xwayland_surface(struct wl_listener *listener,
                                        void *data) {
  TrixieServer *s = CONTAINER_OF(listener, TrixieServer, new_xwayland_surface);
  struct wlr_xwayland_surface *xs = data;
  const char *class = xs->class ? xs->class : "";
  TrixieView *v = calloc(1, sizeof(*v));
  v->server = s;
  v->is_xwayland = true;
  v->xwayland_surface = xs;
  v->pane_id = xs->override_redirect ? 0 : twm_open(&s->twm, class);
  if (!v->pane_id && !xs->override_redirect) {
    free(v);
    wlr_xwayland_surface_close(xs);
    return;
  }
  v->scene_tree = xs->override_redirect
                      ? wlr_scene_tree_create(s->layer_floating)
                      : wlr_scene_tree_create(s->layer_windows);
  /* xs->surface may be NULL until the X11 client associates a surface.
   * Only create the scene surface node if it's already available here;
   * otherwise xwayland_handle_map will do it once surface is guaranteed. */
  if (xs->surface) {
    wlr_scene_surface_create(v->scene_tree, xs->surface);
    v->xw_scene_attached = true;
  }
  wlr_scene_node_set_enabled(&v->scene_tree->node, false);
  v->map.notify = xwayland_handle_map;
  v->unmap.notify = xwayland_handle_unmap;
  v->destroy.notify = xwayland_handle_destroy;
  v->commit.notify = xwayland_handle_commit;
  v->request_fullscreen.notify = xwayland_handle_request_fullscreen;
  v->set_title.notify = xwayland_handle_set_title;
  v->set_app_id.notify = xwayland_handle_set_class;
  wl_signal_add(&xs->events.destroy, &v->destroy);
  wl_signal_add(&xs->events.request_fullscreen, &v->request_fullscreen);
  wl_signal_add(&xs->events.set_title, &v->set_title);
  wl_signal_add(&xs->events.set_class, &v->set_app_id);
  /* Do NOT wire xs->surface->events.{map,unmap,commit} here.
   * Firefox creates a probe X11 window that is immediately destroyed via
   * XCB_DESTROY_NOTIFY before it ever maps.  Wlroots frees xs->surface
   * internally before our destroy handler runs, so any listeners on
   * xs->surface->events.* would be removed from a freed signal list,
   * corrupting the heap.  Wire these exclusively in xwayland_handle_map
   * where wlroots guarantees the surface is live (same pattern as sway). */
  wl_list_insert(&s->views, &v->link);
}
static void handle_xwayland_ready(struct wl_listener *listener, void *data) {
  (void)data;
  TrixieServer *s = CONTAINER_OF(listener, TrixieServer, xwayland_ready);
  wlr_log(WLR_INFO, "XWayland ready on %s", s->xwayland->display_name);
  setenv("DISPLAY", s->xwayland->display_name, true);
  if (s->cfg.cursor_theme[0])
    setenv("XCURSOR_THEME", s->cfg.cursor_theme, true);
  char sz[8];
  snprintf(sz, sizeof(sz), "%d",
           s->cfg.cursor_size > 0 ? s->cfg.cursor_size : 24);
  setenv("XCURSOR_SIZE", sz, true);
}
#endif

/* ── IPC socket ─────────────────────────────────────────────────────────────
 */
static const char *ipc_socket_path(void) {
  static char buf[256];
  if (buf[0])
    return buf;
  const char *xdg = getenv("XDG_RUNTIME_DIR");
  if (xdg)
    snprintf(buf, sizeof(buf), "%s/trixie.sock", xdg);
  else
    snprintf(buf, sizeof(buf), "/tmp/trixie-%d.sock", (int)getuid());
  return buf;
}

static void write_all_fd(int fd, const char *buf, size_t len) {
  while (len > 0) {
    ssize_t n = write(fd, buf, len);
    if (n <= 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    buf += n;
    len -= (size_t)n;
  }
}

static int ipc_read_cb(int fd, uint32_t mask, void *data) {
  (void)mask;
  TrixieServer *s = data;
  int client = accept(fd, NULL, NULL);
  if (client < 0)
    return 0;
  fcntl(client, F_SETFL, O_NONBLOCK);
  char buf[65536] = {0};
  size_t total = 0;
  ssize_t n;
  while (total < sizeof(buf) - 1) {
    n = read(client, buf + total, sizeof(buf) - 1 - total);
    if (n > 0) {
      total += (size_t)n;
      continue;
    }
    if (n < 0 && errno == EAGAIN)
      break;
    break;
  }
  if (total > 0) {
    char *nl = strchr(buf, '\n');
    if (nl)
      *nl = '\0';
    char *cmd = buf;
    while (*cmd == ' ' || *cmd == '\t')
      cmd++;
    if (!strncmp(cmd, "subscribe", 9) &&
        (cmd[9] == '\0' || cmd[9] == ' ' || cmd[9] == '\t')) {
      if (ipc_subscribe(s, client))
        return 0;
      write_all_fd(client, "err: subscriber table full\n", 27);
      close(client);
      return 0;
    }
    if (!strncmp(cmd, "binary_reload", 13) &&
        (cmd[13] == '\0' || cmd[13] == '\n')) {
      server_binary_reload(s);
      write_all_fd(client, "ok: building\n", 13);
      close(client);
      return 0;
    }
    char reply[65536] = {0};
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
  if (fd < 0)
    return;
  struct sockaddr_un addr = {0};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, ipc_socket_path(), sizeof(addr.sun_path) - 1);
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    return;
  }
  listen(fd, 8);
  fcntl(fd, F_SETFL, O_NONBLOCK);
  s->ipc_fd = fd;
  s->ipc_src = wl_event_loop_add_fd(wl_display_get_event_loop(s->display), fd,
                                    WL_EVENT_READABLE, ipc_read_cb, s);
  wlr_log(WLR_INFO, "IPC: %s", ipc_socket_path());
}

/* ── Config file watcher ────────────────────────────────────────────────────
 */
static int inotify_cb(int fd, uint32_t mask, void *data) {
  (void)mask;
  static int64_t last_reload_ms = 0;
  TrixieServer *s = data;
  char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
  ssize_t n = read(fd, buf, sizeof(buf));
  if (n <= 0)
    return 0;
  /* Only reload on .lua file changes */
  bool relevant = false;
  char *p = buf, *end = buf + n;
  while (p < end) {
    struct inotify_event *ev = (struct inotify_event *)p;
    if (ev->len > 0) {
      size_t nl = strlen(ev->name);
      if (nl >= 4 && strcmp(ev->name + nl - 4, ".lua") == 0) {
        relevant = true;
        break;
      }
    }
    p += sizeof(struct inotify_event) + ev->len;
  }
  if (!relevant)
    return 0;
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  int64_t now_ms = now.tv_sec * 1000 + now.tv_nsec / 1000000;
  if (now_ms - last_reload_ms < 500)
    return 0;
  last_reload_ms = now_ms;
  server_schedule_reload(s);
  return 0;
}

void server_init_config_watch(TrixieServer *s) {
  const char *xdg = getenv("XDG_CONFIG_HOME");
  char dir[256];
  if (xdg)
    snprintf(dir, sizeof(dir), "%s/trixie", xdg);
  else {
    const char *home = getenv("HOME");
    if (!home)
      home = "/root";
    snprintf(dir, sizeof(dir), "%s/.config/trixie", home);
  }
  s->inotify_fd = inotify_init1(IN_NONBLOCK);
  if (s->inotify_fd < 0)
    return;
  if (inotify_add_watch(s->inotify_fd, dir,
                        IN_MODIFY | IN_CREATE | IN_MOVED_TO) < 0) {
    close(s->inotify_fd);
    s->inotify_fd = -1;
    return;
  }
  s->inotify_src =
      wl_event_loop_add_fd(wl_display_get_event_loop(s->display), s->inotify_fd,
                           WL_EVENT_READABLE, inotify_cb, s);
  wlr_log(WLR_INFO, "watching %s", dir);
}

/* ── Crash handler ──────────────────────────────────────────────────────────
 */
static void crash_handler(int sig) {
  void *bt[64];
  int n = backtrace(bt, 64);
  fprintf(stderr, "\n=== CRASH sig=%d ===\n", sig);
  backtrace_symbols_fd(bt, n, STDERR_FILENO);
  fprintf(stderr, "===================\n");
  fflush(stderr);
  signal(sig, SIG_DFL);
  raise(sig);
}

/* ── main ───────────────────────────────────────────────────────────────────
 */
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
  s->running = true;
  s->ipc_fd = -1;
  s->inotify_fd = -1;
  s->reload_pid = -1;
  s->reload_pipe_fd = -1;
  s->saved_argv = argv;

  config_defaults(&s->cfg);

  /* TWM + animation init with C defaults; Lua overrides immediately after */
  twm_init(&s->twm, 1920, 1080, 0, true, s->cfg.gap, s->cfg.border_width,
           s->cfg.outer_gap, s->cfg.workspaces, s->cfg.smart_gaps);
  anim_set_resize(&s->anim, 1920, 1080);

  /* Display must exist before lua_init (trixie.timer needs event loop) */
  s->display = wl_display_create();

  /* lua_init runs init.lua — all trixie.set() writes land on the live TWM */
  lua_init(s);

  s->backend = wlr_backend_autocreate(wl_display_get_event_loop(s->display),
                                      &s->session);
  if (!s->backend) {
    wlr_log(WLR_ERROR, "failed to create backend");
    return 1;
  }

  s->renderer = wlr_renderer_autocreate(s->backend);
  wlr_renderer_init_wl_display(s->renderer, s->display);
  s->allocator = wlr_allocator_autocreate(s->backend, s->renderer);
  s->compositor = wlr_compositor_create(s->display, 6, s->renderer);
  wlr_subcompositor_create(s->display);
  wlr_data_device_manager_create(s->display);
  wlr_data_control_manager_v1_create(s->display);
  wlr_primary_selection_v1_device_manager_create(s->display);
  s->presentation = wlr_presentation_create(s->display, s->backend);
  wlr_viewporter_create(s->display);
  s->fractional_scale_mgr =
      wlr_fractional_scale_manager_v1_create(s->display, 1);
  wlr_single_pixel_buffer_manager_v1_create(s->display);

  s->cursor_shape_mgr = wlr_cursor_shape_manager_v1_create(s->display, 1);
  s->cursor_shape_request.notify = handle_cursor_shape_request;
  wl_signal_add(&s->cursor_shape_mgr->events.request_set_shape,
                &s->cursor_shape_request);

  s->xdg_activation = wlr_xdg_activation_v1_create(s->display);
  s->xdg_activation_request.notify = handle_xdg_activation;
  wl_signal_add(&s->xdg_activation->events.request_activate,
                &s->xdg_activation_request);

  s->pointer_constraints = wlr_pointer_constraints_v1_create(s->display);
  s->new_pointer_constraint.notify = handle_pointer_constraint;
  wl_signal_add(&s->pointer_constraints->events.new_constraint,
                &s->new_pointer_constraint);

  s->relative_pointer_mgr = wlr_relative_pointer_manager_v1_create(s->display);
  s->foreign_toplevel_mgr = wlr_foreign_toplevel_manager_v1_create(s->display);
  s->screencopy_mgr = wlr_screencopy_manager_v1_create(s->display);
  wlr_export_dmabuf_manager_v1_create(s->display);
  wlr_linux_dmabuf_v1_create_with_renderer(s->display, 5, s->renderer);
#ifdef HAVE_ALPHA_MODIFIER
  wlr_alpha_modifier_v1_create(s->display);
#endif
#ifdef HAVE_DRM_SYNCOBJ
  if (wlr_backend_is_drm(s->backend)) {
    int drm_fd = wlr_backend_get_drm_fd(s->backend);
    if (drm_fd >= 0)
      wlr_linux_drm_syncobj_manager_v1_create(s->display, 1, drm_fd);
  }
#endif
  wlr_gamma_control_manager_v1_create(s->display);

  s->idle_inhibit_mgr = wlr_idle_inhibit_v1_create(s->display);
  if (s->idle_inhibit_mgr) {
    s->idle_inhibit_new.notify = handle_new_idle_inhibitor;
    wl_signal_add(&s->idle_inhibit_mgr->events.new_inhibitor,
                  &s->idle_inhibit_new);
  }
  wlr_content_type_manager_v1_create(s->display, 1);
  wlr_tearing_control_manager_v1_create(s->display, 1);

  /* ── Additional protocols for full app compatibility ─────────────────────
   * text_input_v3: GTK4, Qt6, Electron keyboard input
   * input_method_v2: fcitx5, ibus CJK input
   * output_management_v1: kanshi, wdisplays monitor config
   * output_power_management_v1: DPMS via wlopm/swayidle
   * ext_idle_notify_v1: modern swayidle (zwp_idle is deprecated)
   * security_context_v1: sandboxed Flatpak/Electron access
   * virtual_keyboard_v1: ydotool, screen keyboards
   * virtual_pointer_v1: warpd, xdotool Wayland mode
   * session_lock_manager_v1: swaylock, hyprlock
   * xdg_foreign_v1/v2: GTK4 portal / cross-surface parenting */
#ifdef HAVE_TEXT_INPUT_V3
  wlr_text_input_manager_v3_create(s->display);
#endif
#ifdef HAVE_INPUT_METHOD_V2
  wlr_input_method_manager_v2_create(s->display);
#endif
#ifdef HAVE_OUTPUT_POWER_MGMT_V1
  wlr_output_power_manager_v1_create(s->display);
#endif
#ifdef HAVE_EXT_IDLE_NOTIFY_V1
  wlr_ext_idle_notifier_v1_create(s->display);
#endif
#ifdef HAVE_SECURITY_CONTEXT_V1
  wlr_security_context_manager_v1_create(s->display);
#endif
#ifdef HAVE_VIRTUAL_KEYBOARD_V1
  wlr_virtual_keyboard_manager_v1_create(s->display);
#endif
#ifdef HAVE_VIRTUAL_POINTER_V1
  wlr_virtual_pointer_manager_v1_create(s->display);
#endif
#ifdef HAVE_SESSION_LOCK_V1
  wlr_session_lock_manager_v1_create(s->display);
#endif
#ifdef HAVE_XDG_FOREIGN
  {
    struct wlr_xdg_foreign_registry *foreign_reg =
        wlr_xdg_foreign_registry_create(s->display);
    wlr_xdg_foreign_v1_create(s->display, foreign_reg);
    wlr_xdg_foreign_v2_create(s->display, foreign_reg);
  }
#endif

  s->output_layout = wlr_output_layout_create(s->display);
  s->scene = wlr_scene_create();
  s->scene_layout = wlr_scene_attach_output_layout(s->scene, s->output_layout);

  s->layer_background = wlr_scene_tree_create(&s->scene->tree);
  s->layer_windows = wlr_scene_tree_create(&s->scene->tree);
  s->layer_chrome = wlr_scene_tree_create(&s->scene->tree);
  s->layer_floating = wlr_scene_tree_create(&s->scene->tree);
  s->layer_chrome_floating = wlr_scene_tree_create(&s->scene->tree);
  s->layer_overlay = wlr_scene_tree_create(&s->scene->tree);

  s->reload_idle = NULL;

  {
    Color bc = s->cfg.colors.background;
    float fc[4] = {bc.r / 255.f, bc.g / 255.f, bc.b / 255.f, bc.a / 255.f};
    s->bg_rect = wlr_scene_rect_create(s->layer_background, 1920, 1080, fc);
    wlr_scene_node_set_position(&s->bg_rect->node, 0, 0);
  }

  s->xdg_shell = wlr_xdg_shell_create(s->display, 6);
  s->new_xdg_surface.notify = handle_new_xdg_surface;
  wl_signal_add(&s->xdg_shell->events.new_toplevel, &s->new_xdg_surface);
  s->new_xdg_popup.notify = handle_new_xdg_popup;
  wl_signal_add(&s->xdg_shell->events.new_popup, &s->new_xdg_popup);

  s->layer_shell = wlr_layer_shell_v1_create(s->display, 4);
  s->new_layer_surface.notify = handle_new_layer_surface;
  wl_signal_add(&s->layer_shell->events.new_surface, &s->new_layer_surface);

  s->output_mgr =
      wlr_xdg_output_manager_v1_create(s->display, s->output_layout);
#ifdef HAVE_OUTPUT_MANAGEMENT_V1
  wlr_output_manager_v1_create(s->display);
#endif

  s->deco_mgr = wlr_xdg_decoration_manager_v1_create(s->display);
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

  s->cursor_motion.notify = handle_cursor_motion;
  s->cursor_motion_abs.notify = handle_cursor_motion_abs;
  s->cursor_button.notify = handle_cursor_button;
  s->cursor_axis.notify = handle_cursor_axis;
  s->cursor_frame.notify = handle_cursor_frame;
  wl_signal_add(&s->cursor->events.motion, &s->cursor_motion);
  wl_signal_add(&s->cursor->events.motion_absolute, &s->cursor_motion_abs);
  wl_signal_add(&s->cursor->events.button, &s->cursor_button);
  wl_signal_add(&s->cursor->events.axis, &s->cursor_axis);
  wl_signal_add(&s->cursor->events.frame, &s->cursor_frame);

  s->pointer_gestures = wlr_pointer_gestures_v1_create(s->display);
  gesture_tracker_init(&s->gesture);

  s->seat = wlr_seat_create(s->display, s->cfg.seat_name);
  s->seat_request_cursor.notify = handle_request_cursor;
  s->seat_request_set_selection.notify = handle_request_set_selection;
  s->seat_request_set_primary_selection.notify =
      handle_request_set_primary_selection;
  s->seat_request_start_drag.notify = handle_request_start_drag;
  s->seat_start_drag.notify = handle_start_drag;
  wl_signal_add(&s->seat->events.request_set_cursor, &s->seat_request_cursor);
  wl_signal_add(&s->seat->events.request_set_selection,
                &s->seat_request_set_selection);
  wl_signal_add(&s->seat->events.request_set_primary_selection,
                &s->seat_request_set_primary_selection);
  wl_signal_add(&s->seat->events.request_start_drag,
                &s->seat_request_start_drag);
  wl_signal_add(&s->seat->events.start_drag, &s->seat_start_drag);

  s->new_output.notify = handle_new_output;
  wl_signal_add(&s->backend->events.new_output, &s->new_output);
  s->new_input.notify = handle_new_input;
  wl_signal_add(&s->backend->events.new_input, &s->new_input);

  const char *socket = wl_display_add_socket_auto(s->display);
  if (!socket) {
    wlr_log(WLR_ERROR, "failed to create Wayland socket");
    return 1;
  }

  /* Set env before wlr_backend_start so any process ly/systemd spawns on
   * socket publication already has the full Wayland environment. */
  setenv("WAYLAND_DISPLAY", socket, true);
  setenv("XDG_SESSION_TYPE", "wayland", true);
  setenv("XDG_CURRENT_DESKTOP", "trixie:wlroots", true);
  /* NVIDIA — needed by wlroots/GBM for buffer allocation.
   * These are intentionally NOT propagated to child processes:
   * server_spawn() unsets them before exec so Firefox/Electron don't
   * inherit them (GBM_BACKEND breaks client Mesa EGL, __GLX_VENDOR breaks
   * libGL dispatch in Wayland-native apps). */
  setenv("GBM_BACKEND", "nvidia-drm", false);
  setenv("__GLX_VENDOR_LIBRARY_NAME", "nvidia", false);
  setenv("EGL_PLATFORM", "wayland", true);
  /* wlroots driver hints — compositor-only, stripped in server_spawn. */
  setenv("WLR_NO_HARDWARE_CURSORS", "1", false);
  setenv("WLR_RENDERER", "gles2", false);
  setenv("WLR_DRM_DEVICES", "/dev/dri/card1", false);
  setenv("QT_QPA_PLATFORM", "wayland;xcb", true);
  setenv("QT_WAYLAND_DISABLE_WINDOWDECORATION", "1", true);
  setenv("QT_AUTO_SCREEN_SCALE_FACTOR", "1", false);
  setenv("GDK_BACKEND", "wayland,x11", true);
  setenv("SDL_VIDEODRIVER", "wayland,x11", true);
  setenv("_JAVA_AWT_WM_NONREPARENTING", "1", true);
  setenv("MOZ_ENABLE_WAYLAND", "1", true);
  setenv("MOZ_DBUS_REMOTE", "1", true);
  setenv("MOZ_USE_XINPUT2", "1", false);
  /* MOZ_X11_EGL caused GPU process to use X11 EGL path instead of Wayland
   * EGL on Nvidia, leaving zwp_linux_dmabuf proxies dangling when the GPU
   * process failed. Removed in favour of the correct Nvidia+Wayland vars. */
  /* MOZ_DISABLE_RDD_SANDBOX: prevents the RDD (Remote Data Decoder) sandbox
   * from breaking VA-API/NVDEC hardware decode under Wayland on Nvidia. */
  setenv("MOZ_DISABLE_RDD_SANDBOX", "1", false);
  /* Tell Firefox's GPU process to use the Wayland EGL platform explicitly. */
  setenv("MOZ_WEBRENDER", "1", false);
  setenv("MOZ_ACCELERATED", "1", false);
  setenv("OZONE_PLATFORM", "wayland", true);
  setenv("ELECTRON_OZONE_PLATFORM_HINT", "wayland", true);
  setenv("NIXOS_OZONE_WL", "1", true);
  setenv("WLR_NO_HARDWARE_CURSORS", "1", false);

  wlr_log(WLR_INFO, "WAYLAND_DISPLAY=%s", socket);

  if (!wlr_backend_start(s->backend)) {
    wlr_log(WLR_ERROR, "failed to start backend");
    return 1;
  }
  g_server = s;

  server_init_ipc(s);
  server_init_config_watch(s);

  if (s->cfg.idle_timeout > 0) {
    s->idle_timeout_ms = s->cfg.idle_timeout * 1000;
    s->idle_timer = wl_event_loop_add_timer(
        wl_display_get_event_loop(s->display), idle_timer_cb, s);
    wl_event_source_timer_update(s->idle_timer, s->idle_timeout_ms);
  }

#ifdef HAS_XWAYLAND
  if (s->cfg.xwayland) {
    s->xwayland = wlr_xwayland_create(s->display, s->compositor, false);
    if (s->xwayland) {
      s->xwayland_ready.notify = handle_xwayland_ready;
      s->new_xwayland_surface.notify = handle_new_xwayland_surface;
      wl_signal_add(&s->xwayland->events.ready, &s->xwayland_ready);
      wl_signal_add(&s->xwayland->events.new_surface, &s->new_xwayland_surface);
      setenv("DISPLAY", s->xwayland->display_name, true);
    }
  }
#endif

  lua_emit_startup(s);
  wl_display_run(s->display);

  /* Shutdown */
  wlr_log(WLR_INFO, "trixie shutting down");
  lua_destroy(s);
  if (s->idle_timer)
    wl_event_source_remove(s->idle_timer);
  if (s->ipc_src)
    wl_event_source_remove(s->ipc_src);
  if (s->inotify_src)
    wl_event_source_remove(s->inotify_src);
  if (s->ipc_fd >= 0) {
    close(s->ipc_fd);
    unlink(ipc_socket_path());
  }
  if (s->reload_idle)
    wl_event_source_remove(s->reload_idle);
  if (s->inotify_fd >= 0)
    close(s->inotify_fd);
#ifdef HAS_XWAYLAND
  if (s->xwayland)
    wlr_xwayland_destroy(s->xwayland);
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
