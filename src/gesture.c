/* gesture.c — pointer gesture recognition */
#include "gesture.h"
#include "trixie.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Helpers ──────────────────────────────────────────────────────────────── */

static void fire_bind(void *server, const GestureBind *b) {
  TrixieServer *s = server;
  Action        a = { .kind = b->action_kind, .n = b->action_n };
  if(b->action_kind == ACTION_EXEC)
    snprintf(a.exec_cmd, sizeof(a.exec_cmd), "%s", b->action_cmd);
  server_dispatch_action(s, &a);
}

static bool try_fire(GestureTracker      *g,
                     const GestureConfig *cfg,
                     void                *server,
                     GestureKind          kind) {
  if(g->triggered) return false;
  for(int i = 0; i < cfg->bind_count; i++) {
    const GestureBind *b = &cfg->binds[i];
    if(b->kind == kind && b->fingers == g->fingers) {
      fire_bind(server, b);
      g->triggered = true;
      return true;
    }
  }
  return false;
}

/* ── Init ─────────────────────────────────────────────────────────────────── */

void gesture_tracker_init(GestureTracker *g) {
  memset(g, 0, sizeof(*g));
  g->state = GSTATE_IDLE;
}

/* ── Swipe ────────────────────────────────────────────────────────────────── */

bool gesture_swipe_begin(GestureTracker      *g,
                         const GestureConfig *cfg,
                         void                *server,
                         int                  fingers) {
  (void)cfg;
  (void)server;
  g->state     = GSTATE_SWIPE;
  g->fingers   = fingers;
  g->dx        = 0.0;
  g->dy        = 0.0;
  g->triggered = false;
  return false;
}

bool gesture_swipe_update(GestureTracker      *g,
                          const GestureConfig *cfg,
                          void                *server,
                          double               dx,
                          double               dy) {
  if(g->state != GSTATE_SWIPE) return false;
  g->dx += dx;
  g->dy += dy;

  float thr =
      cfg->swipe_threshold > 0.0f ? cfg->swipe_threshold : GESTURE_SWIPE_THRESHOLD;

  /* Determine dominant axis and check threshold. */
  if(fabs(g->dx) >= thr && fabs(g->dx) > fabs(g->dy))
    return try_fire(
        g, cfg, server, g->dx < 0 ? GESTURE_SWIPE_LEFT : GESTURE_SWIPE_RIGHT);
  if(fabs(g->dy) >= thr && fabs(g->dy) > fabs(g->dx))
    return try_fire(
        g, cfg, server, g->dy < 0 ? GESTURE_SWIPE_UP : GESTURE_SWIPE_DOWN);
  return false;
}

bool gesture_swipe_end(GestureTracker      *g,
                       const GestureConfig *cfg,
                       void                *server,
                       bool                 cancelled) {
  (void)cfg;
  (void)server;
  bool fired   = g->triggered && !cancelled;
  g->state     = GSTATE_IDLE;
  g->triggered = false;
  g->dx = g->dy = 0.0;
  return fired;
}

/* ── Pinch ────────────────────────────────────────────────────────────────── */

bool gesture_pinch_begin(GestureTracker      *g,
                         const GestureConfig *cfg,
                         void                *server,
                         int                  fingers) {
  (void)cfg;
  (void)server;
  g->state       = GSTATE_PINCH;
  g->fingers     = fingers;
  g->scale_accum = 0.0;
  g->triggered   = false;
  return false;
}

bool gesture_pinch_update(GestureTracker      *g,
                          const GestureConfig *cfg,
                          void                *server,
                          double               scale) {
  if(g->state != GSTATE_PINCH) return false;
  /* scale from wlroots is the cumulative scale factor since begin.
   * Convert to "how far from 1.0" as a simple signed accumulator. */
  g->scale_accum = scale - 1.0;

  float thr =
      cfg->pinch_threshold > 0.0f ? cfg->pinch_threshold : GESTURE_PINCH_THRESHOLD;

  if(g->scale_accum <= -thr) return try_fire(g, cfg, server, GESTURE_PINCH_IN);
  if(g->scale_accum >= thr) return try_fire(g, cfg, server, GESTURE_PINCH_OUT);
  return false;
}

