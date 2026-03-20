/* shadow.h — 9-slice drop shadow system using wlr_scene_buffer.
 *
 * Usage:
 *   TrixieShadow *sh = shadow_create(parent_tree, &cfg->shadow, w, h);
 *   // on window resize:
 *   shadow_resize(sh, new_w, new_h);
 *   // on workspace hide:
 *   shadow_set_enabled(sh, false);
 *   // on destroy:
 *   shadow_destroy(sh);
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

struct wlr_scene_tree;

/* Full struct exposed so deco.c can access ->tree without a cast. */
typedef struct TrixieShadow {
  struct wlr_scene_tree *tree;
  struct wlr_scene_buffer *bufs[8]; /* S_COUNT slices */
  int radius;
  int offset_x, offset_y;
  float opacity;
  uint8_t cr, cg, cb;
  int win_w, win_h;
} TrixieShadow;

/* ShadowCfg is defined in trixie.h. shadow.h only uses a forward reference
 * via void* so we never need to see the full definition here. */

/* Create a shadow attached to parent_tree, sized for a window of (w × h).
 * cfg must be a const ShadowCfg* cast to const void*. */
TrixieShadow *shadow_create(struct wlr_scene_tree *parent, const void *cfg,
                            int w, int h);

/* Resize the shadow (call after the window geometry changes). */
void shadow_resize(TrixieShadow *s, int w, int h);

/* Show / hide without destroying scene nodes. */
void shadow_set_enabled(TrixieShadow *s, bool enabled);

/* Fully destroy and free the shadow (removes scene nodes). */
void shadow_destroy(TrixieShadow *s);
