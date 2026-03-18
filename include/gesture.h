/* gesture.h — pointer gesture recognition (swipe + pinch)
 *
 * Mirrors hyprland's gesture model:
 *   - 3/4-finger swipe → workspace prev/next, or custom action
 *   - 2-finger pinch   → toggle float / fullscreen
 *   - Per-direction threshold, configurable finger count
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* ── Config ──────────────────────────────────────────────────────────────── */

#define GESTURE_SWIPE_THRESHOLD 200.0f /* px total delta to trigger     */
#define GESTURE_PINCH_THRESHOLD 0.4f   /* scale delta (|1 - scale|)     */
#define GESTURE_MAX_BINDS       32

typedef enum {
  GESTURE_SWIPE_LEFT  = 0,
  GESTURE_SWIPE_RIGHT = 1,
  GESTURE_SWIPE_UP    = 2,
  GESTURE_SWIPE_DOWN  = 3,
  GESTURE_PINCH_IN    = 4,
  GESTURE_PINCH_OUT   = 5,
} GestureKind;

typedef struct {
  GestureKind kind;
  int         fingers; /* 2, 3, or 4 */
  /* Action payload — mirrors Action in trixie.h */
  int         action_kind; /* ACTION_* constant           */
  int         action_n;    /* workspace number if needed  */
  char        action_cmd[128];
} GestureBind;

typedef struct {
  GestureBind binds[GESTURE_MAX_BINDS];
  int         bind_count;
  float       swipe_threshold;
  float       pinch_threshold;
} GestureConfig;

/* ── Runtime state ───────────────────────────────────────────────────────── */

typedef enum {
  GSTATE_IDLE,
  GSTATE_SWIPE,
  GSTATE_PINCH,
} GestureState;

typedef struct {
  GestureState state;
  int          fingers;
  /* Swipe accumulator */
  double       dx, dy;
  bool         triggered; /* fired once per gesture to avoid repeat */
  /* Pinch accumulator */
  double       scale_accum; /* cumulative (scale - 1.0) since begin */
} GestureTracker;

/* ── API ─────────────────────────────────────────────────────────────────── */

void gesture_tracker_init(GestureTracker *g);

/* Feed raw wlroots gesture events; returns true if a bind was fired.
 * server is passed opaque (void*) to avoid a circular header dep — cast
 * to TrixieServer* inside gesture.c. */
bool gesture_swipe_begin(GestureTracker      *g,
                         const GestureConfig *cfg,
                         void                *server,
                         int                  fingers);
bool gesture_swipe_update(
    GestureTracker *g, const GestureConfig *cfg, void *server, double dx, double dy);
bool gesture_swipe_end(GestureTracker      *g,
                       const GestureConfig *cfg,
                       void                *server,
                       bool                 cancelled);

bool gesture_pinch_begin(GestureTracker      *g,
                         const GestureConfig *cfg,
                         void                *server,
                         int                  fingers);
bool gesture_pinch_update(GestureTracker      *g,
                          const GestureConfig *cfg,
                          void                *server,
                          double               scale);
bool gesture_pinch_end(GestureTracker      *g,
                       const GestureConfig *cfg,
                       void                *server,
                       bool                 cancelled);

/* Parse a gesture bind line from trixie.conf:
 *   gesture = swipe:3:left, prev_workspace
 *   gesture = swipe:4:right, next_workspace
 *   gesture = pinch:2:in, fullscreen
 * Returns true on success. */
bool gesture_parse_bind(GestureConfig *cfg, const char *line);