bool gesture_pinch_end(GestureTracker      *g,
                       const GestureConfig *cfg,
                       void                *server,
                       bool                 cancelled) {
  (void)cfg;
  (void)server;
  bool fired     = g->triggered && !cancelled;
  g->state       = GSTATE_IDLE;
  g->triggered   = false;
  g->scale_accum = 0.0;
  return fired;
}

/* ── Config parser ────────────────────────────────────────────────────────── */

/* Map action name string → ACTION_* int.
 * Extend as needed to cover all Action kinds. */
static int parse_action_kind(const char *s, int *n_out, char *cmd_out, int cmd_sz) {
  *n_out = 0;
  if(!strcmp(s, "prev_workspace")) {
    *n_out = 0;
    return ACTION_PREV_WS;
  }
  if(!strcmp(s, "next_workspace")) {
    *n_out = 0;
    return ACTION_NEXT_WS;
  }
  if(!strcmp(s, "fullscreen")) return ACTION_FULLSCREEN;
  if(!strcmp(s, "toggle_float")) return ACTION_TOGGLE_FLOAT;
  if(!strcmp(s, "close")) return ACTION_CLOSE;
  if(!strcmp(s, "toggle_bar")) return ACTION_TOGGLE_BAR;
  if(!strcmp(s, "swap_main")) return ACTION_SWAP_MAIN;
  if(!strncmp(s, "workspace:", 10)) {
    *n_out = atoi(s + 10);
    return ACTION_WORKSPACE;
  }
  if(!strncmp(s, "exec:", 5)) {
    snprintf(cmd_out, cmd_sz, "%s", s + 5);
    return ACTION_EXEC;
  }
  return -1;
}

bool gesture_parse_bind(GestureConfig *cfg, const char *line) {
  if(cfg->bind_count >= GESTURE_MAX_BINDS) return false;

  /* Expected format: "type:fingers:direction, action"
   * e.g. "swipe:3:left, prev_workspace" */
  char lhs[64] = { 0 }, rhs[128] = { 0 };
  if(sscanf(line, " %63[^,], %127[^\n]", lhs, rhs) != 2) return false;

  /* Trim trailing whitespace on rhs */
  int rlen = (int)strlen(rhs);
  while(rlen > 0 &&
        (rhs[rlen - 1] == ' ' || rhs[rlen - 1] == '\t' || rhs[rlen - 1] == '\r'))
    rhs[--rlen] = '\0';

  char type[16] = { 0 };
  int  fingers  = 3;
  char dir[16]  = { 0 };
  if(sscanf(lhs, "%15[^:]:%d:%15s", type, &fingers, dir) < 2) return false;

  GestureBind *b = &cfg->binds[cfg->bind_count];
  memset(b, 0, sizeof(*b));
  b->fingers = fingers;

  /* Resolve kind */
  if(!strcmp(type, "swipe")) {
    if(!strcmp(dir, "left"))
      b->kind = GESTURE_SWIPE_LEFT;
    else if(!strcmp(dir, "right"))
      b->kind = GESTURE_SWIPE_RIGHT;
    else if(!strcmp(dir, "up"))
      b->kind = GESTURE_SWIPE_UP;
    else if(!strcmp(dir, "down"))
      b->kind = GESTURE_SWIPE_DOWN;
    else
      return false;
  } else if(!strcmp(type, "pinch")) {
    if(!strcmp(dir, "in"))
      b->kind = GESTURE_PINCH_IN;
    else if(!strcmp(dir, "out"))
      b->kind = GESTURE_PINCH_OUT;
    else
      return false;
  } else {
    return false;
  }

  b->action_kind =
      parse_action_kind(rhs, &b->action_n, b->action_cmd, sizeof(b->action_cmd));
  if(b->action_kind < 0) return false;

  cfg->bind_count++;
  return true;
}
